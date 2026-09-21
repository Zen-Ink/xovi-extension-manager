#include "inventory.h"
#include "diagnostics.h"
#include "jsonutil.h"
#include <algorithm>
#include "xovi.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

const XoViEnvironment *Environment = nullptr;

namespace fs = std::filesystem;

namespace {
class Fixture {
public:
    Fixture() {
        std::string pattern = (fs::temp_directory_path() / "xovi-manager-qmd-deps-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char *created = mkdtemp(writable.data());
        if(created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;
        setenv("XOVI_ROOT", root_.c_str(), 1);
        setenv("XOVI_EXTENSION_MANAGER_DISABLE_RUNTIME_API", "1", 1);
        addRuntimeExtension();
    }

    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;

    void addQmd(const std::string &id,
                const std::string &version,
                int order,
                bool enabled,
                const std::vector<std::pair<std::string, std::string>> &qmdDependencies = {},
                bool active = false) {
        addQmdWithEntry(id, version, order, id + ".qmd", enabled, qmdDependencies, active);
    }

    void addQmdWithEntry(const std::string &id,
                         const std::string &version,
                         int order,
                         const std::string &entry,
                         bool enabled,
                         const std::vector<std::pair<std::string, std::string>> &qmdDependencies = {},
                         bool active = false) {
        fs::path package = qmdPackages() / id;
        fs::create_directories(package);
        write(package / entry, "// test fixture\n");

        std::ostringstream dependencies;
        dependencies << "{";
        for(size_t i = 0; i < qmdDependencies.size(); ++i) {
            if(i != 0) dependencies << ",";
            dependencies << "\"" << qmdDependencies[i].first << "\":\""
                         << qmdDependencies[i].second << "\"";
        }
        dependencies << "}";

        std::ostringstream manifest;
        manifest << "{\n"
                 << "  \"manifestVersion\": 1,\n"
                 << "  \"type\": \"qmd\",\n"
                 << "  \"id\": \"" << id << "\",\n"
                 << "  \"name\": \"" << id << "\",\n"
                 << "  \"version\": \"" << version << "\",\n"
                 << "  \"entry\": \"" << entry << "\",\n"
                 << "  \"enabled\": " << (enabled ? "true" : "false") << ",\n"
                 << "  \"order\": " << order << ",\n"
                 << "  \"requires\": {\n"
                 << "    \"xovi\": \">=0.3.0\",\n"
                 << "    \"extensions\": {\"qt-resource-rebuilder\": \">=0.3.0\"},\n"
                 << "    \"qmd\": " << dependencies.str() << ",\n"
                 << "    \"xochitl\": [],\n"
                 << "    \"architectures\": []\n"
                 << "  }\n"
                 << "}\n";
        write(package / "manifest.json", manifest.str());

        if(active) {
            fs::create_directories(qmdActive());
            fs::create_symlink(package / entry, qmdActive() / activeName(order, id));
        }
    }

    void addLegacyQmd(const std::string &id, int order) {
        fs::create_directories(qmdActive());
        write(qmdActive() / activeName(order, id), "// unmanaged legacy fixture\n");
    }

    void addExtensionWithId(const std::string &id) {
        addExtension(id, "1.0.0", true, false);
    }

    void addExtension(const std::string &id,
                      const std::string &version,
                      bool enabled,
                      bool active) {
        addExtensionWithEntry(id, version, id + ".so", enabled, active);
    }

    void addExtensionWithEntry(const std::string &id,
                               const std::string &version,
                               const std::string &entry,
                               bool enabled,
                               bool active) {
        fs::path package = extensionPackages() / id;
        fs::create_directories(package);
        write(package / entry, "test extension fixture\n");
        write(package / "manifest.json",
              "{\"manifestVersion\":1,\"type\":\"extension\","
              "\"id\":\"" + id + "\",\"name\":\"" + id + "\","
              "\"version\":\"" + version + "\",\"entry\":\"" + entry + "\","
              "\"enabled\":" + std::string(enabled ? "true" : "false") + ",\"requires\":{\"xovi\":\">=0.3.0\","
              "\"extensions\":{},\"xochitl\":[],\"architectures\":[]}}\n");
        if(active) {
            fs::path activeDir = root_ / "extensions.d";
            fs::create_directories(activeDir);
            fs::create_symlink(package / entry, activeDir / (id + ".so"));
        }
    }

    void addExtensionWithSymlinkedSourceEntry(const std::string &id,
                                              const std::string &version,
                                              bool enabled,
                                              bool active) {
        fs::path package = extensionPackages() / id;
        fs::create_directories(package / "package");
        write(package / "package" / (id + ".so"), "test extension fixture\n");
        fs::create_symlink(fs::path("package") / (id + ".so"), package / (id + ".so"));
        write(package / "manifest.json",
              "{\"manifestVersion\":1,\"type\":\"extension\","
              "\"id\":\"" + id + "\",\"name\":\"" + id + "\","
              "\"version\":\"" + version + "\",\"entry\":\"" + id + ".so\","
              "\"enabled\":" + std::string(enabled ? "true" : "false") + ",\"requires\":{\"xovi\":\">=0.3.0\","
              "\"extensions\":{},\"xochitl\":[],\"architectures\":[]}}\n");
        if(active) {
            fs::path activeDir = root_ / "extensions.d";
            fs::create_directories(activeDir);
            fs::create_symlink(package / "package" / (id + ".so"), activeDir / (id + ".so"));
        }
    }

    void addExtensionWithXochitlRequirements(const std::string &id,
                                              const std::vector<std::string> &requirements) {
        fs::path package = extensionPackages() / id;
        fs::create_directories(package);
        write(package / (id + ".so"), "test extension fixture\n");

        std::ostringstream xochitl;
        xochitl << "[";
        for(size_t i = 0; i < requirements.size(); ++i) {
            if(i != 0) xochitl << ",";
            xochitl << "\"" << requirements[i] << "\"";
        }
        xochitl << "]";

        write(package / "manifest.json",
              "{\"manifestVersion\":1,\"type\":\"extension\","
              "\"id\":\"" + id + "\",\"name\":\"" + id + "\","
              "\"version\":\"1.0.0\",\"entry\":\"" + id + ".so\","
              "\"enabled\":false,\"requires\":{\"xovi\":\">=0.3.0\","
              "\"extensions\":{},\"xochitl\":" + xochitl.str() + ",\"architectures\":[]}}\n");
    }

    void addDuplicateQmd(const std::string &directory, const std::string &id, int order) {
        fs::path package = qmdPackages() / directory;
        fs::create_directories(package);
        write(package / (id + ".qmd"), "// duplicate fixture\n");
        std::ostringstream manifest;
        manifest << "{\"manifestVersion\":1,\"type\":\"qmd\","
                 << "\"id\":\"" << id << "\",\"name\":\"" << id << "\","
                 << "\"version\":\"1.0.0\",\"entry\":\"" << id << ".qmd\","
                 << "\"enabled\":true,\"order\":" << order << ","
                 << "\"requires\":{\"xovi\":\">=0.3.0\","
                 << "\"extensions\":{\"qt-resource-rebuilder\":\">=0.3.0\"},"
                 << "\"qmd\":{},\"xochitl\":[],\"architectures\":[]}}\n";
        write(package / "manifest.json", manifest.str());
    }

    fs::path qmdPackage(const std::string &id) const {
        return qmdPackages() / id;
    }

    fs::path extensionPackage(const std::string &id) const {
        return extensionPackages() / id;
    }

    fs::path dataHome(const std::string &id) const {
        return root_ / "exthome" / id;
    }

    bool activeQmdExists(int order, const std::string &id) const {
        return fs::exists(qmdActive() / activeName(order, id));
    }

    bool activeExtensionExists(const std::string &id) const {
        return fs::exists(root_ / "extensions.d" / (id + ".so"));
    }

    void writeDataFile(const std::string &id,
                       const std::string &relativePath,
                       const std::string &contents) {
        write(dataHome(id) / relativePath, contents);
    }

    bool dataFileExists(const std::string &id, const std::string &relativePath) const {
        return fs::exists(dataHome(id) / relativePath);
    }

    fs::path previousSelfPackage() const {
        return root_ / "exthome" / "xovi-extension-manager" /
               "state" / "previous" / "xovi-extension-manager";
    }

    std::string installQmdUpdate(const std::string &id,
                                 const std::string &version,
                                 int order,
                                 const std::vector<std::pair<std::string, std::string>> &qmdDependencies = {}) {
        fs::path incoming = root_ / "incoming" / (id + ".qmd");
        write(incoming, "// updated test fixture\n");

        std::ostringstream dependencies;
        dependencies << "{";
        for(size_t i = 0; i < qmdDependencies.size(); ++i) {
            if(i != 0) dependencies << ",";
            dependencies << "\"" << qmdDependencies[i].first << "\":\""
                         << qmdDependencies[i].second << "\"";
        }
        dependencies << "}";

        std::ostringstream manifest;
        manifest << "{\"manifestVersion\":1,\"type\":\"qmd\","
                 << "\"id\":\"" << id << "\",\"name\":\"" << id << "\","
                 << "\"version\":\"" << version << "\","
                 << "\"entry\":\"" << id << ".qmd\","
                 << "\"enabled\":true,\"order\":" << order << ","
                 << "\"requires\":{\"xovi\":\">=0.3.0\","
                 << "\"extensions\":{\"qt-resource-rebuilder\":\">=0.3.0\"},"
                 << "\"qmd\":" << dependencies.str()
                 << ",\"xochitl\":[],\"architectures\":[]}}\n";
        write(fs::path(incoming.string() + ".manifest.json"), manifest.str());
        return installPackage("{\"path\":\"" + incoming.string() + "\",\"enabled\":true}");
    }

    std::string installExtensionUpdate(const std::string &id,
                                       const std::string &version,
                                       bool enabled = true) {
        fs::path incoming = root_ / "incoming" / (id + ".so");
        write(incoming, "updated extension fixture\n");
        std::string manifest = "{\"manifestVersion\":1,\"type\":\"extension\","
            "\"id\":\"" + id + "\",\"name\":\"" + id + "\","
            "\"version\":\"" + version + "\",\"entry\":\"" + id + ".so\","
            "\"enabled\":" + std::string(enabled ? "true" : "false") + ","
            "\"requires\":{\"xovi\":\">=0.3.0\","
            "\"extensions\":{},\"xochitl\":[],\"architectures\":[]}}\n";
        write(fs::path(incoming.string() + ".manifest.json"), manifest);
        return installPackage("{\"path\":\"" + incoming.string() + "\"}");
    }

private:
    fs::path root_;

    fs::path extensionPackages() const {
        return root_ / "extensions.available";
    }

    fs::path qmdPackages() const {
        return root_ / "qmd.available";
    }

    fs::path qmdActive() const {
        return root_ / "exthome" / "qt-resource-rebuilder";
    }

    static std::string activeName(int order, const std::string &id) {
        std::ostringstream name;
        name.width(3);
        name.fill('0');
        name << order << "-" << id << ".qmd";
        return name.str();
    }

    static void write(const fs::path &path, const std::string &contents) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path);
        if(!output) throw std::runtime_error("cannot write " + path.string());
        output << contents;
    }

    void addRuntimeExtension() {
        fs::path package = extensionPackages() / "qt-resource-rebuilder";
        fs::create_directories(package);
        write(package / "qt-resource-rebuilder.so", "test fixture\n");
        write(package / "manifest.json",
              "{\"manifestVersion\":1,\"type\":\"extension\","
              "\"id\":\"qt-resource-rebuilder\",\"name\":\"qt-resource-rebuilder\","
              "\"version\":\"0.3.0\",\"entry\":\"qt-resource-rebuilder.so\","
              "\"enabled\":true,\"requires\":{\"xovi\":\">=0.3.0\","
              "\"extensions\":{},\"xochitl\":[],\"architectures\":[]}}\n");
        fs::path activeDir = root_ / "extensions.d";
        fs::create_directories(activeDir);
        fs::create_symlink(package / "qt-resource-rebuilder.so",
                           activeDir / "qt-resource-rebuilder.so");
    }
};

void require(bool condition, const std::string &message) {
    if(!condition) throw std::runtime_error(message);
}

void requireContains(const std::string &actual, const std::string &expected) {
    require(actual.find(expected) != std::string::npos,
            "expected JSON to contain " + expected + "\nactual: " + actual);
}

void requireNotContains(const std::string &actual, const std::string &unexpected) {
    require(actual.find(unexpected) == std::string::npos,
            "expected JSON not to contain " + unexpected + "\nactual: " + actual);
}

std::string packageJson(const Inventory &inventory, const std::string &id) {
    int index = findExtension(inventory, id);
    require(index >= 0, "package not found: " + id);
    return extensionToJson(inventory, inventory.extensions[static_cast<size_t>(index)]);
}

void parseAndJsonRoundTrip() {
    Fixture fixture;
    fixture.addQmd("base", "1.2.3", 10, true, {}, true);
    fixture.addQmd("consumer", "2.0.0", 20, true, {{"base", ">=1.2.0"}}, true);

    Inventory inventory = loadInventory(false);
    std::string json = packageJson(inventory, "consumer");
    requireContains(json, "\"qmd\":{\"base\":\">=1.2.0\"}");
}

void missingDependency() {
    Fixture fixture;
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"missing", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "consumer"),
                    "missing-qmd-dependency:missing");
}

void legacyDependencyDoesNotSatisfy() {
    Fixture fixture;
    fixture.addLegacyQmd("base", 10);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "consumer"),
                    "missing-qmd-dependency:base");
}

void wrongTypeDependencyDoesNotSatisfy() {
    Fixture fixture;
    fixture.addExtensionWithId("base");
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "consumer"),
                    "missing-qmd-dependency:base");
}

void duplicateDependencyDoesNotResolveArbitrarily() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);
    fixture.addDuplicateQmd("base-copy", "base", 11);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "consumer"),
                    "missing-qmd-dependency:base");
}

void disabledDependency() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, false);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "consumer"),
                    "disabled-qmd-dependency:base");
}

void effectiveDisabledDependency() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    std::string json = packageJson(loadInventory(false), "consumer");
    requireContains(json, "disabled-qmd-dependency:base");
}

void effectiveEnabledOnlyDependency() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, false, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    std::string json = packageJson(loadInventory(false), "consumer");
    requireContains(json, "disabled-qmd-dependency:base");
}

void incompatibleVersion() {
    Fixture fixture;
    fixture.addQmd("base", "1.4.9", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=2.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "consumer"),
                    "qmd-dependency-version-incompatible:base");
}

void invalidOrder() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 30, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "consumer"),
                    "qmd-dependency-order-invalid:base");
}

void directCycle() {
    Fixture fixture;
    fixture.addQmd("alpha", "1.0.0", 10, true, {{"beta", ">=1.0.0"}}, true);
    fixture.addQmd("beta", "1.0.0", 20, true, {{"alpha", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "alpha"),
                    "qmd-dependency-cycle:beta");
    requireContains(packageJson(loadInventory(false), "beta"),
                    "qmd-dependency-cycle:alpha");
}

void selfCycle() {
    Fixture fixture;
    fixture.addQmd("alpha", "1.0.0", 10, true, {{"alpha", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "alpha"),
                    "qmd-dependency-cycle:alpha");
}

void indirectCycle() {
    Fixture fixture;
    fixture.addQmd("alpha", "1.0.0", 10, true, {{"beta", ">=1.0.0"}}, true);
    fixture.addQmd("beta", "1.0.0", 20, true, {{"gamma", ">=1.0.0"}}, true);
    fixture.addQmd("gamma", "1.0.0", 30, true, {{"alpha", ">=1.0.0"}}, true);
    requireContains(packageJson(loadInventory(false), "alpha"), "qmd-dependency-cycle:");
    requireContains(packageJson(loadInventory(false), "beta"), "qmd-dependency-cycle:");
    requireContains(packageJson(loadInventory(false), "gamma"), "qmd-dependency-cycle:");
}

void successfulDependency() {
    Fixture fixture;
    fixture.addQmd("base", "1.2.3", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.2.0"}}, true);
    std::string json = packageJson(loadInventory(false), "consumer");
    requireNotContains(json, "missing-qmd-dependency:");
    requireNotContains(json, "disabled-qmd-dependency:");
    requireNotContains(json, "qmd-dependency-version-incompatible:");
    requireNotContains(json, "qmd-dependency-order-invalid:");
    requireNotContains(json, "qmd-dependency-cycle:");
}

void xochitlXSegmentsMatch() {
    Fixture fixture;
    setenv("XOVI_XOCHITL_VERSION", "3.28.0.163", 1);
    fixture.addExtensionWithXochitlRequirements(
        "x-segment-match", {"3.27.x.x", "3.28.x.x"}
    );
    fixture.addExtensionWithXochitlRequirements("exact-match", {"3.28.0.163"});
    fixture.addExtensionWithXochitlRequirements("legacy-star-match", {"3.28.*"});

    Inventory inventory = loadInventory(false);
    requireNotContains(packageJson(inventory, "x-segment-match"), "xochitl-version-incompatible");
    requireNotContains(packageJson(inventory, "exact-match"), "xochitl-version-incompatible");
    requireNotContains(packageJson(inventory, "legacy-star-match"), "xochitl-version-incompatible");
}

void xochitlXSegmentsRequireSameSegmentCount() {
    Fixture fixture;
    setenv("XOVI_XOCHITL_VERSION", "3.28.0.163", 1);
    fixture.addExtensionWithXochitlRequirements("short-x-pattern", {"3.28.x"});
    fixture.addExtensionWithXochitlRequirements("long-x-pattern", {"3.28.x.x.x"});

    Inventory inventory = loadInventory(false);
    requireContains(packageJson(inventory, "short-x-pattern"), "xochitl-version-incompatible");
    requireContains(packageJson(inventory, "long-x-pattern"), "xochitl-version-incompatible");
}

void xochitlXSegmentsRejectNonMatches() {
    Fixture fixture;
    setenv("XOVI_XOCHITL_VERSION", "3.28.0.163", 1);
    fixture.addExtensionWithXochitlRequirements("different-release", {"3.27.x.x"});
    fixture.addExtensionWithXochitlRequirements("partial-x-segment", {"3.28.0.16x"});

    Inventory inventory = loadInventory(false);
    requireContains(packageJson(inventory, "different-release"), "xochitl-version-incompatible");
    requireContains(packageJson(inventory, "partial-x-segment"), "xochitl-version-incompatible");
}

void availableRootsAreCanonical() {
    Fixture fixture;
    fixture.addExtension("native", "1.0.0", true, true);
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);

    std::string extensionJson = packageJson(loadInventory(false), "native");
    requireContains(extensionJson, "extensions.available/native");
    requireContains(extensionJson, "extensions.available/native/native.so");
    requireContains(extensionJson, "\"dataPath\":\"");
    requireContains(extensionJson, "exthome/native");

    std::string qmdJson = packageJson(loadInventory(false), "base");
    requireContains(qmdJson, "qmd.available/base");
    requireContains(qmdJson, "qmd.available/base/base.qmd");
}

void entryMayUseSafeRelativeSubdirectory() {
    Fixture fixture;
    fixture.addExtensionWithEntry("nested-native", "1.0.0", "lib/nested-native.so", true, true);
    fixture.addQmdWithEntry("nested-qmd", "1.0.0", 10, "qmd/nested-qmd.qmd", true, {}, true);

    std::string extensionJson = packageJson(loadInventory(false), "nested-native");
    requireContains(extensionJson, "\"entry\":\"lib/nested-native.so\"");
    requireContains(extensionJson, "extensions.available/nested-native/lib/nested-native.so");
    requireContains(extensionJson, "\"sourceEntryExists\":true");
    requireContains(extensionJson, "\"activeEntryMatches\":true");

    std::string qmdJson = packageJson(loadInventory(false), "nested-qmd");
    requireContains(qmdJson, "\"entry\":\"qmd/nested-qmd.qmd\"");
    requireContains(qmdJson, "qmd.available/nested-qmd/qmd/nested-qmd.qmd");
    requireContains(qmdJson, "\"sourceEntryExists\":true");
    requireContains(qmdJson, "\"activeEntryMatches\":true");
}

void sourceEntrySymlinkMatchesActiveTarget() {
    Fixture fixture;
    fixture.addExtensionWithSymlinkedSourceEntry("symlinked-native", "1.0.0", true, true);

    std::string json = packageJson(loadInventory(false), "symlinked-native");
    requireContains(json, "\"sourceEntryExists\":true");
    requireContains(json, "\"activeEntryMatches\":true");
    requireContains(json, "\"activeEntryConflict\":\"\"");
    requireNotContains(json, "entry-missing");
    requireNotContains(json, "active-entry-conflict");
}

void typedPathMatchesFullEntryBeforeBasename() {
    Fixture fixture;
    fixture.addExtensionWithEntry("flat-entry", "1.0.0", "same.so", false, false);
    fixture.addExtensionWithEntry("nested-entry", "1.0.0", "lib/same.so", false, false);

    std::string result = removeManagedPackage("{\"type\":\"extension\",\"path\":\"lib/same.so\"}");
    requireContains(result, "\"ok\":true");
    requireContains(result, "\"id\":\"nested-entry\"");
    require(!fs::exists(fixture.extensionPackage("nested-entry")),
            "full entry path did not remove the nested-entry package");
    require(fs::exists(fixture.extensionPackage("flat-entry")),
            "full entry path matched another package by basename");
}

void reinstallPreservesRuntimeDataHome() {
    Fixture fixture;
    fixture.addExtension("native", "1.0.0", true, true);
    fixture.writeDataFile("native", "data/user.json", "{\"kept\":true}\n");

    std::string result = fixture.installExtensionUpdate("native", "1.1.0");
    requireContains(result, "\"ok\":true");
    require(fixture.dataFileExists("native", "data/user.json"),
            "extension reinstall removed runtime data under exthome/<id>");
    requireContains(packageJson(loadInventory(false), "native"), "\"version\":\"1.1.0\"");
}

void selfDisableBlocked() {
    Fixture fixture;
    fixture.addExtension("xovi-extension-manager", "1.0.0", true, true);

    std::string result = setExtensionEnabled("xovi-extension-manager", false);
    requireContains(result, "\"ok\":false");
    requireContains(result, "self-disable-blocked");
    std::string json = packageJson(loadInventory(false), "xovi-extension-manager");
    requireNotContains(json, "\"disable\"");
    requireNotContains(json, "\"remove\"");
}

void selfUpgradePreservesPreviousPackage() {
    Fixture fixture;
    fixture.addExtension("xovi-extension-manager", "1.0.0", true, true);

    std::string result = fixture.installExtensionUpdate("xovi-extension-manager", "1.1.0", false);
    requireContains(result, "\"ok\":true");
    requireContains(result, "self-package-forced-enabled");
    requireContains(result, "previous-self-package-preserved");
    require(fs::exists(fixture.previousSelfPackage() / "xovi-extension-manager.so"),
            "self upgrade did not preserve previous package");
    require(fixture.activeExtensionExists("xovi-extension-manager"),
            "self upgrade removed active extension symlink");
    requireContains(packageJson(loadInventory(false), "xovi-extension-manager"), "\"version\":\"1.1.0\"");
}

void disableBlockedByReverseDependency() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);

    std::string result = setExtensionEnabled("base", false);
    requireContains(result, "\"ok\":false");
    requireContains(result, "qmd-required-by:consumer");
    requireContains(packageJson(loadInventory(false), "base"), "\"enabled\":true");
}

void removeBlockedByReverseDependency() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);

    std::string result = removeManagedPackage("base");
    requireContains(result, "\"ok\":false");
    requireContains(result, "qmd-required-by:consumer");
    require(fs::exists(fixture.qmdPackage("base")),
            "blocked remove deleted the dependency package");
}

void desiredEnabledConsumerBlocksDependencyChange() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, false);

    std::string result = setExtensionEnabled("base", false);
    requireContains(result, "\"ok\":false");
    requireContains(result, "qmd-required-by:consumer");
}

void effectiveEnabledConsumerBlocksDependencyChange() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, false, {{"base", ">=1.0.0"}}, true);

    std::string result = removeManagedPackage("base");
    requireContains(result, "\"ok\":false");
    requireContains(result, "qmd-required-by:consumer");
}

void dependencyVersionUpgradePreflight() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", "=1.0.0"}}, true);

    std::string result = fixture.installQmdUpdate("base", "2.0.0", 10);
    requireContains(result, "\"ok\":false");
    requireContains(result, "qmd-dependency-version-incompatible:base");
    requireContains(packageJson(loadInventory(false), "base"), "\"version\":\"1.0.0\"");
}

void dependencyOrderUpgradePreflight() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, true, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);

    std::string result = fixture.installQmdUpdate("base", "1.1.0", 30);
    requireContains(result, "\"ok\":false");
    requireContains(result, "qmd-dependency-order-invalid:base");
    requireContains(packageJson(loadInventory(false), "base"), "\"order\":10");
}

void activePackageOwnDependencyUpgradePreflight() {
    Fixture fixture;
    fixture.addQmd("standalone", "1.0.0", 30, true, {}, true);

    std::string result = fixture.installQmdUpdate(
        "standalone", "1.1.0", 30, {{"missing-base", ">=1.0.0"}}
    );
    requireContains(result, "\"ok\":false");
    requireContains(result, "missing-qmd-dependency:missing-base");
    requireContains(packageJson(loadInventory(false), "standalone"), "\"version\":\"1.0.0\"");
    require(fixture.activeQmdExists(30, "standalone"),
            "blocked upgrade removed the existing active QMD");
}

void activeOrderChangeRemovesStaleSymlink() {
    Fixture fixture;
    fixture.addQmd("standalone", "1.0.0", 30, true, {}, true);
    require(fixture.activeQmdExists(30, "standalone"), "initial active QMD is missing");

    std::string result = fixture.installQmdUpdate("standalone", "1.1.0", 10);
    requireContains(result, "\"ok\":true");
    require(fixture.activeQmdExists(10, "standalone"), "new-order active QMD is missing");
    require(!fixture.activeQmdExists(30, "standalone"), "old-order active QMD symlink was left stale");
}

void repairCreatesRelativeActiveSymlink() {
    Fixture fixture;
    fixture.addExtension("repairable", "1.0.0", true, false);

    std::string result = repairExtensionActiveState("repairable");
    requireContains(result, "\"ok\":true");
    requireContains(result, "active-entry-symlink-created");
    fs::path active = fixture.extensionPackage("repairable").parent_path().parent_path() /
        "extensions.d" / "repairable.so";
    require(fs::is_symlink(active), "repair did not create active extension symlink");
    require(fs::read_symlink(active).is_relative(), "repair created an absolute symlink");
    requireContains(packageJson(loadInventory(false), "repairable"), "\"effectiveEnabled\":true");
    requireContains(packageJson(loadInventory(false), "repairable"), "\"activationNeedsRepair\":false");
}

void repairIsAvailableForAbsoluteMatchingSymlink() {
    Fixture fixture;
    fixture.addExtension("absolute-link", "1.0.0", true, true);
    requireContains(packageJson(loadInventory(false), "absolute-link"), "\"availableActions\":[\"inspect\",\"disable\",\"repair\"");
    requireContains(packageJson(loadInventory(false), "absolute-link"), "\"activationIssue\":\"absolute-link\"");
    std::string result = repairExtensionActiveState("absolute-link");
    requireContains(result, "active-entry-symlink-normalized-relative");
}

void repairReplacesWrongSymlinkButPreservesRegularFile() {
    Fixture fixture;
    fixture.addExtension("repairable", "1.0.0", true, false);
    fixture.addExtension("other", "1.0.0", false, false);
    fs::path activeDir = fixture.extensionPackage("repairable").parent_path().parent_path() / "extensions.d";
    fs::create_directories(activeDir);
    fs::create_symlink(fixture.extensionPackage("other") / "other.so", activeDir / "repairable.so");

    std::string replaced = repairExtensionActiveState("repairable");
    requireContains(replaced, "\"ok\":true");
    requireContains(replaced, "active-entry-symlink-replaced");
    require(fs::read_symlink(activeDir / "repairable.so").is_relative(), "replacement symlink is absolute");

    fs::remove(activeDir / "repairable.so");
    std::ofstream output(activeDir / "repairable.so");
    output << "unknown file\n";
    output.close();
    std::string conflict = repairExtensionActiveState("repairable");
    requireContains(conflict, "\"error\":\"active-entry-conflict\"");
    require(!fs::is_symlink(activeDir / "repairable.so"), "repair overwrote unknown regular file");
}

void repairDisabledQmdRemovesOldOrderSymlink() {
    Fixture fixture;
    fixture.addQmd("old-order", "1.0.0", 30, false, {}, false);
    fs::path activeDir = fixture.qmdPackage("old-order").parent_path().parent_path() /
        "exthome" / "qt-resource-rebuilder";
    fs::create_directories(activeDir);
    fs::create_symlink(fixture.qmdPackage("old-order") / "old-order.qmd", activeDir / "010-old-order.qmd");

    std::string result = repairExtensionActiveState("old-order");
    requireContains(result, "\"ok\":true");
    requireContains(result, "stale-qmd-active-symlink-removed");
    require(!fs::exists(activeDir / "010-old-order.qmd"), "repair left old-order QMD symlink active");
}

void danglingActiveEntryIsNotEffectivelyEnabled() {
    Fixture fixture;
    fixture.addExtension("dangling", "1.0.0", true, true);
    fs::path source = fixture.extensionPackage("dangling") / "dangling.so";
    fs::path payload = fixture.extensionPackage("dangling") / "payload.so";
    fs::rename(source, payload);
    fs::create_symlink("payload.so", source);
    fs::remove(payload);
    std::string json = packageJson(loadInventory(false), "dangling");
    requireContains(json, "\"effectiveEnabled\":false");
    requireContains(json, "\"activationIssue\":\"dangling\"");
    requireContains(json, "entry-missing");
    requireContains(json, "effective-disabled");
}

void repairDisabledRemovesStaleQmdWithMissingSource() {
    Fixture fixture;
    fixture.addQmd("missing-source", "1.0.0", 30, false, {}, false);
    fs::path package = fixture.qmdPackage("missing-source");
    fs::path activeDir = package.parent_path().parent_path() / "exthome" / "qt-resource-rebuilder";
    fs::create_directories(activeDir);
    fs::create_symlink(package / "missing-source.qmd", activeDir / "010-missing-source.qmd");
    fs::remove(package / "missing-source.qmd");

    std::string result = repairExtensionActiveState("missing-source");
    requireContains(result, "\"ok\":true");
    requireContains(result, "stale-qmd-active-symlink-removed");
    require(!fs::exists(activeDir / "010-missing-source.qmd"), "repair left stale QMD with missing source");
}

void repairDisabledPreservesDisablePreflight() {
    Fixture fixture;
    fixture.addQmd("base", "1.0.0", 10, false, {}, true);
    fixture.addQmd("consumer", "1.0.0", 20, true, {{"base", ">=1.0.0"}}, true);
    requireContains(repairExtensionActiveState("base"), "\"error\":\"qmd-required-by\"");

    fixture.addExtension("xovi-extension-manager", "1.0.0", false, true);
    requireContains(repairExtensionActiveState("xovi-extension-manager"), "self-disable-blocked");
}

void disableCleansConflictingAndStaleEntries() {
    Fixture fixture;
    fixture.addExtension("disabled-auto", "1.0.0", false, true);
    auto active=fixture.extensionPackage("disabled-auto").parent_path().parent_path()/"extensions.d"/"disabled-auto.so";
    fs::remove(active); fs::create_symlink("../missing.so", active);
    requireContains(reconcileDisabledEntries(), "\"ok\":true");
    require(!fs::is_symlink(active), "auto repair left dangling disabled entry");
    require(reconcileDisabledEntries()=="[]", "auto repair is not idempotent");
    auto alias=active.parent_path()/"old-name.so";
    fs::create_symlink(fixture.extensionPackage("disabled-auto")/"disabled-auto.so",alias);
    requireContains(reconcileDisabledEntries(), "stale-active-symlink-removed");
    require(!fs::is_symlink(alias), "auto repair left old native entry name");
    fs::create_symlink("../other.so",active);
    requireContains(setExtensionEnabled("disabled-auto",false), "\"ok\":true");
    require(!fs::is_symlink(active), "disable left wrong-target symlink");
    std::ofstream(active)<<"preserve me";
    requireContains(reconcileDisabledEntries(), "active-entry-conflict");
    require(fs::is_regular_file(active), "auto repair deleted regular file");
    fixture.addQmd("stale-auto", "1.0.0", 30, false, {}, false);
    auto qmdDir=fixture.qmdPackage("stale-auto").parent_path().parent_path()/"exthome"/"qt-resource-rebuilder";
    fs::create_directories(qmdDir);
    auto old=qmdDir/"010-stale-auto.qmd";
    fs::create_symlink(fixture.qmdPackage("stale-auto")/"stale-auto.qmd",old);
    requireContains(setExtensionEnabled("stale-auto",false), "\"ok\":true");
    require(!fs::is_symlink(old), "disable left old-order QMD link");
}

void newPackageWithMissingDependencyStaysDisabled() {
    Fixture fixture;
    std::string result = fixture.installQmdUpdate(
        "new-consumer", "1.0.0", 30, {{"missing-base", ">=1.0.0"}}
    );
    requireContains(result, "\"ok\":true");
    requireContains(result, "missing-qmd-dependency:missing-base");
    std::string json = packageJson(loadInventory(false), "new-consumer");
    requireContains(json, "\"enabled\":false");
    requireContains(json, "\"effectiveEnabled\":false");
    require(!fixture.activeQmdExists(30, "new-consumer"),
            "failed enable left an active QMD symlink");
}

static int mockRuntimeState=XOVI_EXTENSION_INITIALIZED;
struct RuntimeApiFixture {
    XoViEnvironment env{};
    RuntimeApiFixture(bool scanned=true) {
        auto count=+[](){return 1;};
        auto names=+[](const char **names,int limit){if(limit>0)names[0]="qt-resource-rebuilder";return limit>0 ? 1 : 0;};
        if(scanned){env.getScannedExtensionCount=count;env.getScannedExtensionNames=names;}
        else {env.getExtensionCount=count;env.getExtensionNames=names;}
        env.getExtensionLoadState=+[](const char *){return mockRuntimeState;};
        env.getExtensionVersion=+[](const char *,unsigned char *a,unsigned char *b,unsigned char *c){*a=0;*b=3;*c=0;return 0;};
        Environment=&env;setenv("XOVI_EXTENSION_MANAGER_DISABLE_RUNTIME_API","0",1);
    }
    ~RuntimeApiFixture(){Environment=nullptr;setenv("XOVI_EXTENSION_MANAGER_DISABLE_RUNTIME_API","1",1);mockRuntimeState=XOVI_EXTENSION_INITIALIZED;}
};
void runtimeAndWarningDiagnostics() {
    Fixture f;
    f.addQmd("one","1.0.0",50,true,{},true);
    f.addQmd("two","1.0.0",50,true,{},true);
    // Reproduce a working QMD that never declared the implicit QRR dependency.
    std::ofstream(f.qmdPackage("one")/"manifest.json") << R"({"manifestVersion":1,"type":"qmd","id":"one","name":"One","version":"1.0.0","entry":"one.qmd","enabled":true,"order":50})";
    auto inv=loadInventory();
    auto output=extensionToJson(inv,inv.extensions[findExtension(inv,"one")]);
    requireNotContains(jsonutil::stringArray(jsonutil::readStringArray(output,"issues")),"missing-runtime-dependency");
    requireNotContains(jsonutil::stringArray(jsonutil::readStringArray(output,"issues")),"qmd-order-conflict");
    requireContains(jsonutil::stringArray(jsonutil::readStringArray(output,"warnings")),"qmd-order-conflict");
    const auto root=f.extensionPackage("qt-resource-rebuilder").parent_path().parent_path();
    std::ofstream(root/"extensions.d"/"vendor.so") << "fixture";
    inv=loadInventory();output=extensionToJson(inv,inv.extensions[findExtension(inv,"vendor")]);
    requireContains(output,"\"activationNeedsRepair\":false");
    requireNotContains(jsonutil::stringArray(jsonutil::readStringArray(output,"issues")),"unmanaged");
    requireContains(jsonutil::stringArray(jsonutil::readStringArray(output,"warnings")),"unmanaged");
    fs::remove(root/"extensions.d"/"qt-resource-rebuilder.so");
    fs::remove_all(f.extensionPackage("qt-resource-rebuilder"));
    inv=loadInventory();output=extensionToJson(inv,inv.extensions[findExtension(inv,"one")]);
    requireContains(jsonutil::stringArray(jsonutil::readStringArray(output,"issues")),"missing-runtime-dependency");
    for(bool scanned:{true,false}) {
        RuntimeApiFixture runtime(scanned);
        inv=loadInventory();output=extensionToJson(inv,inv.extensions[findExtension(inv,"one")]);
        requireNotContains(jsonutil::stringArray(jsonutil::readStringArray(output,"issues")),"missing-runtime-dependency");
        const auto dependency=extensionToJson(inv,inv.extensions[findExtension(inv,"qt-resource-rebuilder")]);
        requireContains(dependency,"\"activationNeedsRepair\":false");
        requireContains(dependency,"\"requiresRestart\":false");
        requireNotContains(dependency,"entry-missing");
        mockRuntimeState=XOVI_EXTENSION_LINK_FAILED;
        inv=loadInventory();output=extensionToJson(inv,inv.extensions[findExtension(inv,"one")]);
        requireContains(output,"runtime-dependency-failed:qt-resource-rebuilder");
    }
    f.addExtensionWithEntry("rebuilder-package","0.3.0","qt-resource-rebuilder.so",true,true);
    RuntimeApiFixture runtime;
    inv=loadInventory();
    require(findExtension(inv,"qt-resource-rebuilder")<0,"runtime alias must not produce a duplicate package");
    const auto &p=inv.extensions[findExtension(inv,"rebuilder-package")];
    require(p.runtime.seen && p.runtime.loadState==XOVI_EXTENSION_INITIALIZED,"runtime state should merge by .so basename");
    output=extensionToJson(inv,inv.extensions[findExtension(inv,"one")]);
    requireNotContains(output,"missing-runtime-dependency");
}

void failedRuntimeIsNotARestartFix() {
    Fixture f;
    f.addExtension("bad-elf", "1.0.0", true, true);
    auto inventory=loadInventory(false);
    auto &package=inventory.extensions[findExtension(inventory,"bad-elf")];
    for(int state:{XOVI_EXTENSION_DLOPEN_FAILED,XOVI_EXTENSION_SHOULDLOAD_FAILED,XOVI_EXTENSION_CONDITION_FAILED,XOVI_EXTENSION_DEPENDENCY_FAILED,XOVI_EXTENSION_LINK_FAILED}) {
        package.runtime.loadState=state;
        package.runtime.loadError="/lib/dependency.so: wrong ELF class: ELFCLASS32";
        const auto json=extensionToJson(inventory,package);
        requireContains(json,"\"requiresRestart\":false");
        requireContains(json,"\"restartToApply\":false");
        requireContains(json,"\"diagnostics\":[");
        if(state==XOVI_EXTENSION_DLOPEN_FAILED) {
            requireContains(json,"elf-class-mismatch");
            requireContains(json,"\"severity\":\"error\"");
            requireContains(json,"\"action\":\"replace-build\"");
            requireContains(json,"/lib/dependency.so");
        }
    }
    package.runtime.loadState=XOVI_EXTENSION_INITIALIZED;
    requireContains(extensionToJson(inventory,package),"\"requiresRestart\":false");
    // A real saved change remains restartable; the error is still reported independently.
    const auto disabled=setExtensionEnabled("bad-elf",false);
    requireContains(disabled,"\"ok\":true");
    const auto enabled=setExtensionEnabled("bad-elf",true);
    requireContains(enabled,"\"ok\":true");
    inventory=loadInventory(false);
    auto changed=inventory.extensions[findExtension(inventory,"bad-elf")];
    changed.runtime.loadState=XOVI_EXTENSION_DLOPEN_FAILED;
    changed.runtime.loadError="wrong ELF class";
    const auto output=extensionToJson(inventory,changed);
    requireContains(output,"\"pendingChange\":true");
    requireContains(output,"\"requiresRestart\":true");
    requireContains(output,"elf-class-mismatch");
}
void diagnosticRecoveryIsSpecific() {
    require(xem::classify("missing-manifest","","warning").severity=="warning","inventory metadata is warning");
    require(xem::classify("missing-manifest").severity=="error","manifest-required operation is error");
    require(xem::classify("runtime-dlopen-failed","undefined symbol: qt_symbol").causeCode=="abi-mismatch","symbol failure classified");
    require(xem::classify("runtime-dlopen-failed","libfoo.so: No such file").causeCode=="loader-file-missing","missing library classified");
    require(xem::classify("runtime-dlopen-failed","Permission denied").causeCode=="loader-permission-denied","permissions classified");
    require(xem::classify("runtime-dlopen-failed","file too short").causeCode=="invalid-elf","corrupt binary classified");
    require(xem::classify("revision-conflict").retryable,"revision conflict is retryable after refresh");
    require(xem::classify("shadowed-by-qrr").severity=="warning","resource override is not automatically fatal");
    require(xem::classify("target-not-seen").severity=="info","unseen lazy resource is informational");
    require(xem::classify("unknown-new-code").action=="inspect","unknown cause never recommends reboot");
    require(xem::classify("unknown-new-failed").category=="unknown","unknown failure is not guessed to be a filesystem error");
    requireContains(errorJson("write-failed","read-only filesystem"),"\"diagnostic\":{");
}

void schemaAdvertisesQmdDependencies() {
    std::string schema = schemaJson();
    requireContains(schema, "\"qmd\":{\"scroll-screen-up-or-down\":\">=0.1.2\"}");
    requireContains(schema, "\"xochitl\":[\"3.27.x.x\",\"3.28.x.x\"]");
    requireContains(schema, "lowercase x as a complete dot-delimited segment with the same segment count");
    requireContains(schema, "\"repair\":\"reconcile active symlinks with manifest.enabled using relative targets\"");
}
}

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"runtime failure is not a restart fix", failedRuntimeIsNotARestartFix},
        {"specific diagnostic recovery", diagnosticRecoveryIsSpecific},
        {"runtime aliases, implicit dependencies and warning severity",runtimeAndWarningDiagnostics},
        {"requires.qmd parse and JSON", parseAndJsonRoundTrip},
        {"missing dependency", missingDependency},
        {"legacy dependency does not satisfy", legacyDependencyDoesNotSatisfy},
        {"wrong-type dependency does not satisfy", wrongTypeDependencyDoesNotSatisfy},
        {"duplicate dependency is deterministic", duplicateDependencyDoesNotResolveArbitrarily},
        {"disabled dependency", disabledDependency},
        {"effective-disabled dependency", effectiveDisabledDependency},
        {"effectiveEnabled-only dependency", effectiveEnabledOnlyDependency},
        {"incompatible version", incompatibleVersion},
        {"invalid order", invalidOrder},
        {"self cycle", selfCycle},
        {"direct cycle", directCycle},
        {"indirect cycle", indirectCycle},
        {"successful dependency", successfulDependency},
        {"xochitl x segments match", xochitlXSegmentsMatch},
        {"xochitl x segments require same count", xochitlXSegmentsRequireSameSegmentCount},
        {"xochitl x segments reject non-matches", xochitlXSegmentsRejectNonMatches},
        {"available roots are canonical", availableRootsAreCanonical},
        {"entry may use safe relative subdirectory", entryMayUseSafeRelativeSubdirectory},
        {"source entry symlink matches active target", sourceEntrySymlinkMatchesActiveTarget},
        {"typed path matches full entry before basename", typedPathMatchesFullEntryBeforeBasename},
        {"reinstall preserves runtime data home", reinstallPreservesRuntimeDataHome},
        {"self disable is blocked", selfDisableBlocked},
        {"self upgrade preserves previous package", selfUpgradePreservesPreviousPackage},
        {"reverse dependency blocks disable", disableBlockedByReverseDependency},
        {"reverse dependency blocks remove", removeBlockedByReverseDependency},
        {"desired-enabled consumer blocks dependency change", desiredEnabledConsumerBlocksDependencyChange},
        {"effectiveEnabled-only consumer blocks dependency change", effectiveEnabledConsumerBlocksDependencyChange},
        {"dependency version upgrade preflight", dependencyVersionUpgradePreflight},
        {"dependency order upgrade preflight", dependencyOrderUpgradePreflight},
        {"active package own dependency upgrade preflight", activePackageOwnDependencyUpgradePreflight},
        {"active order change removes stale symlink", activeOrderChangeRemovesStaleSymlink},
        {"repair creates relative active symlink", repairCreatesRelativeActiveSymlink},
        {"repair is available for absolute matching symlink", repairIsAvailableForAbsoluteMatchingSymlink},
        {"repair replaces symlink and preserves regular file", repairReplacesWrongSymlinkButPreservesRegularFile},
        {"repair disabled QMD removes old order symlink", repairDisabledQmdRemovesOldOrderSymlink},
        {"dangling active entry is not enabled", danglingActiveEntryIsNotEffectivelyEnabled},
        {"repair removes stale QMD with missing source", repairDisabledRemovesStaleQmdWithMissingSource},
        {"repair disabled preserves preflight", repairDisabledPreservesDisablePreflight},
        {"automatic disabled entry cleanup", disableCleansConflictingAndStaleEntries},
        {"new package with missing dependency stays disabled", newPackageWithMissingDependencyStaysDisabled},
        {"schema advertises qmd dependencies", schemaAdvertisesQmdDependencies},
    };

    int failed = 0;
    for(const auto &test : tests) {
        try {
            test.second();
            std::cout << "PASS  " << test.first << "\n";
        } catch(const std::exception &error) {
            ++failed;
            std::cerr << "FAIL  " << test.first << "\n" << error.what() << "\n";
        }
    }
    std::cout << (tests.size() - static_cast<size_t>(failed)) << "/"
              << tests.size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
