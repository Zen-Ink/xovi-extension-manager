#include "inventory.h"
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
        fs::path package = qmdPackages() / id;
        fs::create_directories(package);
        write(package / (id + ".qmd"), "// test fixture\n");

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
                 << "  \"entry\": \"" << id << ".qmd\",\n"
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
            fs::create_symlink(package / (id + ".qmd"), qmdActive() / activeName(order, id));
        }
    }

    void addLegacyQmd(const std::string &id, int order) {
        fs::create_directories(qmdActive());
        write(qmdActive() / activeName(order, id), "// unmanaged legacy fixture\n");
    }

    void addExtensionWithId(const std::string &id) {
        fs::path package = root_ / "exthome" / id;
        fs::create_directories(package);
        write(package / (id + ".so"), "test extension fixture\n");
        write(package / "manifest.json",
              "{\"manifestVersion\":1,\"type\":\"extension\","
              "\"id\":\"" + id + "\",\"name\":\"" + id + "\","
              "\"version\":\"1.0.0\",\"entry\":\"" + id + ".so\","
              "\"enabled\":true,\"requires\":{\"xovi\":\">=0.3.0\","
              "\"extensions\":{},\"xochitl\":[],\"architectures\":[]}}\n");
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

    bool activeQmdExists(int order, const std::string &id) const {
        return fs::exists(qmdActive() / activeName(order, id));
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

private:
    fs::path root_;

    fs::path qmdPackages() const {
        return root_ / "exthome" / "xovi-extension-manager" / "qmd";
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
        fs::path package = root_ / "exthome" / "qt-resource-rebuilder";
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

void schemaAdvertisesQmdDependencies() {
    std::string schema = schemaJson();
    requireContains(schema, "\"qmd\":{\"scroll-screen-up-or-down\":\">=0.1.2\"}");
}
}

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
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
        {"reverse dependency blocks disable", disableBlockedByReverseDependency},
        {"reverse dependency blocks remove", removeBlockedByReverseDependency},
        {"desired-enabled consumer blocks dependency change", desiredEnabledConsumerBlocksDependencyChange},
        {"effectiveEnabled-only consumer blocks dependency change", effectiveEnabledConsumerBlocksDependencyChange},
        {"dependency version upgrade preflight", dependencyVersionUpgradePreflight},
        {"dependency order upgrade preflight", dependencyOrderUpgradePreflight},
        {"active package own dependency upgrade preflight", activePackageOwnDependencyUpgradePreflight},
        {"active order change removes stale symlink", activeOrderChangeRemovesStaleSymlink},
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
