#pragma once

#include <map>
#include <string>
#include <vector>

enum PackageType {
    PACKAGE_EXTENSION,
    PACKAGE_QMD,
    PACKAGE_QML,
    PACKAGE_UNKNOWN,
};

struct RuntimeState {
    std::string name;
    bool seen = false;
    int loadState = -1;
    std::string loadStateName = "not-scanned";
    std::string loadError;
    std::string version;
};

struct ExtensionManifest {
    bool hasManifest = false;
    bool valid = true;
    std::string directoryName;
    std::string directoryPath;
    std::string manifestPath;
    PackageType type = PACKAGE_EXTENSION;
    std::string typeName = "extension";
    bool managed = true;
    std::string source = "manifest";
    std::string id;
    std::string name;
    std::string version = "unknown";
    std::string author;
    std::string description;
    std::string license;
    std::string entry;
    int order = 50;
    bool enabled = true;
    std::string requiresXovi;
    std::map<std::string, std::string> requiresExtensions;
    std::map<std::string, std::string> requiresQmd;
    std::vector<std::string> requiresXochitl;
    std::vector<std::string> requiresArchitectures;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::string sourceEntryPathOverride;
    std::string enabledEntryPathOverride;
    RuntimeState runtime;
};

struct Inventory {
    std::string root;
    std::string architecture;
    std::string xochitlVersion;
    std::vector<ExtensionManifest> extensions;
};

std::string xoviRoot();
std::string sourceEntryPath(const ExtensionManifest &manifest);
std::string enabledEntryPath(const ExtensionManifest &manifest);
bool isPathPresent(const std::string &path);

Inventory loadInventory(bool includeRuntime = true);
std::string inventoryToJson(const Inventory &inventory);
std::string extensionToJson(const Inventory &inventory, const ExtensionManifest &extension);
int findExtension(const Inventory &inventory, const std::string &id);
std::string setExtensionEnabled(const std::string &id, bool enabled);
std::string repairExtensionActiveState(const std::string &id);
std::string installPackage(const std::string &request);
std::string adoptLegacyPackage(const std::string &request);
std::string disableLegacyPackage(const std::string &request);
std::string removeManagedPackage(const std::string &request);
std::string requiresRestartJson();
void scanDependenciesAtStartup();
std::string reconcileDisabledEntries();
bool isSettingsProviderSuppressed(const std::string &runtimeName);
std::string schemaJson();
std::string errorJson(const std::string &code, const std::string &message);
