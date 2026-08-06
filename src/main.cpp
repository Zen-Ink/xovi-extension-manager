#include "inventory.h"
#include "jsonutil.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {
    char *brokerResponse(const std::string &json) {
        char *buffer = static_cast<char *>(std::malloc(json.size() + 1));
        if(buffer == nullptr) return nullptr;
        std::memcpy(buffer, json.c_str(), json.size() + 1);
        return buffer;
    }
}

extern "C" void _xovi_construct() {
    scanDependenciesAtStartup();
}

extern "C" char *xovi_extension_manager_list(const char *) {
    return brokerResponse(inventoryToJson(loadInventory()));
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
