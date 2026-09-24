#include "diagnostics_qt.h"
#include "notifications.h"
#include "../sdk/xovi-notifications.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <map>
#include <vector>
#include <atomic>
#include <QCoreApplication>
#include <QThread>
#include <QMetaObject>

namespace {
constexpr int kMaxNotifications = 100;
constexpr int kMaxRequestBytes = 16 * 1024;
constexpr int kMaxTitleBytes = 256;
constexpr int kMaxMessageBytes = 4096;
constexpr int kMaxPageBytes = 128;
constexpr int kMaxActionLabelBytes = 128;

std::mutex notificationsMutex;
QJsonArray notifications;
QJsonArray pendingActions;
quint64 nextSequence = 1;
quint64 revision = 0;
quint64 nextToastRevision = 1;
quint64 nextActionSequence = 1;
struct Subscription {
    QString owner;
    XemNotificationCallback callback;
    void *userData;
    bool active = true;
};
std::map<uint64_t, std::shared_ptr<Subscription>> subscriptions;
uint64_t nextSubscription = 1;
std::atomic_bool dispatchPending{false};
void scheduleDispatch();
bool applicationThread() {
    auto *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

QJsonObject failure(const char *code) { return withDiagnostic({{"ok", false}, {"error", code}}); }
bool validId(const QString &value) {
    return QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$").match(value).hasMatch();
}
bool utf8Within(const QString &value, int maximum) { return value.toUtf8().size() <= maximum; }
bool knownLevel(const QString &level) { return level == "info" || level == "warning" || level == "error"; }
bool knownState(const QString &state) {
    return state == "running" || state == "completed" || state == "failed" || state == "cancelled";
}
std::string encode(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}
char *owned(const std::string &value) {
    auto *out = static_cast<char *>(std::malloc(value.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}
int indexOf(const QString &owner, const QString &id) {
    for (int i = 0; i < notifications.size(); ++i) {
        const auto entry = notifications.at(i).toObject();
        if (entry.value("ownerId").toString() == owner && entry.value("notificationId").toString() == id)
            return i;
    }
    return -1;
}
QJsonObject requireKey(const QJsonObject &request, QString *owner, QString *id) {
    if (!request.value("ownerId").isString()) return failure("invalid-owner-id");
    if (!request.value("notificationId").isString()) return failure("invalid-notification-id");
    *owner = request.value("ownerId").toString();
    *id = request.value("notificationId").toString();
    if (!validId(*owner)) return failure("invalid-owner-id");
    if (!validId(*id)) return failure("invalid-notification-id");
    return {};
}
QJsonObject requireOwner(const QJsonObject &request, QString *owner) {
    if (!request.value("ownerId").isString()) return failure("invalid-owner-id");
    *owner = request.value("ownerId").toString();
    return validId(*owner) ? QJsonObject{} : failure("invalid-owner-id");
}
QJsonObject validateProgress(const QJsonValue &value, QJsonObject *progress) {
    if (value.isNull()) { *progress = {}; return {}; }
    if (!value.isObject()) return failure("invalid-progress");
    const auto object = value.toObject();
    if (object.contains("indeterminate") && !object.value("indeterminate").isBool()) return failure("invalid-progress");
    const bool indeterminate = object.value("indeterminate").toBool(false);
    if (!object.contains("value") && !indeterminate) return failure("invalid-progress");
    if (object.contains("value")) {
        const auto number = object.value("value");
        const double progressValue = number.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!number.isDouble() || !std::isfinite(progressValue) || progressValue < 0.0 || progressValue > 1.0)
            return failure("invalid-progress");
        progress->insert("value", progressValue);
    }
    if (indeterminate) progress->insert("indeterminate", true);
    return {};
}
QJsonObject validateActions(const QJsonValue &value, QJsonArray *actions) {
    if (!value.isArray() || value.toArray().size() > 2) return failure("invalid-actions");
    QJsonArray validated;
    for (const auto &candidate : value.toArray()) {
        if (!candidate.isObject()) return failure("invalid-actions");
        const auto action = candidate.toObject();
        if (!action.value("id").isString() || !action.value("label").isString()) return failure("invalid-actions");
        const auto id = action.value("id").toString();
        const auto label = action.value("label").toString();
        if (!validId(id) || label.isEmpty() || !utf8Within(label, kMaxActionLabelBytes)) return failure("invalid-actions");
        for (const auto &previous : validated)
            if (previous.toObject().value("id").toString() == id) return failure("invalid-actions");
        validated.append(QJsonObject{{"id", id}, {"label", label}});
    }
    *actions = validated;
    return {};
}
void removePending(const QString &owner, const QString &id = {}) {
    QJsonArray kept;
    for (const auto &value : pendingActions) {
        const auto action = value.toObject();
        if (action.value("ownerId").toString() == owner && (id.isEmpty() || action.value("notificationId").toString() == id))
            continue;
        kept.append(action);
    }
    pendingActions = kept;
}
QJsonObject post(const QJsonObject &request) {
    QString owner, id;
    const auto keyError = requireKey(request, &owner, &id);
    if (!keyError.isEmpty()) return keyError;
    const int existing = indexOf(owner, id);
    const auto previous = existing >= 0 ? notifications.at(existing).toObject() : QJsonObject{};
    const bool patch = existing >= 0 && (request.contains("progress") || request.contains("state"));
    if ((existing < 0 || request.contains("title")) && !request.value("title").isString()) return failure("invalid-title");
    if ((existing < 0 || request.contains("message")) && !request.value("message").isString()) return failure("invalid-message");
    const QString title = request.contains("title") ? request.value("title").toString() : previous.value("title").toString();
    const QString message = request.contains("message") ? request.value("message").toString() : previous.value("message").toString();
    if (request.contains("pageId") && !request.value("pageId").isString()) return failure("invalid-page-id");
    if (request.contains("level") && !request.value("level").isString()) return failure("invalid-level");
    if (request.contains("state") && !request.value("state").isString()) return failure("invalid-state");
    const QString page = request.contains("pageId") ? request.value("pageId").toString() : previous.value("pageId").toString();
    const QString level = request.contains("level") ? request.value("level").toString() : previous.value("level").toString("info");
    const QString priorState = previous.value("state").toString("completed");
    const QString state = request.contains("state") ? request.value("state").toString()
        : (existing >= 0 ? priorState : (request.contains("progress") ? "running" : "completed"));
    if (title.isEmpty() || !utf8Within(title, kMaxTitleBytes)) return failure("invalid-title");
    if (message.isEmpty() || !utf8Within(message, kMaxMessageBytes)) return failure("invalid-message");
    if (!page.isEmpty() && (!validId(page) || !utf8Within(page, kMaxPageBytes))) return failure("invalid-page-id");
    if (!knownLevel(level)) return failure("invalid-level");
    if (!knownState(state)) return failure("invalid-state");

    QJsonObject progress = previous.value("progress").toObject();
    if (request.contains("progress")) {
        const auto progressError = validateProgress(request.value("progress"), &progress);
        if (!progressError.isEmpty()) return progressError;
    }
    QJsonArray actions = previous.value("actions").toArray();
    if (request.contains("actions")) {
        const auto actionsError = validateActions(request.value("actions"), &actions);
        if (!actionsError.isEmpty()) return actionsError;
    }

    const bool terminalTransition = patch && priorState == "running" && state != "running";
    if (terminalTransition && !request.contains("actions")) actions = {};
    const bool toast = existing < 0 || !patch || terminalTransition;
    const bool unread = terminalTransition || !patch ? false : previous.value("read").toBool(false);
    const qint64 sequence = patch ? previous.value("sequence").toInteger()
                                  : static_cast<qint64>(nextSequence++);
    const qint64 toastRevision = toast ? static_cast<qint64>(nextToastRevision++)
                                       : previous.value("toastRevision").toInteger();

    QJsonObject entry{{"ownerId", owner}, {"notificationId", id}, {"title", title},
                      {"message", message}, {"pageId", page}, {"level", level},
                      {"state", state}, {"actions", actions},
                      {"progress", progress.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(progress)},
                      {"pendingActionIds", previous.value("pendingActionIds").toArray()},
                      {"read", unread}, {"count", patch ? previous.value("count").toInt(1)
                                                           : previous.value("count").toInt(0) + 1},
                      {"sequence", sequence}, {"toastRevision", toastRevision}};
    if (request.contains("actions")) {
        QJsonArray kept, pendingIds;
        for (const auto &value : pendingActions) {
            const auto queued = value.toObject();
            if (queued.value("ownerId").toString() != owner || queued.value("notificationId").toString() != id) {
                kept.append(queued);
                continue;
            }
            bool stillAvailable = false;
            for (const auto &action : actions)
                if (action.toObject().value("id").toString() == queued.value("actionId").toString()) {
                    stillAvailable = true;
                    break;
                }
            if (stillAvailable) {
                kept.append(queued);
                if (queued.value("status")=="queued" || queued.value("status")=="delivered")
                    pendingIds.append(queued.value("actionId"));
            }
        }
        pendingActions = kept;
        entry.insert("pendingActionIds", pendingIds);
    }
    if (existing >= 0 && !patch) {
        // A legacy/full replacement is a new notification generation. Pending
        // clicks for the old sequence must never be delivered to it.
        removePending(owner, id);
        entry.insert("pendingActionIds", QJsonArray{});
    }
    if (terminalTransition && !request.contains("actions")) {
        // Completion normally retires running actions. A producer that still
        // needs an action must explicitly publish it with the terminal state.
        removePending(owner, id);
        entry.insert("pendingActionIds", QJsonArray{});
    }
    if (existing >= 0 && patch)
        notifications.replace(existing, entry);
    else {
        if (existing >= 0) notifications.removeAt(existing);
        notifications.prepend(entry);
    }
    while (notifications.size() > kMaxNotifications) {
        const auto evicted = notifications.last().toObject();
        removePending(evicted.value("ownerId").toString(), evicted.value("notificationId").toString());
        notifications.removeLast();
    }
    ++revision;
    return {{"ok", true}, {"status", "queued"}, {"notification", entry},
            {"revision", static_cast<qint64>(revision)}};
}
QJsonObject dismiss(const QJsonObject &request) {
    QString owner, id;
    const auto keyError = requireKey(request, &owner, &id);
    if (!keyError.isEmpty()) return keyError;
    const int index = indexOf(owner, id);
    if (index < 0) return failure("not-found");
    notifications.removeAt(index);
    removePending(owner, id);
    ++revision;
    return {{"ok", true}, {"status", "dismissed"}, {"revision", static_cast<qint64>(revision)}};
}
QJsonObject markRead(const QJsonObject &request) {
    QString owner, id;
    const auto keyError = requireKey(request, &owner, &id);
    if (!keyError.isEmpty()) return keyError;
    const int index = indexOf(owner, id);
    if (index < 0) return failure("not-found");
    auto entry = notifications.at(index).toObject();
    entry.insert("read", true);
    notifications.replace(index, entry);
    ++revision;
    return {{"ok", true}, {"status", "read"}, {"revision", static_cast<qint64>(revision)}};
}
QJsonObject list(const QJsonObject &request) {
    if (request.contains("ownerId") && !request.value("ownerId").isString()) return failure("invalid-owner-id");
    const QString owner = request.value("ownerId").toString();
    if (!owner.isEmpty() && !validId(owner)) return failure("invalid-owner-id");
    if (request.contains("sinceRevision")) {
        const auto value=request.value("sinceRevision");
        if(!value.isDouble() || value.toDouble()<0 || value.toDouble()!=static_cast<double>(value.toInteger(-1))) return failure("invalid-revision");
        if(static_cast<quint64>(value.toInteger())==revision) return {{"ok",true},{"unchanged",true},{"revision",static_cast<qint64>(revision)}};
    }
    QJsonArray result;
    int unread = 0;
    for (const auto &value : notifications) {
        const auto entry = value.toObject();
        if (owner.isEmpty() || entry.value("ownerId").toString() == owner) {
            result.append(entry);
            if (!entry.value("read").toBool()) ++unread;
        }
    }
    QJsonArray actions;
    if (!owner.isEmpty()) for (const auto &value : pendingActions)
        if (value.toObject().value("ownerId").toString() == owner) actions.append(value);
    return {{"actions", actions}, {"ok", true}, {"entries", result}, {"notifications", result}, {"unread", unread},
            {"revision", static_cast<qint64>(revision)}, {"storage", "memory"}};
}
QJsonObject clear(const QJsonObject &request) {
    if (!request.value("ownerId").isString()) return failure("invalid-owner-id");
    const QString owner = request.value("ownerId").toString();
    if (!validId(owner)) return failure("invalid-owner-id");
    QJsonArray kept;
    int removed = 0;
    for (const auto &value : notifications) {
        if (value.toObject().value("ownerId").toString() == owner) ++removed;
        else kept.append(value);
    }
    notifications = kept;
    removePending(owner);
    if (removed) ++revision;
    return {{"ok", true}, {"status", "cleared"}, {"removed", removed},
            {"revision", static_cast<qint64>(revision)}};
}
QJsonObject actionInvoke(const QJsonObject &request) {
    QString owner, id;
    const auto keyError = requireKey(request, &owner, &id);
    if (!keyError.isEmpty()) return keyError;
    if (!request.value("actionId").isString() || !validId(request.value("actionId").toString()))
        return failure("invalid-action-id");
    const int index = indexOf(owner, id);
    if (index < 0) return failure("not-found");
    auto entry = notifications.at(index).toObject();
    if (request.contains("sequence")) {
        const auto supplied = request.value("sequence");
        if (!supplied.isDouble() || supplied.toInteger(-1) < 1) return failure("invalid-sequence");
        if (supplied.toInteger() != entry.value("sequence").toInteger()) return failure("stale-notification");
    }
    const QString actionId = request.value("actionId").toString();
    bool available = false;
    for (const auto &value : entry.value("actions").toArray())
        if (value.toObject().value("id").toString() == actionId) { available = true; break; }
    if (!available) return failure("action-not-available");
    for (const auto &value : pendingActions) {
        const auto queued = value.toObject();
        if (queued.value("ownerId").toString() == owner && queued.value("notificationId").toString() == id &&
            queued.value("actionId").toString() == actionId &&
            (queued.value("status")=="queued" || queued.value("status")=="delivered"))
            return failure("action-already-pending");
    }
    while (pendingActions.size() >= kMaxNotifications) {
        int finished = -1;
        for (int i=0; i<pendingActions.size(); ++i) {
            const auto status=pendingActions.at(i).toObject().value("status").toString();
            if(status=="completed" || status=="failed") { finished=i; break; }
        }
        if(finished<0) return failure("action-queue-full");
        pendingActions.removeAt(finished);
    }
    pendingActions.append(QJsonObject{{"status", "queued"}, {"ownerId", owner}, {"notificationId", id}, {"actionId", actionId},
                                      {"sequence", entry.value("sequence")},
                                      {"actionSequence", static_cast<qint64>(nextActionSequence++)}});
    auto pendingIds = entry.value("pendingActionIds").toArray();
    pendingIds.append(actionId);
    entry.insert("pendingActionIds", pendingIds);
    notifications.replace(index, entry);
    ++revision;
    return {{"ok", true}, {"status", "queued"}, {"revision", static_cast<qint64>(revision)}};
}
QJsonObject acknowledge(const QJsonObject &request) {
    QString owner;
    const auto ownerError=requireOwner(request,&owner);
    if(!ownerError.isEmpty()) return ownerError;
    const auto sequence=request.value("actionSequence").toInteger(-1);
    const QString status=request.value("status").toString();
    if(sequence<1 || (status!="completed" && status!="failed")) return failure("invalid-action-result");
    for(int i=0;i<pendingActions.size();++i) {
        auto action=pendingActions.at(i).toObject();
        if(action.value("ownerId")!=owner || action.value("actionSequence").toInteger()!=sequence) continue;
        if(action.value("status")==status) return {{"ok",true},{"status",status}};
        if(action.value("status")!="delivered") return failure("action-not-delivered");
        action.insert("status",status);
        action.insert("result",request.value("result"));
        pendingActions.replace(i,action);
        const int index=indexOf(owner,action.value("notificationId").toString());
        if(index>=0) {
            auto entry=notifications.at(index).toObject();
            auto ids=entry.value("pendingActionIds").toArray();
            for(int j=ids.size()-1;j>=0;--j) if(ids.at(j)==action.value("actionId")) ids.removeAt(j);
            entry.insert("pendingActionIds",ids);notifications.replace(index,entry);
        }
        ++revision;
        return {{"ok",true},{"status",status},{"revision",static_cast<qint64>(revision)}};
    }
    return failure("action-not-found");
}

void dispatch() {
    dispatchPending=false;
    std::vector<std::shared_ptr<Subscription>> listeners;
    QByteArray changed;
    {
        std::lock_guard<std::mutex> lock(notificationsMutex);
        for(const auto &pair:subscriptions) listeners.push_back(pair.second);
        changed=QJsonDocument(QJsonObject{{"type","changed"},{"revision",static_cast<qint64>(revision)}}).toJson(QJsonDocument::Compact);
    }
    for(const auto &listener:listeners) {
        if(!listener->active) continue;
        listener->callback(changed.constData(),listener->userData);
        if(!listener->active || listener->owner.isEmpty()) continue;
        // Claim one action at a time: a callback may dismiss, replace, or
        // complete a notification, invalidating other queued actions.
        for(int delivered=0;listener->active && delivered<kMaxNotifications;++delivered) {
            QByteArray event;
            {
                std::lock_guard<std::mutex> lock(notificationsMutex);
                for(int i=0;i<pendingActions.size();++i) {
                    auto action=pendingActions.at(i).toObject();
                    if(action.value("ownerId")!=listener->owner || action.value("status")!="queued") continue;
                    action.insert("status","delivered");pendingActions.replace(i,action);++revision;
                    event=QJsonDocument(QJsonObject{{"type","action"},{"action",action},
                        {"revision",static_cast<qint64>(revision)}}).toJson(QJsonDocument::Compact);
                    break;
                }
            }
            if(event.isEmpty()) break;
            listener->callback(event.constData(),listener->userData);
            scheduleDispatch();
        }
    }
}
void scheduleDispatch() {
    auto *app=QCoreApplication::instance();
    if(!app || dispatchPending.exchange(true)) return;
    if(!QMetaObject::invokeMethod(app,[]{dispatch();},Qt::QueuedConnection)) dispatchPending=false;
}
uint64_t subscribe(const char *owner, XemNotificationCallback callback, void *userData) {
    if(!applicationThread() || !owner || !callback) return 0;
    const QString id=QString::fromUtf8(owner);
    if(!id.isEmpty() && !validId(id)) return 0;
    uint64_t handle;
    {
        std::lock_guard<std::mutex> lock(notificationsMutex);
        if(subscriptions.size()>=128) return 0;
        if(!id.isEmpty()) for(const auto &pair:subscriptions)
            if(pair.second->owner==id) return 0;
        handle=nextSubscription++;
        subscriptions.emplace(handle,std::make_shared<Subscription>(Subscription{id,callback,userData}));
    }
    scheduleDispatch();
    return handle;
}
void unsubscribe(uint64_t handle) {
    if(!applicationThread()) return;
    std::lock_guard<std::mutex> lock(notificationsMutex);
    auto found=subscriptions.find(handle);
    if(found==subscriptions.end()) return;
    found->second->active=false;
    subscriptions.erase(found);
}
QJsonObject run(const std::string &command, const QJsonObject &request) {
    if (command == "post") return post(request);
    if (command == "list") return list(request);
    if (command == "dismiss") return dismiss(request);
    if (command == "markRead") return markRead(request);
    if (command == "clear") return clear(request);
    if (command == "actionInvoke") return actionInvoke(request);
    if (command == "acknowledge") return acknowledge(request);
    return failure("unknown-command");
}
char *apiPost(const char *description) { return owned(notificationCommand("post", description)); }
char *apiDismiss(const char *owner, const char *id) {
    if (!owner || !id) return owned(encode(failure("invalid-request")));
    const auto request = QJsonDocument(QJsonObject{{"ownerId", QString::fromUtf8(owner)},
                                                    {"notificationId", QString::fromUtf8(id)}})
                             .toJson(QJsonDocument::Compact);
    return owned(notificationCommand("dismiss", request.constData()));
}
void apiFree(char *value) { std::free(value); }
char *apiQuery(const char *request) { return owned(notificationCommand("list",request)); }
char *apiAcknowledge(const char *request) { return owned(notificationCommand("acknowledge",request)); }
const XemNotificationsApi api{XEM_NOTIFICATIONS_ABI,sizeof(XemNotificationsApi),
    apiPost,apiDismiss,apiQuery,subscribe,unsubscribe,apiAcknowledge,apiFree};
}

std::string notificationCommand(const std::string &command, const char *request) {
    if (command.empty() || command.size() > 32) return encode(failure("invalid-command"));
    if (request && strnlen(request, kMaxRequestBytes + 1) > kMaxRequestBytes) return encode(failure("request-too-large"));
    QJsonObject object;
    if (request && *request) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(request, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return encode(failure("invalid-json"));
        object = document.object();
    }
    QJsonObject result;
    bool changed;
    {
        std::lock_guard<std::mutex> guard(notificationsMutex);
        const auto before=revision;
        result=run(command,object);
        changed=before!=revision;
    }
    if(changed) scheduleDispatch();
    return encode(result);
}

extern "C" const XemNotificationsApi *xem_notifications_get_api(void) { return &api; }
