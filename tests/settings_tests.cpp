#include "../src/settings.h"
#include "../src/inventory.h"
#include "../xovi.h"
#include "../sdk/xovi-settings.h"
#include "../sdk/qrr-api.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char pageSource[]="import QtQuick\nItem { required property var settingsContext }";
static XemSettingsProviderV1 provider{1,sizeof(provider),"main","Native",1,pageSource,sizeof(pageSource)-1,"qrc:/xovi/native/Main.qml"};
static const XemSettingsProviderV1 *getProvider(){return &provider;}
static bool consumed;
static bool qrrInstalled=false;
static bool wantQrr=false;
static int releases=0;
static char *snapshot(){return strdup("{\"ok\":true,\"sessionId\":\"test-session\",\"revision\":3,\"results\":[]}");}
static void release(char *s){++releases;free(s);}
static const QrrApiV1 qrrApi{1,sizeof(QrrApiV1),nullptr,snapshot,release};
static const QrrApiV1 *getQrr(){return &qrrApi;}
static bool matching, wantPresentation, wantDefaults;
static bool declareDefaults=true;
static XemLauncherDefaultsV1 defaults{1,sizeof(XemLauncherDefaultsV1),"main",XEM_LAUNCHER_SETTINGS | XEM_LAUNCHER_QUICK};
static const XemLauncherDefaultsV1 *getDefaults(){return &defaults;}
static const XemSettingsPresentationV1 display{1,sizeof(XemSettingsPresentationV1),"main","Original page","OriginalPage","qrc:/native/original.svg"};
static bool pageChrome=false;
static const XemSettingsPresentationV2 displayV2{{2,sizeof(XemSettingsPresentationV2),"main","Original page","OriginalPage","qrc:/native/original.svg"},XEM_CHROME_PAGE};
static const XemSettingsPresentationV1 *getDisplay(){return pageChrome ? &displayV2.base : &display;}
static int runtimeState=7;
static void begin(ExtensionMetadataIterator*,const char *key){consumed=false;wantDefaults=std::strcmp(key,XEM_LAUNCHER_DEFAULTS_METADATA)==0;wantPresentation=std::strcmp(key,XEM_PRESENTATION_METADATA)==0;matching=std::strcmp(key,XEM_SETTINGS_METADATA)==0;wantQrr=std::strcmp(key,QRR_API_METADATA)==0;}
static XoviMetadataEntry *next(ExtensionMetadataIterator *it){
    if(!consumed && wantDefaults && declareDefaults) {consumed=true;it->extensionName="native";it->functionAddress=reinterpret_cast<void *>(getDefaults);static XoviMetadataEntry meta{XEM_LAUNCHER_DEFAULTS_METADATA,METADATA_TYPE_INT,{1}};return &meta;}
    if(!consumed && wantPresentation) { consumed=true;it->extensionName="native";it->functionAddress=reinterpret_cast<void *>(getDisplay);static XoviMetadataEntry meta{XEM_PRESENTATION_METADATA,METADATA_TYPE_INT,{1}};meta.value.i=pageChrome ? 2 : 1;return &meta; }
    static XoviMetadataEntry qrrMeta{QRR_API_METADATA,METADATA_TYPE_INT,{1}};
    if(!consumed && wantQrr && qrrInstalled) {consumed=true;it->extensionName="qt-resource-rebuilder";it->functionAddress=reinterpret_cast<void *>(getQrr);return &qrrMeta;}
    if(consumed || !matching) return nullptr; consumed=true;
    it->extensionName="native";it->functionAddress=reinterpret_cast<void *>(getProvider);
    static XoviMetadataEntry meta{XEM_SETTINGS_METADATA,METADATA_TYPE_INT,{1}};return &meta;
}
static int state(const char *id){return !std::strcmp(id,"native") ? runtimeState : (!std::strcmp(id,"qt-resource-rebuilder") && qrrInstalled ? 7 : -1);}
static XoViEnvironment env{};
extern "C" { const XoViEnvironment *Environment=&env; }
static void check(bool b,const char *message){if(!b){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}std::printf("PASS %s\n",message);}
static void write(const QString &path,const QByteArray &data){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);check(f.open(QIODevice::WriteOnly),"fixture open");f.write(data);}
static QJsonObject call(const char *cmd,QJsonObject req={}){
    auto b=QJsonDocument(req).toJson(QJsonDocument::Compact);auto s=settingsCommand(cmd,b.constData());return QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);QTemporaryDir dir;check(dir.isValid(),"temporary root");
    qputenv("XOVI_ROOT",dir.path().toUtf8());
    env.createMetadataSearchingIterator=begin;env.nextFunctionMetadataEntry=next;env.getExtensionLoadState=state;
    check(call("settingsList")["pages"].toArray().size()==1,"native provider discovered without manifest");
    QJsonObject runtimePage{{"id","pure-qmd"},{"pageId","main"},{"title","Pure QMD"},
        {"kind","inline"},{"source","import QtQuick; Item {}"},{"baseUrl","qrc:/pure/Main.qml"},
        {"chrome","host"},{"registrationToken","lease-one"},{"launcherDefaults",QJsonObject{{"settings",true}}}};
    check(call("settingsRegister",runtimePage)["ok"].toBool(),"QML runtime registration needs no manifest or installed package");
    auto registeredPages=call("settingsList")["pages"].toArray();
    check(registeredPages.size()==2 && registeredPages.last().toObject()["provider"]=="runtime","runtime page appears in settings list");
    auto runtimePins=call("launcherList")["entries"].toArray();
    check(runtimePins.last().toObject()["id"]=="pure-qmd" && runtimePins.last().toObject()["settings"].toBool(),"runtime defaults create pinnable entry");
    check(call("launcherSet",{{"id","pure-qmd"},{"pageId","main"},{"location","settings"},{"enabled",false}})["ok"].toBool(),"runtime entry can be unpinned");
    auto conflicting=runtimePage;conflicting["registrationToken"]="other-owner";
    check(call("settingsRegister",conflicting)["error"]=="page-already-registered","another lifetime cannot replace a registration");
    check(call("settingsRegister",conflicting)["diagnostic"].toObject()["category"]=="page","registration rejection carries page recovery classification");
    check(call("settingsUnregister",conflicting)["error"]=="page-already-registered","stale unregister cannot remove a current page");
    check(call("settingsGet",{{"id","pure-qmd"}})["ok"].toBool(),"runtime provider can use optional settings storage");
    check(call("settingsUnregister",runtimePage)["ok"].toBool(),"runtime page unregisters");
    check(call("settingsList")["pages"].toArray().size()==1,"unregistered page disappears");
    check(call("settingsRegister",runtimePage)["ok"].toBool(),"runtime page can register again next lifetime");
    runtimePins=call("launcherList")["entries"].toArray();
    check(!runtimePins.last().toObject()["settings"].toBool(),"reregistration preserves user PIN choice over defaults");
    call("settingsUnregister",runtimePage);
    auto invalidRuntime=runtimePage;invalidRuntime["source"]="https://example.com/Page.qml";invalidRuntime["kind"]="url";
    check(call("settingsRegister",invalidRuntime)["error"]=="invalid-page-url","runtime registration rejects nonlocal page URL");
    const auto originalPage=call("settingsList")["pages"].toArray().first().toObject();
    check(originalPage["title"]=="Original page" && originalPage["titleContext"]=="OriginalPage" && originalPage["iconSource"]=="qrc:/native/original.svg","provider owns original presentation, without manager guessing");
    check(call("settingsList")["pages"].toArray().first().toObject()["chrome"]=="host","legacy presentation keeps manager chrome");
    pageChrome=true;
    check(call("settingsList")["pages"].toArray().first().toObject()["chrome"]=="page","V2 presentation opts into self-contained page chrome");
    pageChrome=false;
    const auto nativeEntries=call("launcherList")["nativeEntries"].toArray();
    check(nativeEntries.size()==14 && nativeEntries.first().toObject()["sidebar"].toBool(),"native entries default visible");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","my-files"},{"location","sidebar"},{"enabled",false}})["ok"].toBool(),"native sidebar visibility can be saved");
    check(!call("launcherList")["nativeEntries"].toArray().first().toObject()["sidebar"].toBool(),"native sidebar choice persists");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","search-action"},{"location","bottom"},{"enabled",false}})["ok"].toBool(),"native bottom action can be hidden");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","search-action"},{"location","sidebar"},{"enabled",true}})["error"]=="unsupported-native-location","native actions cannot be moved into unsupported containers");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","airplane"},{"location","quick"},{"enabled",false}})["ok"].toBool(),"native quick setting visibility is configurable");
    check(call("launcherSet",{{"id","native"},{"pageId","main"},{"location","quick"},{"enabled",true}})["ok"].toBool(),"plugin page can be pinned to quick settings");
    auto launchers=call("launcherList")["entries"].toArray();
    check(launchers.size()==3 && !launchers[2].toObject()["sidebar"].toBool() && !launchers[2].toObject()["bottom"].toBool(),"settings providers become optional shortcuts without declarations");
    check(launchers[2].toObject()["settings"].toBool(),"native declaration supplies initial settings-sidebar pin");
    defaults.locations=XEM_LAUNCHER_BOTTOM;
    auto unchanged=call("launcherList")["entries"].toArray()[2].toObject();
    check(unchanged["settings"].toBool() && !unchanged["bottom"].toBool(),"updated defaults do not overwrite captured choices");
    check(call("launcherSet",{{"id","native"},{"pageId","main"},{"location","settings"},{"enabled",false}})["ok"].toBool(),"user can override declared settings-sidebar default");
    check(!call("launcherList")["entries"].toArray()[2].toObject()["settings"].toBool(),"explicit false survives rediscovery");
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","settings"},{"enabled",false}})["error"]=="last-manager-entry","cannot hide final Extensions entry inside native Settings");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","settings"},{"location","sidebar"},{"enabled",false}})["error"]=="last-manager-entry","cannot hide the final route through native settings");
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","bottom"},{"enabled",true}})["ok"].toBool(),"direct manager bottom entry can be enabled first");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","settings"},{"location","sidebar"},{"enabled",false}})["ok"].toBool(),"settings route can be hidden when direct manager entry remains");
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","bottom"},{"enabled",false}})["error"]=="last-manager-entry","cannot remove final direct manager pin");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","settings"},{"location","sidebar"},{"enabled",true}})["ok"].toBool(),"settings route can be restored");
    call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","bottom"},{"enabled",false}});
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","notifications"},{"location","settings"},{"enabled",true}})["ok"].toBool(),"notification center can provide alternate Settings route");
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","settings"},{"enabled",false}})["ok"].toBool(),"Extensions child can be hidden while notification route remains");
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","notifications"},{"location","settings"},{"enabled",false}})["error"]=="last-manager-entry","last Settings child route is protected");
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","settings"},{"enabled",true}})["ok"].toBool(),"restore Extensions child");
    call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","notifications"},{"location","settings"},{"enabled",false}});
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","quick"},{"enabled",true}})["ok"].toBool(),"manager can be reached from quick settings");
    check(call("launcherSet",{{"id","xochitl"},{"pageId","settings"},{"location","sidebar"},{"enabled",false}})["ok"].toBool(),"quick manager entry can replace native Settings route");
    check(call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","quick"},{"enabled",false}})["error"]=="last-manager-entry","last direct quick manager entry is protected");
    call("launcherSet",{{"id","xochitl"},{"pageId","settings"},{"location","sidebar"},{"enabled",true}});
    call("launcherSet",{{"id","xovi-extension-manager"},{"pageId","inventory"},{"location","quick"},{"enabled",false}});
    const QString launchersFile=dir.path()+"/exthome/xovi-extension-manager/launchers.json";
    QFile priorLaunchers(launchersFile);check(priorLaunchers.open(QIODevice::ReadOnly),"open saved launcher fixture");const QByteArray backup=priorLaunchers.readAll();priorLaunchers.close();
    write(launchersFile,R"({"version":1,"entries":{"xochitl/settings":{"sidebar":false}}})");
    bool recovered=false;
    for(const auto &v:call("launcherList")["nativeEntries"].toArray()) { const auto entry=v.toObject();if(entry["pageId"]=="settings") recovered=entry["sidebar"].toBool(); }
    check(recovered,"legacy all-hidden config exposes a recovery Settings entry");
    write(launchersFile,backup);
    check(call("launcherSet",{{"id","native"},{"pageId","main"},{"location","sidebar"},{"enabled",true}})["ok"].toBool(),"sidebar choice saved");
    auto pinned=call("launcherList")["entries"].toArray()[2].toObject();
    check(pinned["sidebar"].toBool() && !pinned["bottom"].toBool(),"sidebar and bottom choices are independent");
    check(call("launcherSet",{{"id","native"},{"pageId","main"},{"location","bottom"},{"enabled",true}})["requiresRestart"]==false,"bottom pin applies without restart");
    check(call("launcherSet",{{"id","native"},{"pageId","main"},{"location","arbitrary"},{"enabled",true}})["error"]=="invalid-location","unsupported launcher location rejected");
    check(call("launcherSet",{{"id","native"},{"pageId","missing"},{"location","sidebar"},{"enabled",true}})["error"]=="launcher-entry-not-found","unknown page cannot be pinned");
    runtimeState=2;
    check(call("settingsList")["pages"].toArray().isEmpty(),"failed native provider not invoked");
    check(call("launcherList")["entries"].toArray().size()==2,"failed provider shortcut hidden without calling getter");runtimeState=7;
    check(call("launcherList")["entries"].toArray()[2].toObject()["bottom"].toBool(),"pin choice retained across provider absence");
    auto get=call("settingsGet",{{"id","native"}});check(get["revision"]==0,"initial revision");
    auto update=call("settingsUpdate",{{"id","native"},{"expectedRevision",0},{"values",QJsonObject{{"a",true}}}});
    check(update["ok"].toBool() && update["revision"]==1,"atomic config update");
    check(call("settingsUpdate",{{"id","native"},{"expectedRevision",0},{"values",QJsonObject{}}})["error"]=="revision-conflict","stale revision rejected");
    check(call("settingsGet",{{"id","native"}})["values"].toObject()["a"]==true,"saved config readable");
    check(call("settingsGet",{{"id","../escape"}})["ok"]==false,"path traversal rejected");
    check(call("injectionsSet",{{"id","native"},{"injectionId","button"},{"enabled",false}})["requiresRestart"]==true,"injection policy stored for restart");
    check(call("injectionsGet")["error"]=="rebuilder-unavailable","missing rebuilder is explicit");
    qrrInstalled=true;
    check(call("injectionsGet")["sessionId"]=="test-session" && releases==1,"QRR snapshot discovered and released by its allocator");
    check(call("uiReport",{{"id","native"},{"pageId","main"},{"state","failed"},{"message","bad QML"}})["ok"]==true,"page failure recorded independently");
    check(call("settingsList")["uiStates"].toObject()["native/main"].toObject()["state"]=="failed","page failure queryable");
    write(dir.path()+"/exthome/native/manager-settings.json","broken");
    check(call("settingsGet",{{"id","native"}})["error"]=="invalid-settings-file","corrupt config not overwritten");
    QString source=dir.path()+"/input";
    write(source+"/manifest.json",R"({"manifestVersion":1,"id":"qml-example","name":"QML","type":"qml","entry":"Main.qml","version":"1.0.0","enabled":false,"settings":{"launchers":{"settings":true}},"requires":{"xovi":">=0.3.0"}})");
    write(source+"/Main.qml",pageSource);
    auto installed=QJsonDocument::fromJson(QByteArray::fromStdString(installPackage((source+"/Main.qml").toStdString()))).object();
    check(installed["ok"].toBool(),"install pure QML package");
    auto enabled=QJsonDocument::fromJson(QByteArray::fromStdString(setExtensionEnabled("qml-example",true))).object();
    check(enabled["ok"].toBool() && !enabled["requiresRestart"].toBool(),"QML enable without process restart");
    auto pages=call("settingsList")["pages"].toArray();
    check(pages.size()==2 && pages.last().toObject()["available"].toBool(),"pure QML settings discovered");
    check(call("launcherList")["entries"].toArray().last().toObject()["settings"].toBool(),"manifest declaration supplies QML settings-sidebar default");
    check(QJsonDocument::fromJson(QByteArray::fromStdString(requiresRestartJson())).object()["requiresRestart"]==false,"QML changes do not set global restart flag");
    auto replaced=QJsonDocument::fromJson(QByteArray::fromStdString(installPackage((source+"/Main.qml").toStdString()))).object();
    check(replaced["ok"].toBool() && replaced["requiresRestart"].toBool(),"QML replacement reports cached component restart");
    setExtensionEnabled("qml-example",false);
    check(!call("settingsList")["pages"].toArray().last().toObject()["available"].toBool(),"disabled QML page unavailable");
    check(QJsonDocument::fromJson(QByteArray::fromStdString(removeManagedPackage("qml-example"))).object()["ok"]==true,"remove QML package");
    const auto qmdDirectory=dir.path()+"/extensions.available/qmd-with-settings";
    write(qmdDirectory+"/manifest.json",R"({"manifestVersion":1,"id":"qmd-with-settings","name":"QMD settings","type":"qmd","entry":"patch.qmd","version":"1.0.0","enabled":true,"settings":{"page":"Settings.qml","iconSource":"icons/bluetooth.svg","titleTranslations":{"zh_CN":"蓝牙"},"launchers":{"settings":true}}})");
    write(qmdDirectory+"/patch.qmd","VERSION 3.28.x.x\n");write(qmdDirectory+"/Settings.qml",pageSource);
    write(qmdDirectory+"/icons/bluetooth.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
    bool qmdDeclared=false;
    for(const auto &v:call("launcherList")["entries"].toArray()) {const auto e=v.toObject();if(e["id"]=="qmd-with-settings") qmdDeclared=e["settings"].toBool() && e["available"].toBool();}
    check(qmdDeclared,"QMD package can declare its shipped QML settings entry");
    const auto qmdEntry=call("launcherList")["entries"].toArray().last().toObject();
    check(qmdEntry["iconSource"].toString().startsWith("file:") && qmdEntry["iconSource"].toString().endsWith("icons/bluetooth.svg"),"manifest icon resolves safely inside installed package");
    check(qmdEntry["titleTranslations"].toObject()["zh_CN"]==QString::fromUtf8("蓝牙"),"package-owned localized entry titles survive discovery");
    QDir(qmdDirectory).removeRecursively();
    write(dir.path()+"/extensions.available/native-package/manifest.json",R"({"manifestVersion":1,"id":"native-package","name":"Native package","type":"extension","entry":"native.so","version":"1.0.0","enabled":true})");
    write(dir.path()+"/extensions.available/native-package/native.so","fixture");
    const auto aliasedPage=call("settingsList")["pages"].toArray().first().toObject();
    check(aliasedPage["id"]=="native" && aliasedPage["packageId"]=="native-package","runtime provider name maps to manifest package without changing settings owner");
    const auto aliasedPin=call("launcherList")["entries"].toArray().last().toObject();
    check(aliasedPin["id"]=="native" && aliasedPin["sidebar"].toBool() && aliasedPin["bottom"].toBool(),"provider alias mapping preserves saved pins");
    setExtensionEnabled("native-package",false);
    check(call("settingsList")["pages"].toArray().isEmpty(),"disabled native provider is hidden despite initialized runtime");
    check(call("launcherList")["entries"].toArray().size()==2,"disabled plugin leaves no pinned entry");
    const auto active=dir.path()+"/extensions.d/native-package.so";
    QDir().mkpath(QFileInfo(active).absolutePath());
    check(QFile::link(dir.path()+"/missing-target",active),"create stale dangling active link");
    call("launcherList");
    check(!QFileInfo(active).isSymLink(),"launcher discovery automatically repairs disabled active link");
    setExtensionEnabled("native-package",true);
    const auto restoredPin=call("launcherList")["entries"].toArray().last().toObject();
    check(restoredPin["sidebar"].toBool() && restoredPin["bottom"].toBool(),"reenabling restores pin preferences");

    QDir(dir.path()+"/extensions.available/native-package").removeRecursively();
    const auto legacy=dir.path()+"/extensions.d/native.so";
    write(legacy,"legacy fixture");
    const auto legacyRequest=QJsonDocument(QJsonObject{{"path",legacy}}).toJson(QJsonDocument::Compact);
    check(QJsonDocument::fromJson(QByteArray::fromStdString(disableLegacyPackage(legacyRequest.toStdString()))).object()["ok"].toBool(),"legacy plugin disabled");
    check(call("settingsList")["pages"].toArray().isEmpty(),"disabled legacy provider is suppressed until restart");

    const auto runtimePackage=dir.path()+"/extensions.available/runtime-package";
    write(runtimePackage+"/manifest.json",R"({"manifestVersion":1,"id":"runtime-package","name":"Runtime package","type":"qmd","entry":"pure-qmd.qmd","version":"1.0.0","enabled":true,"settings":{"page":"Settings.qml"}})");
    write(runtimePackage+"/pure-qmd.qmd","VERSION 3.28.x.x\n");
    write(runtimePackage+"/Settings.qml",pageSource);
    check(call("settingsRegister",runtimePage)["ok"].toBool(),"runtime registration with package alias");
    auto aliasPages=call("settingsList")["pages"].toArray();
    check(aliasPages.size()==1 && aliasPages.first().toObject()["packageId"]=="runtime-package" && aliasPages.first().toObject()["provider"]=="runtime","runtime QMD basename maps to package and overrides optional manifest page");
    check(QJsonDocument::fromJson(QByteArray::fromStdString(setExtensionEnabled("runtime-package",false))).object()["ok"].toBool(),"runtime QMD package disabled");
    bool visibleRuntime=false;
    for(auto v:call("settingsList")["pages"].toArray()) if(v.toObject()["provider"]=="runtime") visibleRuntime=true;
    check(!visibleRuntime,"disabled QMD hides already registered runtime page");
    call("settingsUnregister",runtimePage);

    auto loosePage=runtimePage;loosePage["id"]="loose-qmd";
    const auto loosePath=dir.path()+"/exthome/qt-resource-rebuilder/065-loose-qmd.qmd";
    write(loosePath,"VERSION 3.28.x.x\n");
    check(call("settingsRegister",loosePage)["ok"].toBool(),"loose QMD registers with no manifest");
    bool looseVisible=false;
    for(auto v:call("settingsList")["pages"].toArray()) if(v.toObject()["id"]=="loose-qmd") looseVisible=true;
    check(looseVisible,"loose QMD runtime page discovered");
    auto looseRequest=QJsonDocument(QJsonObject{{"path",loosePath}}).toJson(QJsonDocument::Compact);
    check(QJsonDocument::fromJson(QByteArray::fromStdString(disableLegacyPackage(looseRequest.toStdString()))).object()["ok"].toBool(),"loose QMD disabled without unloading MainView");
    looseVisible=false;
    for(auto v:call("settingsList")["pages"].toArray()) if(v.toObject()["id"]=="loose-qmd") looseVisible=true;
    check(!looseVisible,"disabled loose QMD runtime page is suppressed before restart");
    call("settingsUnregister",loosePage);

}
