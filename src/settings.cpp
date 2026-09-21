#include "settings.h"
#include "inventory.h"
#include "../xovi.h"
#include "../sdk/xovi-settings.h"
#include "../sdk/qrr-api.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QLockFile>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <mutex>
#include <cstring>

namespace {
std::mutex mutex;
QJsonObject uiStates;
QMap<QString,QJsonObject> runtimePages;
const QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
QJsonObject failure(const QString &code) { return {{"ok", false}, {"error", code}}; }
bool validId(const QString &id) {
    return QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$").match(id).hasMatch();
}
QJsonObject readObject(const QString &path, bool *ok = nullptr) {
    QFile f(path);
    if(!f.exists()) { if(ok) *ok=true; return {}; }
    QJsonParseError err;
    bool opened=f.open(QIODevice::ReadOnly);
    auto doc=QJsonDocument::fromJson(opened ? f.readAll() : QByteArray(), &err);
    if(ok) *ok=opened && err.error==QJsonParseError::NoError && doc.isObject();
    return doc.object();
}
bool writeObject(const QString &path, const QJsonObject &value) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile f(path);
    auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
    return f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size() && f.commit();
}
QString root() { return QString::fromStdString(xoviRoot()); }
QString configPath(const QString &id) { return root()+"/exthome/"+id+"/manager-settings.json"; }
QString pageKey(const QString &id, const QString &page) { return id+"/"+page; }
QString safeFile(const ExtensionManifest &p, const QString &relative) {
    if(relative.isEmpty() || QDir::isAbsolutePath(relative)) return {};
    const auto parts=relative.split('/');
    if(parts.contains("..") || parts.contains(".") || parts.contains("")) return {};
    auto base=QFileInfo(QString::fromStdString(p.directoryPath)).canonicalFilePath();
    auto file=QFileInfo(base+"/"+relative).canonicalFilePath();
    if(base.isEmpty() || !file.startsWith(base+"/") || !QFileInfo(file).isFile()) return {};
    return QUrl::fromLocalFile(file).toString();
}
bool ready(const char *id) {
    return Environment && Environment->getExtensionLoadState &&
        Environment->getExtensionLoadState(id)==XOVI_EXTENSION_INITIALIZED;
}
QJsonArray pages() {
    reconcileDisabledEntries();
    const auto inventory = loadInventory(false);
    QJsonArray result;
    QSet<QString> keys;
    QMap<QString, QJsonObject> presentation;
    QMap<QString, QJsonObject> launcherDefaults;
    if(Environment && Environment->createMetadataSearchingIterator && Environment->nextFunctionMetadataEntry) {
        ExtensionMetadataIterator it{};
        Environment->createMetadataSearchingIterator(&it, XEM_LAUNCHER_DEFAULTS_METADATA);
        while(auto *meta=Environment->nextFunctionMetadataEntry(&it)) {
            if(meta->type!=METADATA_TYPE_INT || meta->value.i!=1 || !ready(it.extensionName) || !it.functionAddress) continue;
            const auto *p=reinterpret_cast<XemLauncherDefaultsGetterV1>(it.functionAddress)();
            if(!p || p->abiVersion!=1 || p->structSize<sizeof(*p) || !p->pageId || (p->locations & ~15u)) continue;
            launcherDefaults.insert(pageKey(QString::fromUtf8(it.extensionName),QString::fromUtf8(p->pageId)),
                {{"sidebar",bool(p->locations & XEM_LAUNCHER_SIDEBAR)}, {"bottom",bool(p->locations & XEM_LAUNCHER_BOTTOM)},
                 {"settings",bool(p->locations & XEM_LAUNCHER_SETTINGS)},{"quick",bool(p->locations & XEM_LAUNCHER_QUICK)}});
        }
    }
    if(Environment && Environment->createMetadataSearchingIterator && Environment->nextFunctionMetadataEntry) {
        ExtensionMetadataIterator it{};
        Environment->createMetadataSearchingIterator(&it, XEM_PRESENTATION_METADATA);
        while(auto *meta=Environment->nextFunctionMetadataEntry(&it)) {
            if(meta->type!=METADATA_TYPE_INT || (meta->value.i!=1 && meta->value.i!=2) || !ready(it.extensionName) || !it.functionAddress) continue;
            const auto *p=reinterpret_cast<XemSettingsPresentationGetterV1>(it.functionAddress)();
            if(!p || (p->abiVersion!=1 && p->abiVersion!=2) || p->abiVersion!=meta->value.i || p->structSize<sizeof(*p) || !p->pageId) continue;
            QString chrome="host";
            if (p->abiVersion==2) {
                if (p->structSize<sizeof(XemSettingsPresentationV2)) continue;
                const auto mode=reinterpret_cast<const XemSettingsPresentationV2 *>(p)->chromeMode;
                if (mode>XEM_CHROME_PAGE) continue;
                if (mode==XEM_CHROME_PAGE) chrome="page";
            }
            const QString icon=QString::fromUtf8(p->iconSource ? p->iconSource : "");
            if(!icon.isEmpty() && QUrl(icon).scheme()!="qrc") continue;
            presentation.insert(pageKey(QString::fromUtf8(it.extensionName),QString::fromUtf8(p->pageId)),
                {{"title",QString::fromUtf8(p->title ? p->title : "")},
                 {"titleContext",QString::fromUtf8(p->translationContext ? p->translationContext : "XoviPluginNames")},
                 {"iconSource",icon},{"chrome",chrome}});
        }
    }
    if(Environment && Environment->createMetadataSearchingIterator && Environment->nextFunctionMetadataEntry) {
        ExtensionMetadataIterator it{};
        Environment->createMetadataSearchingIterator(&it, XEM_SETTINGS_METADATA);
        while(auto *meta=Environment->nextFunctionMetadataEntry(&it)) {
            if(meta->type!=METADATA_TYPE_INT || meta->value.i!=1 || !ready(it.extensionName) || !it.functionAddress) continue;
            const QString id=QString::fromUtf8(it.extensionName);
            if(isSettingsProviderSuppressed(id.toStdString())) continue;
            int package = findExtension(inventory, id.toStdString());
            if(package < 0) package = findExtension(inventory, id.toStdString()+".so");
            if(package >= 0 && inventory.extensions[package].hasManifest && !inventory.extensions[package].enabled) continue;
            const auto *p=reinterpret_cast<XemSettingsProviderGetterV1>(it.functionAddress)();
            if(!p || p->abiVersion!=1 || p->structSize<sizeof(*p) || !p->pageId || !p->source || !p->sourceSize || p->sourceSize>1024*1024) continue;
            QString page=QString::fromUtf8(p->pageId);
            if(!validId(id) || !validId(page) || p->sourceKind>1) continue;
            QString key=pageKey(id,page);
            if(keys.contains(key)) continue;
            QString source=QString::fromUtf8(p->source, int(p->sourceSize));
            QString base=QString::fromUtf8(p->baseUrl ? p->baseUrl : "");
            QUrl url(p->sourceKind==1 ? base : source);
            if(!url.isValid() || (url.scheme()!="file" && url.scheme()!="qrc")) continue;
            keys.insert(key);
            // XOVI's runtime name may be an old .so basename rather than the
            // managed manifest id (advanced_settings vs advanced-settings).
            const QString packageId = package < 0 ? id : QString::fromStdString(inventory.extensions[package].id);
            auto display=presentation.value(key);
            auto defaults=launcherDefaults.value(key);
            if(defaults.isEmpty() && package>=0) {
                defaults=readObject(QString::fromStdString(inventory.extensions[package].manifestPath)).value("settings").toObject().value("launchers").toObject();
            }
            result.append(QJsonObject{{"id",id},{"packageId",packageId},{"pageId",page},{"title",display.value("title").toString(QString::fromUtf8(p->title ? p->title : p->pageId))},{"titleContext",display.value("titleContext").toString("XoviPluginNames")},{"iconSource",display.value("iconSource").toString()},
                {"launcherDefaults",defaults},{"chrome",display.value("chrome").toString("host")},{"kind",p->sourceKind==1 ? "inline" : "url"},{"source",source},{"baseUrl",base},{"available",true},{"provider","native"}});
        }
    }
    for(auto page:runtimePages) {
        const auto id=page.value("id").toString();
        if(isSettingsProviderSuppressed(id.toStdString())) continue;
        int package=findExtension(inventory,id.toStdString());
        if(package<0) package=findExtension(inventory,id.toStdString()+".qmd");
        if(package<0) package=findExtension(inventory,id.toStdString()+".so");
        if(package>=0 && !inventory.extensions[package].enabled) continue;
        const auto key=pageKey(id,page.value("pageId").toString());
        if(keys.contains(key)) continue;
        keys.insert(key);
        const auto packageId=package<0 ? id : QString::fromStdString(inventory.extensions[package].id);
        keys.insert(pageKey(packageId,page.value("pageId").toString()));
        page.insert("packageId",packageId);
        page.insert("available",true);
        page.insert("provider","runtime");
        result.append(page);
    }
    for(const auto &p: inventory.extensions) {
        if(!p.valid || !p.hasManifest) continue;
        auto manifest=readObject(QString::fromStdString(p.manifestPath));
        auto settings=manifest.value("settings").toObject();
        if(settings.value("apiVersion").toInt(1)!=1) continue;
        QString relative=settings.value("page").toString();
        if(p.type==PACKAGE_QML && relative.isEmpty()) relative=QString::fromStdString(p.entry);
        if(relative.isEmpty()) continue;
        QString id=QString::fromStdString(p.id), page=settings.value("pageId").toString("main");
        if(!validId(id) || !validId(page) || keys.contains(pageKey(id,page))) continue;
        QString icon=settings.value("iconSource").toString();
        if(!icon.isEmpty() && QUrl(icon).isRelative()) icon=safeFile(p,icon);
        QString url=safeFile(p,relative);
        bool active=p.type!=PACKAGE_QML || (p.enabled && QFileInfo(QString::fromStdString(enabledEntryPath(p))).canonicalFilePath()==QFileInfo(QString::fromStdString(sourceEntryPath(p))).canonicalFilePath());
        result.append(QJsonObject{{"id",id},{"pageId",page},{"title",settings.value("title").toString(QString::fromStdString(p.name))},{"titleTranslations",settings.value("titleTranslations").toObject()},
            {"kind","url"},{"source",url},{"baseUrl",url},{"available",active && !url.isEmpty()},
            {"launcherDefaults",settings.value("launchers").toObject()},{"chrome",settings.value("chrome").toString()=="page" ? "page" : "host"},{"provider","manifest"},{"launcherAvailable",p.enabled && active && !url.isEmpty()}, {"titleContext",settings.value("titleContext").toString("XoviPluginNames")}, {"iconSource",icon}, {"error",url.isEmpty() ? "invalid-page-path" : active ? "" : "disabled"}});
    }
    return result;
}
QString launcherPath() { return root()+"/exthome/xovi-extension-manager/launchers.json"; }
bool managerReachable(const QJsonObject &entries) {
    const auto settings=entries.value(pageKey("xochitl","settings")).toObject();
    if (settings.value("sidebar").toBool(true) &&
        (entries.value(pageKey("xovi-extension-manager","inventory")).toObject().value("settings").toBool(true) ||
         entries.value(pageKey("xovi-extension-manager","notifications")).toObject().value("settings").toBool(false))) return true;
    if(entries.value(pageKey("xovi-extension-manager","inventory")).toObject().value("quick").toBool(false)) return true;
    // Both built-in screens expose the manager navigation rail.
    for (const auto &page:{QStringLiteral("inventory"),QStringLiteral("notifications")}) {
        const auto manager=entries.value(pageKey("xovi-extension-manager",page)).toObject();
        if (manager.value("sidebar").toBool(false) || manager.value("bottom").toBool(false)) return true;
    }
    return false;
}
QJsonObject recoverManagerEntry(QJsonObject entries) {
    if (!managerReachable(entries)) {
        auto settings=entries.value(pageKey("xochitl","settings")).toObject();
        settings.insert("sidebar",true);entries.insert(pageKey("xochitl","settings"),settings);
        auto manager=entries.value(pageKey("xovi-extension-manager","inventory")).toObject();
        manager.insert("settings",true);entries.insert(pageKey("xovi-extension-manager","inventory"),manager);
    }
    return entries;
}
QJsonObject launcherList() {
    const auto discoveredPages=pages();
    // Snapshot defaults once, before presenting them. Future declarations never
    // overwrite a user's choice, including an explicit all-off choice.
    QDir().mkpath(QFileInfo(launcherPath()).absolutePath());
    QLockFile lock(launcherPath()+".lock");
    if(!lock.tryLock(0)) return failure("busy");
    bool ok=false;
    auto config=readObject(launcherPath(),&ok);
    if(!ok || (!config.isEmpty() && (config.value("version")!=1 || !config.value("entries").isObject()))) return failure("invalid-launcher-file");
    auto saved=config.value("entries").toObject();
    for(const auto &value:discoveredPages) {
        const auto p=value.toObject();
        if(!p.value("launcherAvailable").toBool(p.value("available").toBool())) continue;
        const auto key=pageKey(p.value("id").toString(),p.value("pageId").toString());
        if(saved.contains(key)) continue;
        const auto defaults=p.value("launcherDefaults").toObject();
        saved.insert(key,QJsonObject{{"sidebar",defaults.value("sidebar").toBool(false)},
            {"bottom",defaults.value("bottom").toBool(false)},{"settings",defaults.value("settings").toBool(false)},{"quick",defaults.value("quick").toBool(false)}});
    }
    saved=recoverManagerEntry(saved);
    if(saved!=config.value("entries").toObject()) {
        config.insert("version",1);config.insert("entries",saved);
        if(!writeObject(launcherPath(),config)) return failure("write-failed");
    }
    lock.unlock();
    QJsonArray entries;
    QSet<QString> seen;
    auto append=[&](const QString &id,const QString &page,const QString &title,bool available,const QString &context="XoviPluginNames",const QString &icon="qrc:/ark/icons/puzzle",const QJsonObject &translations=QJsonObject()) {
        auto key=pageKey(id,page);
        if(!validId(id) || !validId(page) || seen.contains(key)) return;
        seen.insert(key);
        auto pins=saved.value(key).toObject();
        entries.append(QJsonObject{{"id",id},{"pageId",page},{"title",title.left(128)},{"titleContext",context},{"iconSource",icon},{"titleTranslations",translations},
            {"sidebar",pins.value("sidebar").toBool(false)},{"bottom",pins.value("bottom").toBool(false)},
            {"settings",pins.value("settings").toBool(id=="xovi-extension-manager" && page=="inventory")},
            {"quick",pins.value("quick").toBool(id=="xovi-extension-manager" && page=="notifications")},
            {"available",available}});
    };
    append("xovi-extension-manager","inventory","Extensions",true);
    append("xovi-extension-manager","notifications","Notifications",true);
    for(auto value:discoveredPages) {
        auto p=value.toObject();
        append(p.value("id").toString(),p.value("pageId").toString(),p.value("title").toString(),p.value("launcherAvailable").toBool(p.value("available").toBool()),p.value("titleContext").toString(),p.value("iconSource").toString(),p.value("titleTranslations").toObject());
    }
    QJsonArray nativeEntries;
    const auto addNative = [&](const QString &page, const QString &title, const QString &location, const QString &icon) {
        const auto pins = saved.value(pageKey("xochitl", page)).toObject();
        nativeEntries.append(QJsonObject{{"id","xochitl"},{"pageId",page},{"title",title},
            {"titleContext","NativeLaunchers"},{"iconSource","qrc:/ark/icons/"+icon},{"native",true},{"available",true},
            {"location",location},{"sidebar",location=="sidebar" && pins.value(location).toBool(true)},
            {"bottom",location=="bottom" && pins.value(location).toBool(true)},
            {"quick",location=="quick" && pins.value(location).toBool(true)}});
    };
    addNative("my-files", "My files", "sidebar", "my_files");
    addNative("filters", "Filter by", "sidebar", "filter");
    addNative("favorites", "Favorites", "sidebar", "star");
    addNative("tags", "Tags", "sidebar", "tag");
    addNative("integrations", "Integrations", "sidebar", "cloud");
    addNative("trash", "Trash", "sidebar", "trashcan");
    addNative("help", "Help", "sidebar", "compass");
    addNative("settings", "Settings", "sidebar", "cog");
    addNative("search-action", "Search", "bottom", "lens");
    addNative("library-action", "Create", "bottom", "plus");
    addNative("calendar-actions", "Calendar", "bottom", "calendar");
    addNative("airplane", "Airplane mode", "quick", "airplane");
    addNative("screen-share", "Screen Share", "quick", "screen_share");
    addNative("rotation", "Rotation lock", "quick", "orientation_lock_locked");
    return {{"ok",true},{"entries",entries},{"nativeEntries",nativeEntries}};
}
const QrrApiV1 *qrr() {
    if(!ready("qt-resource-rebuilder") || !Environment->createMetadataSearchingIterator || !Environment->nextFunctionMetadataEntry) return nullptr;
    ExtensionMetadataIterator it{};
    Environment->createMetadataSearchingIterator(&it,QRR_API_METADATA);
    while(auto *m=Environment->nextFunctionMetadataEntry(&it)) {
        if(std::strcmp(it.extensionName,"qt-resource-rebuilder") || m->type!=METADATA_TYPE_INT || m->value.i!=1 || !it.functionAddress) continue;
        auto *api=reinterpret_cast<const QrrApiV1 *(*)()>(it.functionAddress)();
        if(api && api->abiVersion==1 && api->structSize>=sizeof(*api) && api->snapshot && api->freeString) return api;
    }
    return nullptr;
}
QJsonObject run(const std::string &cmd,const QJsonObject &req) {
    QString id=req.value("id").toString();
    if(cmd=="settingsRegister" || cmd=="settingsUnregister") {
        const auto page=req.value("pageId").toString("main");
        const auto token=req.value("registrationToken").toString();
        if(!validId(id) || !validId(page) || token.isEmpty() || token.size()>128) return failure("invalid-registration");
        const auto key=pageKey(id,page);
        if(runtimePages.contains(key) && runtimePages[key].value("registrationToken")!=token) return failure("page-already-registered");
        if(cmd=="settingsUnregister") {runtimePages.remove(key);return {{"ok",true}};}
        if(runtimePages.size()>=256 && !runtimePages.contains(key)) return failure("registration-limit");
        const auto kind=req.value("kind").toString();
        const auto source=req.value("source").toString();
        if(req.value("title").toString().isEmpty() || source.isEmpty() || source.toUtf8().size()>1024*1024) return failure("invalid-page");
        if(kind!="url" && kind!="inline" && kind!="component") return failure("invalid-page-kind");
        if(kind!="component") {
            const QUrl url(kind=="inline" ? req.value("baseUrl").toString() : source);
            if(!url.isValid() || (url.scheme()!="qrc" && url.scheme()!="file")) return failure("invalid-page-url");
        }
        if(req.value("chrome").toString()!="host" && req.value("chrome").toString()!="page") return failure("invalid-chrome");
        auto registered=req;
        registered.insert("pageId",page);
        runtimePages.insert(key,registered);
        return {{"ok",true}};
    }
    if(cmd=="launcherList") return launcherList();
    if(cmd=="launcherSet") {
        const auto page=req.value("pageId").toString();
        const auto location=req.value("location").toString();
        if(location!="sidebar" && location!="bottom" && location!="settings" && location!="quick") return failure("invalid-location");
        if(!validId(id) || !validId(page) || !req.value("enabled").isBool()) return failure("invalid-request");
        auto listed=launcherList();
        if(!listed.value("ok").toBool()) return listed;
        bool found=false;
        auto candidates = listed.value("entries").toArray();
        for (auto value : listed.value("nativeEntries").toArray()) candidates.append(value);
        for(auto value:candidates) {
            auto entry=value.toObject();
            if(entry.value("id")==id && entry.value("pageId")==page) {
                if (entry.value("native").toBool() && entry.value("location").toString()!=location)
                    return failure("unsupported-native-location");
                found=true;
            }
        }
        if(!found) return failure("launcher-entry-not-found");
        QDir().mkpath(QFileInfo(launcherPath()).absolutePath());
        QLockFile lock(launcherPath()+".lock");
        if(!lock.tryLock(0)) return failure("busy");
        bool ok=false;
        auto config=readObject(launcherPath(),&ok);
        if(!ok || (!config.isEmpty() && (config.value("version")!=1 || !config.value("entries").isObject()))) return failure("invalid-launcher-file");
        auto entries=recoverManagerEntry(config.value("entries").toObject());
        auto pins=entries.value(pageKey(id,page)).toObject();
        pins.insert(location,req.value("enabled"));
        entries.insert(pageKey(id,page),pins);
        if (!managerReachable(entries)) return failure("last-manager-entry");
        config.insert("version",1);config.insert("entries",entries);
        if(!writeObject(launcherPath(),config)) return failure("write-failed");
        return {{"ok",true},{"requiresRestart",false}};
    }
    if(cmd=="settingsList") return {{"ok",true},{"pages",pages()},{"sessionId",session},{"uiStates",uiStates}};
    if(cmd=="injectionsGet") {
        auto *api=qrr();
        if(!api) return failure("rebuilder-unavailable");
        char *s=api->snapshot();
        if(!s) return failure("snapshot-unavailable");
        auto doc=QJsonDocument::fromJson(s); api->freeString(s);
        auto out=doc.object();
        bool ok; auto policy=readObject(root()+"/exthome/qt-resource-rebuilder/injection-policy.json",&ok);
        out.insert("policy",policy); out.insert("policyValid",ok);
        return out;
    }
    if(!validId(id)) return failure("invalid-id");
    bool known=false;
    for(const auto &p:loadInventory(false).extensions) if(p.id==id.toStdString()) known=true;
    for(const auto &page:runtimePages) if(page.value("id")==id) known=true;
    if(!known && !ready(id.toUtf8().constData())) return failure("not-found");
    if(cmd=="settingsGet" || cmd=="settingsUpdate") {
        QString path=configPath(id);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QLockFile lock(path+".lock");
        if(!lock.tryLock(0)) return failure("settings-busy");
        bool ok; auto current=readObject(path,&ok);
        if(!ok) return failure("invalid-settings-file");
        if(!current.isEmpty() && (!current.value("values").isObject() || !current.value("revision").isDouble() ||
            current.value("revision").toDouble()<0 || current.value("revision").toDouble()!=current.value("revision").toInt(-1))) return failure("invalid-settings-file");
        int revision=current.value("revision").toInt(0);
        if(cmd=="settingsUpdate") {
            if(!req.value("values").isObject() || !req.value("expectedRevision").isDouble()) return failure("invalid-request");
            if(req.value("expectedRevision").toDouble()!=revision) return failure("revision-conflict");
            if(revision==2147483647) return failure("revision-exhausted");
            auto values=current.value("values").toObject();
            auto patch=req.value("values").toObject();
            for(auto i=patch.begin();i!=patch.end();++i) values.insert(i.key(),i.value());
            current={{"revision",revision+1},{"values",values}};
            if(!writeObject(path,current)) return failure("write-failed");
        }
        return {{"ok",true},{"id",id},{"revision",current.value("revision").toInt()},
            {"values",current.value("values").toObject()},{"applyState","stored"}};
    }
    if(cmd=="injectionsSet") {
        QString injection=req.value("injectionId").toString();
        if(!validId(injection) || !req.value("enabled").isBool()) return failure("invalid-request");
        QString path=root()+"/exthome/qt-resource-rebuilder/injection-policy.json";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QLockFile lock(path+".lock");
        if(!lock.tryLock(0)) return failure("policy-busy");
        bool ok; auto policy=readObject(path,&ok);
        if(!ok || (!policy.isEmpty() && (policy.value("version")!=1 || !policy.value("entries").isObject()))) return failure("invalid-policy-file");
        auto entries=policy.value("entries").toObject();
        entries.insert(pageKey(id,injection),req.value("enabled"));
        policy.insert("entries",entries); policy.insert("version",1);
        if(!writeObject(path,policy)) return failure("write-failed");
        return {{"ok",true},{"requiresRestart",true},{"restartTarget","xochitl"},{"applyState","pending-restart"}};
    }
    if(cmd=="uiReport") {
        QString page=req.value("pageId").toString(), state=req.value("state").toString();
        if(!validId(page) || !QStringList{"loading","ready","failed","unloaded","warning"}.contains(state)) return failure("invalid-request");
        if(uiStates.size()>=256 && !uiStates.contains(pageKey(id,page))) return failure("status-limit");
        auto previous=uiStates.value(pageKey(id,page)).toObject();
        if(state=="warning") previous.insert("warning",req.value("message").toString().left(8192));
        else {if(state=="loading" || state=="unloaded") previous.remove("warning");previous.insert("state",state);previous.insert("message",req.value("message").toString().left(8192));}
        uiStates.insert(pageKey(id,page),previous);
        return {{"ok",true}};
    }
    return failure("unknown-command");
}
}
std::string settingsCommand(const std::string &cmd,const char *request) {
    std::lock_guard<std::mutex> guard(mutex);
    QJsonObject req;
    if(request && *request) {
        QJsonParseError error;
        auto doc=QJsonDocument::fromJson(request,&error);
        if(error.error!=QJsonParseError::NoError || !doc.isObject())
            return QJsonDocument(failure("invalid-json")).toJson(QJsonDocument::Compact).toStdString();
        req=doc.object();
    }
    return QJsonDocument(run(cmd,req)).toJson(QJsonDocument::Compact).toStdString();
}
