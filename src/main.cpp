#include "diagnostics_qt.h"
#include "notifications.h"
#include "inventory.h"
#include "jsonutil.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {
    char *brokerResponse(const std::string &input) {
        const auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(input));
        const auto json=doc.isObject() ? QJsonDocument(withDiagnostic(doc.object())).toJson(QJsonDocument::Compact).toStdString() : input;
        char *buffer = static_cast<char *>(std::malloc(json.size() + 1));
        if(buffer == nullptr) return nullptr;
        std::memcpy(buffer, json.c_str(), json.size() + 1);
        return buffer;
    }
}

extern "C" void _xovi_construct() {
    reconcileDisabledEntries();
    scanDependenciesAtStartup();
}

extern "C" char *xovi_extension_manager_list(const char *) {
    auto repairs = reconcileDisabledEntries();
    auto json = inventoryToJson(loadInventory());
    json.insert(json.size()-1, ",\"automaticRepairs\":" + repairs);
    return brokerResponse(json);
}

extern "C" char *xovi_extension_manager_get(const char *value) {
    Inventory inventory = loadInventory();
    int index = findExtension(inventory, value == nullptr ? "" : value);
    if(index < 0) return brokerResponse(errorJson("not-found", "package was not found"));
    std::string package = extensionToJson(inventory, inventory.extensions[static_cast<size_t>(index)]);
    return brokerResponse(std::string("{\"ok\":true,\"package\":") + package + ",\"extension\":" + package + "}");
}

extern "C" char *xovi_extension_manager_enable(const char *value) {
    return brokerResponse(setExtensionEnabled(value == nullptr ? "" : value, true));
}

extern "C" char *xovi_extension_manager_disable(const char *value) {
    return brokerResponse(setExtensionEnabled(value == nullptr ? "" : value, false));
}

extern "C" char *xovi_extension_manager_repair(const char *value) {
    return brokerResponse(repairExtensionActiveState(value == nullptr ? "" : value));
}

extern "C" char *xovi_extension_manager_install(const char *value) {
    return brokerResponse(installPackage(value == nullptr ? "" : value));
}

extern "C" char *xovi_extension_manager_adopt(const char *value) {
    return brokerResponse(adoptLegacyPackage(value == nullptr ? "" : value));
}

extern "C" char *xovi_extension_manager_disable_legacy(const char *value) {
    return brokerResponse(disableLegacyPackage(value == nullptr ? "" : value));
}

extern "C" char *xovi_extension_manager_remove(const char *value) {
    return brokerResponse(removeManagedPackage(value == nullptr ? "" : value));
}

extern "C" char *xovi_extension_manager_requires_restart(const char *) {
    return brokerResponse(requiresRestartJson());
}

extern "C" char *xovi_extension_manager_schema(const char *) {
    return brokerResponse(schemaJson());
}

extern "C" char *xovi_extension_manager_health(const char *) {
    Inventory inventory = loadInventory();
    return brokerResponse(
        std::string("{\"ok\":true,\"root\":\"") + jsonutil::escape(xoviRoot()) +
        "\",\"count\":" + std::to_string(inventory.extensions.size()) + "}"
    );
}

#include "settings.h"
extern "C" char *xem_settingsRegister(const char *value) { return brokerResponse(settingsCommand("settingsRegister", value)); }
extern "C" char *xem_settingsUnregister(const char *value) { return brokerResponse(settingsCommand("settingsUnregister", value)); }
extern "C" char *xem_settingsList(const char *value) { return brokerResponse(settingsCommand("settingsList", value)); }
extern "C" char *xem_settingsGet(const char *value) { return brokerResponse(settingsCommand("settingsGet", value)); }
extern "C" char *xem_settingsUpdate(const char *value) { return brokerResponse(settingsCommand("settingsUpdate", value)); }
extern "C" char *xem_injectionsGet(const char *value) { return brokerResponse(settingsCommand("injectionsGet", value)); }
extern "C" char *xem_injectionsSet(const char *value) { return brokerResponse(settingsCommand("injectionsSet", value)); }
extern "C" char *xem_uiReport(const char *value) { return brokerResponse(settingsCommand("uiReport", value)); }

extern "C" char *xem_launcherList(const char *value) { return brokerResponse(settingsCommand("launcherList", value)); }
extern "C" char *xem_launcherSet(const char *value) { return brokerResponse(settingsCommand("launcherSet", value)); }

extern "C" char *xem_notificationsPost(const char *value) { return brokerResponse(notificationCommand("post", value)); }

extern "C" char *xem_notificationsList(const char *value) { return brokerResponse(notificationCommand("list", value)); }

extern "C" char *xem_notificationsRead(const char *value) { return brokerResponse(notificationCommand("markRead", value)); }

extern "C" char *xem_notificationsDismiss(const char *value) { return brokerResponse(notificationCommand("dismiss", value)); }

extern "C" char *xem_notificationsClear(const char *value) { return brokerResponse(notificationCommand("clear", value)); }

extern "C" char *xem_notificationsActionInvoke(const char *value) { return brokerResponse(notificationCommand("actionInvoke", value)); }
extern "C" char *xem_notificationsAcknowledge(const char *value) { return brokerResponse(notificationCommand("acknowledge", value)); }
