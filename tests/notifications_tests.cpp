#include "../src/notifications.h"
#include "../sdk/xovi-notifications.h"

#include <QCoreApplication>
#include <QThread>
#include <thread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

QJsonObject command(const char *name, const QJsonObject &request = {}) {
    const auto json = QJsonDocument(request).toJson(QJsonDocument::Compact);
    const auto response = notificationCommand(name, json.constData());
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(response), &error);
    expect(error.error == QJsonParseError::NoError && document.isObject(), "response is JSON object");
    return document.object();
}

QJsonObject post(const QString &id, const QString &message = "Message") {
    return command("post", {{"ownerId", "plugin_one"}, {"notificationId", id},
                            {"title", "Title"}, {"message", message},
                            {"level", "info"}, {"pageId", "main"}});
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    const auto *api=xem_notifications_get_api();
    QJsonArray received;
    const auto callback=+[](const char *text,void *data) {
        auto event=QJsonDocument::fromJson(text).object();
        expect(QThread::currentThread()==QCoreApplication::instance()->thread(), "callback runs on application thread");
        // A callback can query synchronously without taking a recursive lock.
        command("list");
        if(event.value("type")=="action") static_cast<QJsonArray *>(data)->append(event.value("action"));
    };
    const auto subscription=api->subscribe("plugin_one",callback,&received);
    expect(subscription!=0,"owner subscribes");
    expect(!api->subscribe("plugin_one",callback,&received),"duplicate action subscriber rejected");
    const auto first = post("sync");
    expect(first.value("ok").toBool(), "post succeeds");
    expect(first.value("status").toString() == "queued", "post only reports queued");
    expect(first.value("notification").toObject().value("count").toInt() == 1, "first count is one");
    expect(first.value("notification").toObject().value("progress").isNull(), "ordinary notification has null progress");

    const auto repeated = post("sync", "Updated");
    expect(repeated.value("notification").toObject().value("count").toInt() == 2, "duplicate increments count");
    const auto listed = command("list");
    expect(listed.value("notifications").toArray().size() == 1, "duplicate is deduplicated");
    expect(listed.value("notifications").toArray().first().toObject().value("message").toString() == "Updated",
           "duplicate replaces content");

    expect(command("markRead", {{"ownerId", "plugin_one"}, {"notificationId", "sync"}}).value("ok").toBool(),
           "markRead succeeds");
    expect(command("list").value("notifications").toArray().first().toObject().value("read").toBool(),
           "read state persists");
    expect(command("dismiss", {{"ownerId", "plugin_one"}, {"notificationId", "sync"}}).value("ok").toBool(),
           "dismiss succeeds");
    expect(command("list").value("notifications").toArray().isEmpty(), "dismiss removes entry");

    const auto running = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "job"},
                                             {"title", "Download"}, {"message", "Starting"},
                                             {"state", "running"}, {"progress", QJsonObject{{"value", 0.1}}},
                                             {"actions", QJsonArray{QJsonObject{{"id", "cancel"}, {"label", "Cancel"}}}}});
    const auto runningEntry = running.value("notification").toObject();
    const auto runningToast = runningEntry.value("toastRevision").toInteger();
    const auto runningSequence = runningEntry.value("sequence").toInteger();
    expect(running.value("ok").toBool() && runningEntry.value("count").toInt() == 1, "running notification is queued once");
    expect(runningEntry.value("progress").isObject(), "running notification exposes progress object");
    expect(command("markRead", {{"ownerId", "plugin_one"}, {"notificationId", "job"}}).value("ok").toBool(),
           "running notification is readable");
    const auto progress = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "job"},
                                              {"progress", QJsonObject{{"value", 0.5}}}}).value("notification").toObject();
    expect(progress.value("count").toInt() == 1 && progress.value("read").toBool(), "progress update preserves count and read state");
    expect(progress.value("toastRevision").toInteger() == runningToast && progress.value("sequence").toInteger() == runningSequence,
           "progress update does not create a toast or replace the entry");
    const auto completed = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "job"},
                                               {"state", "completed"}, {"progress", QJsonObject{{"value", 1.0}}},
                                               {"actions", QJsonArray{QJsonObject{{"id", "cancel"}, {"label", "Cancel"}}}}}).value("notification").toObject();
    expect(!completed.value("read").toBool() && completed.value("toastRevision").toInteger() > runningToast,
           "terminal progress state creates one unread toast");
    expect(command("markRead", {{"ownerId", "plugin_one"}, {"notificationId", "job"}}).value("ok").toBool(),
           "completed notification is readable");
    const auto completedAgain = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "job"},
                                                    {"state", "completed"}}).value("notification").toObject();
    expect(completedAgain.value("read").toBool() && completedAgain.value("toastRevision").toInteger() == completed.value("toastRevision").toInteger(),
           "repeated terminal state does not toast again");

    expect(command("actionInvoke", {{"ownerId", "plugin_one"}, {"notificationId", "job"},
                                      {"actionId", "cancel"}, {"sequence", runningSequence}}).value("ok").toBool(),
           "available action queues without executing plugin code");
    expect(!command("actionInvoke", {{"ownerId", "plugin_one"}, {"notificationId", "job"},
                                       {"actionId", "cancel"}, {"sequence", runningSequence}}).value("ok").toBool(),
           "duplicate action click is rejected while pending");
    expect(command("list", {{"ownerId", "plugin_one"}}).value("entries").toArray().first().toObject().value("pendingActionIds").toArray().contains("cancel"),
           "pending action is exposed on entry");
    expect(command("list", {{"ownerId", "other_plugin"}}).value("actions").toArray().isEmpty(), "query is owner scoped");
    expect(received.isEmpty(),"action is not delivered inline");
    QCoreApplication::processEvents();
    expect(received.size()==1 && received.first().toObject().value("actionId")=="cancel", "subscription receives queued action");
    const auto actionSequence=received.first().toObject().value("actionSequence");
    expect(command("list", {{"ownerId", "plugin_one"}}).value("actions").toArray().first().toObject().value("status")=="delivered", "query retains delivered action");
    expect(!command("actionInvoke", {{"ownerId","plugin_one"},{"notificationId","job"},{"actionId","cancel"}}).value("ok").toBool(),"delivery does not re-enable action");
    const QJsonObject acknowledgment{{"ownerId","plugin_one"},{"actionSequence",actionSequence},{"status","completed"}};
    expect(command("acknowledge",acknowledgment).value("ok").toBool(),"plugin explicitly acknowledges completion");
    expect(command("acknowledge",acknowledgment).value("ok").toBool(),"acknowledgment is idempotent");
    expect(command("list", {{"ownerId", "plugin_one"}}).value("entries").toArray().first().toObject().value("pendingActionIds").toArray().isEmpty(),"completion re-enables action");
    expect(command("list", {{"ownerId", "plugin_one"}}).value("actions").toArray().size()==1,"query does not consume action history");
    expect(command("actionInvoke", {{"ownerId", "plugin_one"}, {"notificationId", "job"},
                                      {"actionId", "cancel"}, {"sequence", runningSequence}}).value("ok").toBool(),
           "action can be queued again after completion");
    expect(command("dismiss", {{"ownerId", "plugin_one"}, {"notificationId", "job"}}).value("ok").toBool(),
           "dismiss clears notification with a pending action");
    expect(command("list", {{"ownerId", "plugin_one"}}).value("actions").toArray().isEmpty(),
           "dismiss removes stale queued action");

    const auto retiring = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "retiring"},
                                              {"title", "Retiring"}, {"message", "Running"}, {"state", "running"},
                                              {"progress", QJsonObject{{"value", 0.2}}},
                                              {"actions", QJsonArray{QJsonObject{{"id", "cancel"}, {"label", "Cancel"}}}}}).value("notification").toObject();
    expect(command("actionInvoke", {{"ownerId", "plugin_one"}, {"notificationId", "retiring"},
                                      {"actionId", "cancel"}, {"sequence", retiring.value("sequence")}}).value("ok").toBool(),
           "running action queues before terminal transition");
    const auto retired = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "retiring"},
                                             {"state", "completed"}}).value("notification").toObject();
    expect(retired.value("actions").toArray().isEmpty() && retired.value("pendingActionIds").toArray().isEmpty(),
           "terminal patch retires implicit running actions");
    expect(command("list", {{"ownerId", "plugin_one"}}).value("actions").toArray().isEmpty(),
           "terminal transition removes pending running action");
    const auto clearedProgress = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "retiring"},
                                                     {"progress", QJsonValue(QJsonValue::Null)}}).value("notification").toObject();
    expect(clearedProgress.value("progress").isNull(), "progress null clears progress on patch");

    const auto replacement = post("replace").value("notification").toObject();
    const auto staleSequence = replacement.value("sequence").toInteger();
    const auto replacementNew = post("replace", "replacement").value("notification").toObject();
    expect(replacementNew.value("sequence").toInteger() != staleSequence, "ordinary replacement gets a new sequence");
    expect(!command("actionInvoke", {{"ownerId", "plugin_one"}, {"notificationId", "replace"},
                                       {"actionId", "cancel"}, {"sequence", staleSequence}}).value("ok").toBool(),
           "stale sequence is rejected before action dispatch");

    expect(!command("post", {{"ownerId", "bad owner"}, {"notificationId", "id"},
                              {"title", "T"}, {"message", "M"}}).value("ok").toBool(),
           "invalid owner is rejected");
    expect(!command("post", {{"ownerId", "plugin_one"}, {"notificationId", "id"},
                              {"title", "T"}, {"message", "M"}, {"level", "urgent"}}).value("ok").toBool(),
           "invalid level is rejected");
    expect(!command("list", {{"ownerId", 7}}).value("ok").toBool(), "wrong owner type is rejected");
    expect(notificationCommand("post", "not json").find("invalid-json") != std::string::npos, "bad JSON is rejected");
    expect(notificationCommand("post", std::string(16 * 1024 + 1, 'x').c_str()).find("request-too-large") != std::string::npos,
           "oversized request is rejected");

    for (int i = 0; i <= 100; ++i) post(QString("n%1").arg(i));
    const auto bounded = command("list", {{"ownerId", "plugin_one"}}).value("notifications").toArray();
    expect(bounded.size() == 100, "store is capped at one hundred entries");
    expect(bounded.first().toObject().value("notificationId").toString() == "n100", "newest entry is retained");
    const auto evicting = command("post", {{"ownerId", "plugin_one"}, {"notificationId", "evict"},
                                             {"title", "Evict"}, {"message", "Queued action"},
                                             {"actions", QJsonArray{QJsonObject{{"id", "cancel"}, {"label", "Cancel"}}}}}).value("notification").toObject();
    expect(command("actionInvoke", {{"ownerId", "plugin_one"}, {"notificationId", "evict"},
                                      {"actionId", "cancel"}, {"sequence", evicting.value("sequence")}}).value("ok").toBool(),
           "action can be pending before notification eviction");
    for (int i = 0; i <= 100; ++i) post(QString("evict%1").arg(i));
    expect(command("list", {{"ownerId", "plugin_one"}}).value("actions").toArray().isEmpty(),
           "eviction removes queued action for evicted notification");

    expect(api && api->abiVersion == XEM_NOTIFICATIONS_ABI, "C API ABI is available");
    expect(api && api->structSize == sizeof(XemNotificationsApi), "C API table size matches");
    const auto apiInput = QJsonDocument(QJsonObject{{"ownerId", "plugin_two"}, {"notificationId", "api"},
                                                    {"title", "API"}, {"message", "Posted"}})
                              .toJson(QJsonDocument::Compact);
    char *apiResult = api->post(apiInput.constData());
    expect(apiResult && std::string(apiResult).find("queued") != std::string::npos, "C API post queues notification");
    api->freeString(apiResult);
    char *dismissResult = api->dismiss("plugin_two", "api");
    expect(dismissResult && std::string(dismissResult).find("dismissed") != std::string::npos, "C API dismiss works");
    api->freeString(dismissResult);

    int changes=0;
    const auto observer=api->subscribe("",+[](const char *,void *data){++*static_cast<int *>(data);},&changes);
    std::thread worker([&] { auto *reply=api->post(apiInput.constData());api->freeString(reply); });
    worker.join();
    expect(changes==0,"worker post never invokes subscriber inline");
    QCoreApplication::processEvents();
    expect(changes>0,"worker post wakes application thread without polling");
    const int before=changes;
    post("unsubscribe");api->unsubscribe(observer);api->unsubscribe(subscription);
    QCoreApplication::processEvents();
    expect(changes==before,"unsubscribe cancels already queued callbacks");

    expect(command("clear", {{"ownerId", "plugin_one"}}).value("ok").toBool(), "clear owner succeeds");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
