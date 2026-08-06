#include "inventory.h"

#include "jsonutil.h"
#include "xovi.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <spawn.h>
#include <set>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define XOVI_ROOT_DEFAULT "/home/root/xovi"

extern char **environ;

namespace {
    static bool PENDING_RESTART = false;
    static std::set<std::string> PENDING_RESTART_PACKAGES;

    std::string trim(const std::string &value) {
        size_t start = 0;
        while(start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
        size_t end = value.size();
        while(end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
        return value.substr(start, end - start);
    }

    std::string joinPath(const std::string &left, const std::string &right) {
        if(left.empty() || left[left.size() - 1] == '/') return left + right;
        return left + "/" + right;
    }

    bool isDirectory(const std::string &path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool isSymlink(const std::string &path) {
        struct stat st;
        return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
    }

    bool isRegularFile(const std::string &path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }

    std::string parentPath(const std::string &path) {
        size_t slash = path.find_last_of('/');
        if(slash == std::string::npos) return ".";
        if(slash == 0) return "/";
        return path.substr(0, slash);
    }

    std::string absolutePathFrom(const std::string &baseDir, const std::string &path) {
        if(path.empty() || path[0] == '/') return path;
        return joinPath(baseDir, path);
    }

    bool pathsReferToSameFile(const std::string &left, const std::string &right) {
        struct stat leftStat;
        struct stat rightStat;
        if(stat(left.c_str(), &leftStat) != 0 || stat(right.c_str(), &rightStat) != 0) return false;
        return leftStat.st_dev == rightStat.st_dev && leftStat.st_ino == rightStat.st_ino;
    }

    bool readSymlinkTarget(const std::string &path, std::string &target, std::string &error) {
        char buffer[4096];
        ssize_t length = readlink(path.c_str(), buffer, sizeof(buffer) - 1);
        if(length < 0) {
            error = std::strerror(errno);
            return false;
        }
        buffer[length] = 0;
        target = buffer;
        return true;
    }

    std::string readFile(const std::string &path) {
        std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
        if(!input) return "";
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

    bool writeFile(const std::string &path, const std::string &contents, std::string &error) {
        std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if(!output) {
            error = std::strerror(errno);
            return false;
        }
        output << contents;
        if(!output.good()) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    }

    bool validId(const std::string &id) {
        if(id.empty()) return false;
        for(unsigned char c : id) {
            if(std::isalnum(c) || c == '-' || c == '_' || c == '.') continue;
            return false;
        }
        return id != "." && id != "..";
    }

    bool hasSuffix(const std::string &value, const std::string &suffix) {
        return value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    PackageType packageTypeFromString(const std::string &type) {
        if(type == "extension") return PACKAGE_EXTENSION;
        if(type == "qmd") return PACKAGE_QMD;
        return PACKAGE_UNKNOWN;
    }

    std::string packageTypeName(PackageType type) {
        switch(type) {
            case PACKAGE_EXTENSION: return "extension";
            case PACKAGE_QMD: return "qmd";
            default: return "unknown";
        }
    }

    PackageType inferPackageType(const std::string &entry) {
        if(hasSuffix(entry, ".so")) return PACKAGE_EXTENSION;
        if(hasSuffix(entry, ".qmd")) return PACKAGE_QMD;
        return PACKAGE_UNKNOWN;
    }

    bool validRelativeEntry(const std::string &entry) {
        if(entry.empty() || entry[0] == '/') return false;
        size_t start = 0;
        while(start <= entry.size()) {
            size_t slash = entry.find('/', start);
            std::string part = entry.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if(part.empty() || part == "." || part == "..") return false;
            if(slash == std::string::npos) break;
            start = slash + 1;
        }
        return true;
    }

    bool validEntryForType(const std::string &entry, PackageType type) {
        if(!validRelativeEntry(entry)) return false;
        if(type == PACKAGE_EXTENSION) return hasSuffix(entry, ".so");
        if(type == PACKAGE_QMD) return hasSuffix(entry, ".qmd");
        return false;
    }

    std::string extensionPackageRoot() {
        return joinPath(xoviRoot(), "extensions.available");
    }

    std::string qmdPackageRoot() {
        return joinPath(xoviRoot(), "qmd.available");
    }

    std::string qmdActiveDir() {
        return joinPath(joinPath(xoviRoot(), "exthome"), "qt-resource-rebuilder");
    }

    std::string persistentDataDir(const ExtensionManifest &manifest) {
        return joinPath(joinPath(xoviRoot(), "exthome"), manifest.id);
    }

    std::string managerStateRoot() {
        return joinPath(joinPath(joinPath(xoviRoot(), "exthome"), "xovi-extension-manager"), "state");
    }

    std::string previousSelfPackageDir() {
        return joinPath(joinPath(managerStateRoot(), "previous"), "xovi-extension-manager");
    }

    std::string qmdActiveFileName(const ExtensionManifest &manifest) {
        std::ostringstream name;
        name.width(3);
        name.fill('0');
        name << manifest.order << "-" << manifest.id << ".qmd";
        return name.str();
    }

    struct ActiveEntryState {
        bool present = false;
        bool symlink = false;
        bool matchesSource = false;
        std::string target;
        std::string conflict;
    };

    ActiveEntryState inspectActiveEntry(const ExtensionManifest &manifest) {
        ActiveEntryState state;
        std::string active = enabledEntryPath(manifest);
        std::string source = sourceEntryPath(manifest);
        struct stat st;
        if(lstat(active.c_str(), &st) != 0) return state;

        state.present = true;
        state.symlink = S_ISLNK(st.st_mode);
        if(state.symlink) {
            std::string error;
            if(!readSymlinkTarget(active, state.target, error)) {
                state.conflict = "active-entry-unreadable";
                return state;
            }
            std::string absoluteTarget = absolutePathFrom(parentPath(active), state.target);
            if(state.target == source || absoluteTarget == source || pathsReferToSameFile(absoluteTarget, source)) {
                state.matchesSource = true;
            } else {
                state.conflict = "active-entry-conflict";
            }
            return state;
        }

        if(isRegularFile(active) && pathsReferToSameFile(active, source)) {
            state.matchesSource = true;
        } else {
            state.conflict = "active-entry-conflict";
        }
        return state;
    }

    bool effectiveEnabled(const ExtensionManifest &manifest) {
        return inspectActiveEntry(manifest).matchesSource;
    }

    void markRestartPending(const ExtensionManifest &manifest) {
        PENDING_RESTART = true;
        if(!manifest.id.empty()) PENDING_RESTART_PACKAGES.insert(manifest.id);
    }

    bool mkdirRecursive(const std::string &path) {
        if(path.empty()) return false;
        if(isDirectory(path)) return true;

        std::string current;
        size_t cursor = 0;
        if(path[0] == '/') {
            current = "/";
            cursor = 1;
        }
        while(cursor <= path.size()) {
            size_t slash = path.find('/', cursor);
            std::string part = path.substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
            if(!part.empty()) {
                if(!current.empty() && current[current.size() - 1] != '/') current += "/";
                current += part;
                if(!isDirectory(current) && mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                    return false;
                }
            }
            if(slash == std::string::npos) break;
            cursor = slash + 1;
        }
        return true;
    }

    bool copyFile(const std::string &source, const std::string &target, std::string &error) {
        std::ifstream input(source.c_str(), std::ios::in | std::ios::binary);
        if(!input) {
            error = "cannot open source: " + std::string(std::strerror(errno));
            return false;
        }
        std::ofstream output(target.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if(!output) {
            error = "cannot open target: " + std::string(std::strerror(errno));
            return false;
        }
        output << input.rdbuf();
        if(!output.good()) {
            error = "copy failed: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }

    bool copyDirectory(const std::string &source, const std::string &target, std::string &error) {
        if(!mkdirRecursive(target)) {
            error = "cannot create target directory";
            return false;
        }

        DIR *dir = opendir(source.c_str());
        if(dir == nullptr) {
            error = "cannot open source directory: " + std::string(std::strerror(errno));
            return false;
        }

        bool ok = true;
        struct dirent *entry;
        while((entry = readdir(dir)) != nullptr) {
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            std::string sourcePath = joinPath(source, entry->d_name);
            std::string targetPath = joinPath(target, entry->d_name);

            struct stat st;
            if(lstat(sourcePath.c_str(), &st) != 0) {
                error = "cannot stat " + sourcePath + ": " + std::string(std::strerror(errno));
                ok = false;
                break;
            }
            if(S_ISDIR(st.st_mode)) {
                ok = copyDirectory(sourcePath, targetPath, error);
            } else if(S_ISREG(st.st_mode)) {
                ok = copyFile(sourcePath, targetPath, error);
            }
            if(!ok) break;
        }
        closedir(dir);
        return ok;
    }

    bool startsWith(const char *value, const char *prefix) {
        return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
    }

    bool dropForChildProcess(const char *entry) {
        return startsWith(entry, "LD_PRELOAD=") ||
            startsWith(entry, "LD_AUDIT=") ||
            startsWith(entry, "XOVI_DISABLE=");
    }

    std::vector<std::string> childEnvironment() {
        std::vector<std::string> result;
        for(char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            if(dropForChildProcess(*entry)) continue;
            result.emplace_back(*entry);
        }
        result.emplace_back("XOVI_DISABLE=1");
        return result;
    }

    bool runCommandNoXovi(const std::vector<std::string> &args, std::string &error) {
        if(args.empty()) {
            error = "empty command";
            return false;
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for(const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);

        std::vector<std::string> envStrings = childEnvironment();
        std::vector<char *> envp;
        envp.reserve(envStrings.size() + 1);
        for(std::string &entry : envStrings) envp.push_back(const_cast<char *>(entry.c_str()));
        envp.push_back(nullptr);

        pid_t pid = 0;
        int spawnResult = posix_spawnp(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), envp.data());
        if(spawnResult != 0) {
            error = std::strerror(spawnResult);
            return false;
        }

        int status = 0;
        while(waitpid(pid, &status, 0) < 0) {
            if(errno == EINTR) continue;
            error = std::strerror(errno);
            return false;
        }

        if(WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
        if(WIFEXITED(status)) {
            error = "command exited with status " + std::to_string(WEXITSTATUS(status));
            return false;
        }
        if(WIFSIGNALED(status)) {
            error = "command terminated by signal " + std::to_string(WTERMSIG(status));
            return false;
        }
        error = "command did not exit normally";
        return false;
    }

    bool removeTree(const std::string &path, std::string &error) {
        struct stat st;
        if(lstat(path.c_str(), &st) != 0) {
            if(errno == ENOENT) return true;
            error = std::strerror(errno);
            return false;
        }

        if(S_ISDIR(st.st_mode)) {
            DIR *dir = opendir(path.c_str());
            if(dir == nullptr) {
                error = std::strerror(errno);
                return false;
            }
            struct dirent *entry;
            while((entry = readdir(dir)) != nullptr) {
                if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
                if(!removeTree(joinPath(path, entry->d_name), error)) {
                    closedir(dir);
                    return false;
                }
            }
            closedir(dir);
            if(rmdir(path.c_str()) != 0) {
                error = std::strerror(errno);
                return false;
            }
            return true;
        }

        if(unlink(path.c_str()) != 0) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    }

    void cleanupTempDir(const std::string &path) {
        std::string ignored;
        removeTree(path, ignored);
    }

    bool renamePath(const std::string &from, const std::string &to, std::string &error) {
        if(rename(from.c_str(), to.c_str()) == 0) return true;
        error = std::strerror(errno);
        return false;
    }

    bool replacePackageDirectory(
        const std::string &sourceDir,
        const std::string &targetDir,
        bool preservePrevious,
        std::vector<std::string> &actions,
        std::string &error
    ) {
        std::string parent = parentPath(targetDir);
        if(!mkdirRecursive(parent)) {
            error = "cannot create package root";
            return false;
        }

        size_t slash = targetDir.find_last_of('/');
        std::string name = slash == std::string::npos ? targetDir : targetDir.substr(slash + 1);
        std::string staging = joinPath(parent, ".xovi-install-staged-" + name);
        std::string backup = joinPath(parent, ".xovi-install-backup-" + name);
        removeTree(staging, error);
        removeTree(backup, error);
        error.clear();

        if(!copyDirectory(sourceDir, staging, error)) {
            removeTree(staging, error);
            return false;
        }

        bool hadTarget = isPathPresent(targetDir);
        if(hadTarget && !renamePath(targetDir, backup, error)) {
            removeTree(staging, error);
            return false;
        }

        if(!renamePath(staging, targetDir, error)) {
            std::string rollbackError;
            removeTree(targetDir, rollbackError);
            if(hadTarget) renamePath(backup, targetDir, rollbackError);
            return false;
        }

        actions.push_back(hadTarget ? "package-directory-replaced" : "package-directory-created");

        if(hadTarget) {
            if(preservePrevious) {
                std::string previous = previousSelfPackageDir();
                std::string previousParent = parentPath(previous);
                if(!mkdirRecursive(previousParent)) {
                    actions.push_back("previous-self-package-preserve-failed");
                    removeTree(backup, error);
                    error.clear();
                } else {
                    removeTree(previous, error);
                    error.clear();
                    if(renamePath(backup, previous, error)) {
                        actions.push_back("previous-self-package-preserved");
                    } else {
                        actions.push_back("previous-self-package-preserve-failed");
                        removeTree(backup, error);
                        error.clear();
                    }
                }
            } else {
                if(!removeTree(backup, error)) return false;
            }
        }

        return true;
    }

    bool extractArchive(const std::string &archivePath, const std::string &targetDir, std::string &error) {
        if(hasSuffix(archivePath, ".tar.gz") || hasSuffix(archivePath, ".tgz")) {
            return runCommandNoXovi({"tar", "-xzf", archivePath, "-C", targetDir}, error);
        }
        if(hasSuffix(archivePath, ".zip")) {
            return runCommandNoXovi({"unzip", "-q", archivePath, "-d", targetDir}, error);
        }
        error = "unsupported archive type";
        return false;
    }

    std::string findManifestRecursive(const std::string &root, int depth = 0) {
        if(depth > 3) return "";
        std::string candidate = joinPath(root, "manifest.json");
        if(isPathPresent(candidate)) return candidate;

        DIR *dir = opendir(root.c_str());
        if(dir == nullptr) return "";
        std::string result;
        struct dirent *entry;
        while((entry = readdir(dir)) != nullptr && result.empty()) {
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            std::string child = joinPath(root, entry->d_name);
            if(isDirectory(child)) result = findManifestRecursive(child, depth + 1);
        }
        closedir(dir);
        return result;
    }

    std::string directoryName(const std::string &path) {
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void warnIfMissing(const std::string &json, const std::string &key, ExtensionManifest &manifest) {
        if(!jsonutil::hasKey(json, key)) manifest.warnings.push_back("missing-" + key);
    }

    std::string currentArchitecture() {
        struct utsname info;
        if(uname(&info) != 0) return "";
        return info.machine;
    }

    std::string currentXochitlVersion() {
        const char *envVersion = std::getenv("XOVI_XOCHITL_VERSION");
        if(envVersion != nullptr && *envVersion != 0) return envVersion;

        std::string updateConf = readFile("/usr/share/remarkable/update.conf");
        const char *keys[] = { "REMARKABLE_RELEASE_VERSION=", "RELEASE_VERSION=", "VERSION=" };
        for(const char *key : keys) {
            size_t pos = updateConf.find(key);
            if(pos == std::string::npos) continue;
            pos += std::strlen(key);
            size_t end = updateConf.find_first_of("\r\n", pos);
            return trim(updateConf.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        }
        return "";
    }

    bool parseSemver(const std::string &version, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        std::stringstream stream(version);
        std::string token;
        for(int i = 0; i < 3; ++i) {
            if(!std::getline(stream, token, '.')) return false;
            if(token.empty()) return false;
            for(unsigned char c : token) {
                if(!std::isdigit(c)) return false;
            }
            out[i] = std::atoi(token.c_str());
        }
        return stream.eof();
    }

    int compareSemver(const std::string &left, const std::string &right) {
        int l[3], r[3];
        if(!parseSemver(left, l) || !parseSemver(right, r)) return 0;
        for(int i = 0; i < 3; ++i) {
            if(l[i] < r[i]) return -1;
            if(l[i] > r[i]) return 1;
        }
        return 0;
    }

    bool versionSatisfies(const std::string &installed, const std::string &requirement) {
        if(requirement.empty() || installed.empty() || installed == "unknown") return true;
        std::string value = trim(requirement);
        if(value.rfind(">=", 0) == 0) return compareSemver(installed, trim(value.substr(2))) >= 0;
        if(value.rfind(">", 0) == 0) return compareSemver(installed, trim(value.substr(1))) > 0;
        if(value.rfind("=", 0) == 0) return compareSemver(installed, trim(value.substr(1))) == 0;
        return compareSemver(installed, value) == 0;
    }

    bool architectureMatches(const std::string &wanted, const std::string &actual) {
        if(wanted == actual) return true;
        if(wanted == "arm" && actual.find("arm") == 0) return true;
        if(wanted == "armv7" && actual.find("armv7") == 0) return true;
        if(wanted == "aarch64" && actual == "arm64") return true;
        if(wanted == "arm64" && actual == "aarch64") return true;
        return false;
    }

    bool wildcardMatches(const std::string &pattern, const std::string &value) {
        std::string wanted = trim(pattern);
        if(wanted.empty() || value.empty()) return false;
        if(wanted == "*") return true;

        size_t patternIndex = 0;
        size_t valueIndex = 0;
        size_t starIndex = std::string::npos;
        size_t retryValueIndex = 0;

        while(valueIndex < value.size()) {
            if(patternIndex < wanted.size() && wanted[patternIndex] == value[valueIndex]) {
                ++patternIndex;
                ++valueIndex;
            } else if(patternIndex < wanted.size() && wanted[patternIndex] == '*') {
                starIndex = patternIndex++;
                retryValueIndex = valueIndex;
            } else if(starIndex != std::string::npos) {
                patternIndex = starIndex + 1;
                valueIndex = ++retryValueIndex;
            } else {
                return false;
            }
        }

        while(patternIndex < wanted.size() && wanted[patternIndex] == '*') ++patternIndex;
        return patternIndex == wanted.size();
    }

    bool xochitlVersionMatches(const std::vector<std::string> &requirements, const std::string &actual) {
        for(const std::string &wanted : requirements) {
            if(wildcardMatches(wanted, actual)) return true;
        }
        return false;
    }

    std::string loadStateName(int state) {
        switch(state) {
            case XOVI_EXTENSION_DISCOVERED: return "discovered";
            case XOVI_EXTENSION_DLOPEN_FAILED: return "dlopen-failed";
            case XOVI_EXTENSION_SHOULDLOAD_FAILED: return "shouldload-failed";
            case XOVI_EXTENSION_CONDITION_FAILED: return "condition-failed";
            case XOVI_EXTENSION_DEPENDENCY_FAILED: return "dependency-failed";
            case XOVI_EXTENSION_LINK_FAILED: return "link-failed";
            case XOVI_EXTENSION_INITIALIZED: return "initialized";
            default: return "not-scanned";
        }
    }

    ExtensionManifest parseManifest(const std::string &extensionDir, const std::string &path, PackageType defaultType = PACKAGE_UNKNOWN) {
        std::string json = readFile(path);

        ExtensionManifest manifest;
        manifest.hasManifest = true;
        manifest.directoryPath = extensionDir;
        manifest.directoryName = directoryName(extensionDir);
        manifest.manifestPath = path;

        if(!jsonutil::isObject(json)) {
            manifest.valid = false;
            manifest.id = manifest.directoryName;
            manifest.name = manifest.directoryName;
            manifest.type = defaultType == PACKAGE_UNKNOWN ? PACKAGE_EXTENSION : defaultType;
            manifest.typeName = packageTypeName(manifest.type);
            manifest.entry = manifest.directoryName + (manifest.type == PACKAGE_QMD ? ".qmd" : ".so");
            manifest.errors.push_back("invalid-json-object");
            return manifest;
        }

        if(!jsonutil::hasKey(json, "manifestVersion")) manifest.warnings.push_back("missing-manifestVersion");
        if(!jsonutil::hasKey(json, "type")) manifest.warnings.push_back("missing-type");
        if(!jsonutil::hasKey(json, "id")) {
            manifest.valid = false;
            manifest.errors.push_back("missing-id");
        }

        manifest.id = jsonutil::readString(json, "id", manifest.directoryName);
        manifest.name = jsonutil::readString(json, "name", manifest.id);
        manifest.version = jsonutil::readString(json, "version", "unknown");
        manifest.author = jsonutil::readString(json, "author", "");
        manifest.description = jsonutil::readString(json, "description", "");
        manifest.license = jsonutil::readString(json, "license", "");
        manifest.entry = jsonutil::readString(json, "entry", "");
        manifest.order = jsonutil::readInt(json, "order", 50);
        manifest.enabled = jsonutil::readBool(json, "enabled", false);

        manifest.type = packageTypeFromString(jsonutil::readString(json, "type", ""));
        if(manifest.type == PACKAGE_UNKNOWN && !manifest.entry.empty()) {
            manifest.type = inferPackageType(manifest.entry);
        }
        if(manifest.type == PACKAGE_UNKNOWN && defaultType != PACKAGE_UNKNOWN) {
            manifest.type = defaultType;
        }
        if(manifest.type == PACKAGE_UNKNOWN) {
            manifest.valid = false;
            manifest.errors.push_back("invalid-type");
            manifest.type = PACKAGE_EXTENSION;
        }
        manifest.typeName = packageTypeName(manifest.type);

        warnIfMissing(json, "name", manifest);
        warnIfMissing(json, "version", manifest);
        warnIfMissing(json, "enabled", manifest);
        if(manifest.entry.empty()) {
            if(manifest.type == PACKAGE_EXTENSION) {
                manifest.entry = manifest.id + ".so";
                manifest.warnings.push_back("missing-entry");
            } else {
                manifest.valid = false;
                manifest.errors.push_back("missing-entry");
                manifest.entry = manifest.id + ".qmd";
            }
        }

        if(!validId(manifest.id)) {
            manifest.valid = false;
            manifest.errors.push_back("invalid-id");
        }
        if(!validEntryForType(manifest.entry, manifest.type)) {
            manifest.valid = false;
            manifest.errors.push_back("invalid-entry");
        }
        if(manifest.type == PACKAGE_QMD) {
            if(!jsonutil::hasKey(json, "order")) manifest.warnings.push_back("missing-order");
            if(manifest.order < 0 || manifest.order > 999) {
                manifest.valid = false;
                manifest.errors.push_back("invalid-order");
            }
        }

        std::string requires = jsonutil::readObject(json, "requires");
        if(requires.empty()) {
            manifest.warnings.push_back("missing-requires");
        } else {
            manifest.requiresXovi = jsonutil::readString(requires, "xovi", "");
            manifest.requiresExtensions = jsonutil::readStringObject(requires, "extensions");
            manifest.requiresQmd = jsonutil::readStringObject(requires, "qmd");
            manifest.requiresXochitl = jsonutil::readStringArray(requires, "xochitl");
            manifest.requiresArchitectures = jsonutil::readStringArray(requires, "architectures");
        }
        if(manifest.requiresXovi.empty()) manifest.warnings.push_back("missing-requires.xovi");

        return manifest;
    }

    ExtensionManifest runtimeOnlyManifest(const std::string &id) {
        ExtensionManifest manifest;
        manifest.hasManifest = false;
        manifest.valid = true;
        manifest.managed = false;
        manifest.source = "runtime";
        manifest.type = PACKAGE_EXTENSION;
        manifest.typeName = packageTypeName(manifest.type);
        manifest.directoryName = id;
        manifest.directoryPath = joinPath(extensionPackageRoot(), id);
        manifest.id = id;
        manifest.name = id;
        manifest.entry = id + ".so";
        manifest.warnings.push_back("missing-manifest");
        manifest.warnings.push_back("unmanaged");
        return manifest;
    }

    bool runtimeStateDisabledByEnv() {
        const char *value = std::getenv("XOVI_EXTENSION_MANAGER_DISABLE_RUNTIME_API");
        if(value == nullptr) return false;
        return std::strcmp(value, "1") == 0 ||
            std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0 ||
            std::strcmp(value, "yes") == 0 ||
            std::strcmp(value, "YES") == 0;
    }

    void attachRuntimeState(Inventory &inventory) {
        if(runtimeStateDisabledByEnv()) return;
        if(Environment == nullptr || Environment->getScannedExtensionCount == nullptr || Environment->getScannedExtensionNames == nullptr) {
            return;
        }

        std::map<std::string, size_t> byId;
        for(size_t i = 0; i < inventory.extensions.size(); ++i) {
            if(inventory.extensions[i].type == PACKAGE_EXTENSION) byId[inventory.extensions[i].id] = i;
        }

        int count = Environment->getScannedExtensionCount();
        if(count <= 0) return;

        std::vector<const char *> names(static_cast<size_t>(count));
        int actual = Environment->getScannedExtensionNames(names.data(), count);
        for(int i = 0; i < actual; ++i) {
            if(names[static_cast<size_t>(i)] == nullptr) continue;
            std::string runtimeName = names[static_cast<size_t>(i)];

            size_t index;
            auto match = byId.find(runtimeName);
            if(match == byId.end()) {
                inventory.extensions.push_back(runtimeOnlyManifest(runtimeName));
                index = inventory.extensions.size() - 1;
                byId[runtimeName] = index;
            } else {
                index = match->second;
            }

            ExtensionManifest &manifest = inventory.extensions[index];
            manifest.runtime.seen = true;
            if(Environment->getExtensionLoadState != nullptr) {
                manifest.runtime.loadState = Environment->getExtensionLoadState(runtimeName.c_str());
                manifest.runtime.loadStateName = loadStateName(manifest.runtime.loadState);
            }
            if(Environment->getExtensionLoadError != nullptr) {
                const char *error = Environment->getExtensionLoadError(runtimeName.c_str());
                if(error != nullptr) manifest.runtime.loadError = error;
            }
            if(Environment->getExtensionVersion != nullptr) {
                unsigned char major = 0, minor = 0, patch = 0;
                if(Environment->getExtensionVersion(runtimeName.c_str(), &major, &minor, &patch) == 0) {
                    std::ostringstream version;
                    version << static_cast<int>(major) << "." << static_cast<int>(minor) << "." << static_cast<int>(patch);
                    manifest.runtime.version = version.str();
                }
            }
        }
    }

    std::map<std::string, const ExtensionManifest *> managedValidQmdById(const Inventory &inventory) {
        std::map<std::string, std::vector<const ExtensionManifest *>> matchesById;
        for(const ExtensionManifest &package : inventory.extensions) {
            matchesById[package.id].push_back(&package);
        }

        std::map<std::string, const ExtensionManifest *> byId;
        for(const auto &matches : matchesById) {
            if(matches.second.size() != 1) continue;
            const ExtensionManifest *package = matches.second.front();
            if(package->type == PACKAGE_QMD && package->managed && package->hasManifest && package->valid) {
                byId.insert(std::make_pair(matches.first, package));
            }
        }
        return byId;
    }

    bool qmdDependencyPathExists(
        const std::map<std::string, const ExtensionManifest *> &byId,
        const std::string &current,
        const std::string &wanted,
        std::set<std::string> &visited
    ) {
        if(current == wanted) return true;
        if(!visited.insert(current).second) return false;

        auto package = byId.find(current);
        if(package == byId.end()) return false;
        for(const auto &dependency : package->second->requiresQmd) {
            if(byId.find(dependency.first) == byId.end()) continue;
            if(qmdDependencyPathExists(byId, dependency.first, wanted, visited)) return true;
        }
        return false;
    }

    std::vector<std::string> qmdRequiredBy(const Inventory &inventory, const std::string &id) {
        std::vector<std::string> consumers;
        for(const ExtensionManifest &candidate : inventory.extensions) {
            if(candidate.type != PACKAGE_QMD || !candidate.managed || !candidate.hasManifest) continue;
            if(candidate.id == id || candidate.requiresQmd.find(id) == candidate.requiresQmd.end()) continue;
            if(candidate.enabled || effectiveEnabled(candidate)) consumers.push_back(candidate.id);
        }
        std::sort(consumers.begin(), consumers.end());
        consumers.erase(std::unique(consumers.begin(), consumers.end()), consumers.end());
        return consumers;
    }

    std::vector<std::string> qmdRequiredByIssues(const Inventory &inventory, const ExtensionManifest &manifest) {
        std::vector<std::string> issues;
        if(manifest.type != PACKAGE_QMD || !manifest.managed || !manifest.hasManifest) return issues;
        for(const std::string &consumer : qmdRequiredBy(inventory, manifest.id)) {
            issues.push_back("qmd-required-by:" + consumer);
        }
        return issues;
    }

    std::vector<std::string> computeIssues(const Inventory &inventory, const ExtensionManifest &manifest) {
        std::vector<std::string> issues = manifest.errors;
        if(!manifest.managed) issues.push_back("unmanaged");
        if(!manifest.hasManifest) issues.push_back("missing-manifest");
        if(!manifest.valid) issues.push_back("manifest-invalid");
        if(manifest.valid && !isPathPresent(sourceEntryPath(manifest))) issues.push_back("entry-missing");

        ActiveEntryState active = inspectActiveEntry(manifest);
        if(active.present && !active.matchesSource) issues.push_back(active.conflict.empty() ? "active-entry-conflict" : active.conflict);
        if(manifest.enabled && !active.matchesSource) issues.push_back("effective-disabled");
        if(!manifest.enabled && active.matchesSource) issues.push_back("effective-enabled-while-disabled");

        if(!manifest.requiresXovi.empty() && !versionSatisfies(XOVI_VERSION, manifest.requiresXovi)) {
            issues.push_back("xovi-version-incompatible");
        }

        std::map<std::string, const ExtensionManifest *> byId;
        for(const auto &extension : inventory.extensions) {
            if(extension.type == PACKAGE_EXTENSION) byId[extension.id] = &extension;
        }
        for(const auto &dependency : manifest.requiresExtensions) {
            auto found = byId.find(dependency.first);
            if(found == byId.end() || !found->second->valid) {
                issues.push_back("missing-dependency:" + dependency.first);
                continue;
            }
            if(!found->second->enabled) issues.push_back("disabled-dependency:" + dependency.first);
            if(!versionSatisfies(found->second->version, dependency.second)) {
                issues.push_back("dependency-version-incompatible:" + dependency.first);
            }
        }

        if(manifest.type == PACKAGE_QMD) {
            std::map<std::string, const ExtensionManifest *> qmdById = managedValidQmdById(inventory);
            for(const auto &dependency : manifest.requiresQmd) {
                auto found = qmdById.find(dependency.first);
                if(found == qmdById.end()) {
                    issues.push_back("missing-qmd-dependency:" + dependency.first);
                    continue;
                }
                const ExtensionManifest &required = *found->second;
                if(!required.enabled || !effectiveEnabled(required)) {
                    issues.push_back("disabled-qmd-dependency:" + dependency.first);
                }
                if(!versionSatisfies(required.version, dependency.second)) {
                    issues.push_back("qmd-dependency-version-incompatible:" + dependency.first);
                }
                if(required.order >= manifest.order) {
                    issues.push_back("qmd-dependency-order-invalid:" + dependency.first);
                }
                std::set<std::string> visited;
                if(qmdDependencyPathExists(qmdById, dependency.first, manifest.id, visited)) {
                    issues.push_back("qmd-dependency-cycle:" + dependency.first);
                }
            }
        }

        if(!manifest.requiresArchitectures.empty() && !inventory.architecture.empty()) {
            bool matched = false;
            for(const std::string &wanted : manifest.requiresArchitectures) {
                if(architectureMatches(wanted, inventory.architecture)) {
                    matched = true;
                    break;
                }
            }
            if(!matched) issues.push_back("architecture-mismatch");
        }

        if(!manifest.requiresXochitl.empty() && !inventory.xochitlVersion.empty()) {
            if(!xochitlVersionMatches(manifest.requiresXochitl, inventory.xochitlVersion)) {
                issues.push_back("xochitl-version-incompatible");
            }
        }

        if(manifest.type == PACKAGE_QMD && manifest.requiresExtensions.find("qt-resource-rebuilder") == manifest.requiresExtensions.end()) {
            issues.push_back("missing-runtime-dependency:qt-resource-rebuilder");
        }
        if(manifest.type == PACKAGE_QMD) {
            for(const auto &other : inventory.extensions) {
                if(other.type != PACKAGE_QMD) continue;
                if(other.id == manifest.id) continue;
                if(other.order == manifest.order) {
                    issues.push_back("qmd-order-conflict:" + other.id);
                }
            }
        }

        if(manifest.type == PACKAGE_EXTENSION) {
            switch(manifest.runtime.loadState) {
                case XOVI_EXTENSION_DLOPEN_FAILED:
                    issues.push_back("runtime-dlopen-failed");
                    break;
                case XOVI_EXTENSION_SHOULDLOAD_FAILED:
                    issues.push_back("runtime-shouldload-failed");
                    break;
                case XOVI_EXTENSION_CONDITION_FAILED:
                    issues.push_back("runtime-condition-failed");
                    break;
                case XOVI_EXTENSION_DEPENDENCY_FAILED:
                    issues.push_back("runtime-dependency-failed");
                    break;
                case XOVI_EXTENSION_LINK_FAILED:
                    issues.push_back("runtime-link-failed");
                    break;
                default:
                    break;
            }
        }

        std::sort(issues.begin(), issues.end());
        issues.erase(std::unique(issues.begin(), issues.end()), issues.end());
        return issues;
    }

    bool hasBlockingEnableIssue(const std::vector<std::string> &issues) {
        for(const std::string &issue : issues) {
            if(issue == "manifest-invalid" ||
                issue == "active-entry-conflict" ||
                issue == "active-entry-unreadable" ||
                issue == "entry-missing" ||
                issue == "xovi-version-incompatible" ||
                issue == "architecture-mismatch" ||
                issue == "xochitl-version-incompatible" ||
                issue == "missing-runtime-dependency:qt-resource-rebuilder" ||
                issue.rfind("missing-dependency:", 0) == 0 ||
                issue.rfind("disabled-dependency:", 0) == 0 ||
                issue.rfind("dependency-version-incompatible:", 0) == 0 ||
                issue.rfind("missing-qmd-dependency:", 0) == 0 ||
                issue.rfind("disabled-qmd-dependency:", 0) == 0 ||
                issue.rfind("qmd-dependency-version-incompatible:", 0) == 0 ||
                issue.rfind("qmd-dependency-order-invalid:", 0) == 0 ||
                issue.rfind("qmd-dependency-cycle:", 0) == 0) {
                return true;
            }
        }
        return false;
    }

    bool requiresRestart(const ExtensionManifest &manifest) {
        bool active = effectiveEnabled(manifest);
        if(PENDING_RESTART_PACKAGES.find(manifest.id) != PENDING_RESTART_PACKAGES.end()) return true;
        if(manifest.type == PACKAGE_QMD) return manifest.enabled != active;
        if(manifest.runtime.loadState == XOVI_EXTENSION_INITIALIZED && !active) return true;
        if(manifest.runtime.loadState != XOVI_EXTENSION_INITIALIZED && active) return true;
        return false;
    }

    std::string boolValue(bool value) {
        return value ? "true" : "false";
    }

    void addComma(std::ostringstream &out, bool &first) {
        if(!first) out << ",";
        first = false;
    }

    void addStringField(std::ostringstream &out, bool &first, const std::string &name, const std::string &value) {
        addComma(out, first);
        out << "\"" << name << "\":\"" << jsonutil::escape(value) << "\"";
    }

    void addBoolField(std::ostringstream &out, bool &first, const std::string &name, bool value) {
        addComma(out, first);
        out << "\"" << name << "\":" << boolValue(value);
    }

    void addIntField(std::ostringstream &out, bool &first, const std::string &name, int value) {
        addComma(out, first);
        out << "\"" << name << "\":" << value;
    }

    std::vector<std::string> availableActions(const ExtensionManifest &manifest) {
        std::vector<std::string> actions;
        actions.push_back("inspect");
        if(manifest.managed && manifest.hasManifest) {
            if(manifest.id == "xovi-extension-manager") {
                if(!manifest.enabled) actions.push_back("enable");
                return actions;
            }
            actions.push_back(manifest.enabled ? "disable" : "enable");
            actions.push_back("remove");
            return actions;
        }
        if(manifest.source == "legacy") {
            actions.push_back("adopt");
            if(effectiveEnabled(manifest)) actions.push_back("disableLegacy");
            actions.push_back("remove");
            return actions;
        }
        if(isPathPresent(sourceEntryPath(manifest))) actions.push_back("adopt");
        return actions;
    }

    bool replaceEnabledValue(std::string &json, bool enabled, std::string &error) {
        size_t key = json.find("\"enabled\"");
        if(key != std::string::npos) {
            size_t colon = json.find(':', key + 9);
            if(colon == std::string::npos) {
                error = "enabled key has no value";
                return false;
            }
            size_t start = colon + 1;
            while(start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
            size_t end = start;
            while(end < json.size() && json[end] != ',' && json[end] != '}') ++end;
            json.replace(start, end - start, enabled ? "true" : "false");
            return true;
        }

        size_t close = json.find_last_of('}');
        if(close == std::string::npos) {
            error = "manifest is not a JSON object";
            return false;
        }
        size_t previous = close;
        while(previous > 0 && std::isspace(static_cast<unsigned char>(json[previous - 1]))) --previous;
        bool needsComma = previous > 0 && json[previous - 1] != '{';
        json.insert(close, std::string(needsComma ? ",\n" : "\n") + "  \"enabled\": " + (enabled ? "true" : "false") + "\n");
        return true;
    }

    std::string fileName(const std::string &path) {
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string removeSuffix(const std::string &value, const std::string &suffix) {
        if(hasSuffix(value, suffix)) return value.substr(0, value.size() - suffix.size());
        return value;
    }

    std::string idFromFileName(const std::string &path) {
        std::string name = fileName(path);
        name = removeSuffix(name, ".tar.gz");
        name = removeSuffix(name, ".tgz");
        name = removeSuffix(name, ".zip");
        name = removeSuffix(name, ".so");
        name = removeSuffix(name, ".qmd");
        return name;
    }

    std::string sanitizeId(const std::string &value) {
        std::string id;
        bool previousDash = false;
        for(unsigned char c : trim(value)) {
            bool allowed = std::isalnum(c) || c == '-' || c == '_' || c == '.';
            char next = allowed ? static_cast<char>(c) : '-';
            if(next == '-' && previousDash) continue;
            id.push_back(next);
            previousDash = next == '-';
        }
        while(!id.empty() && id[0] == '-') id.erase(id.begin());
        while(!id.empty() && id[id.size() - 1] == '-') id.resize(id.size() - 1);
        if(id.empty() || id == "." || id == "..") return "legacy-package";
        return id;
    }

    bool qmdFileNameOrderAndId(const std::string &path, int &order, std::string &id) {
        std::string name = removeSuffix(fileName(path), ".qmd");
        if(name.size() > 4 &&
            std::isdigit(static_cast<unsigned char>(name[0])) &&
            std::isdigit(static_cast<unsigned char>(name[1])) &&
            std::isdigit(static_cast<unsigned char>(name[2])) &&
            name[3] == '-') {
            order = std::atoi(name.substr(0, 3).c_str());
            id = name.substr(4);
            return true;
        }
        order = 50;
        id = name;
        return false;
    }

    std::string activeEntrySourcePath(const std::string &activePath) {
        if(!isSymlink(activePath)) return activePath;
        std::string target;
        std::string error;
        if(!readSymlinkTarget(activePath, target, error)) return activePath;
        return absolutePathFrom(parentPath(activePath), target);
    }

    ExtensionManifest legacyManifestFromActivePath(PackageType type, const std::string &activePath) {
        std::string source = activeEntrySourcePath(activePath);
        std::string inferredId;
        int order = 50;
        if(type == PACKAGE_QMD) {
            qmdFileNameOrderAndId(activePath, order, inferredId);
        } else {
            inferredId = idFromFileName(activePath);
        }

        ExtensionManifest manifest;
        manifest.hasManifest = false;
        manifest.valid = true;
        manifest.managed = false;
        manifest.source = "legacy";
        manifest.type = type;
        manifest.typeName = packageTypeName(type);
        manifest.id = sanitizeId(inferredId);
        manifest.name = inferredId.empty() ? manifest.id : inferredId;
        manifest.entry = fileName(source);
        manifest.order = order;
        manifest.enabled = true;
        manifest.directoryName = manifest.id;
        manifest.directoryPath = parentPath(source);
        manifest.manifestPath = "";
        manifest.sourceEntryPathOverride = source;
        manifest.enabledEntryPathOverride = activePath;
        manifest.warnings.push_back("missing-manifest");
        manifest.warnings.push_back("unmanaged");
        if(manifest.id != inferredId) manifest.warnings.push_back("id-normalized-from-filename");
        return manifest;
    }

    ExtensionManifest legacyManifestFromDirectory(PackageType type, const std::string &directoryPath) {
        std::string id = sanitizeId(directoryName(directoryPath));
        ExtensionManifest manifest;
        manifest.hasManifest = false;
        manifest.valid = true;
        manifest.managed = false;
        manifest.source = "legacy";
        manifest.type = type;
        manifest.typeName = packageTypeName(type);
        manifest.directoryName = directoryName(directoryPath);
        manifest.directoryPath = directoryPath;
        manifest.id = id;
        manifest.name = manifest.directoryName;
        manifest.entry = id + (type == PACKAGE_QMD ? ".qmd" : ".so");
        manifest.enabled = false;
        manifest.warnings.push_back("missing-manifest");
        manifest.warnings.push_back("unmanaged");
        if(manifest.id != manifest.directoryName) manifest.warnings.push_back("id-normalized-from-filename");
        return manifest;
    }

    std::string targetDirForPackage(PackageType type, const std::string &id) {
        if(type == PACKAGE_QMD) return joinPath(qmdPackageRoot(), id);
        return joinPath(extensionPackageRoot(), id);
    }

    std::string sidecarManifestPath(const std::string &path) {
        std::string named = path + ".manifest.json";
        if(isPathPresent(named)) return named;
        std::string sibling = joinPath(parentPath(path), "manifest.json");
        if(isPathPresent(sibling)) return sibling;
        return "";
    }

    std::string minimalManifestJson(
        PackageType type,
        const std::string &id,
        const std::string &entry,
        bool enabled,
        int order = 50,
        const std::string &name = "",
        const std::string &version = "unknown"
    ) {
        std::ostringstream out;
        out << "{\n"
            << "  \"manifestVersion\": 1,\n"
            << "  \"type\": \"" << packageTypeName(type) << "\",\n"
            << "  \"id\": \"" << jsonutil::escape(id) << "\",\n"
            << "  \"name\": \"" << jsonutil::escape(name.empty() ? id : name) << "\",\n"
            << "  \"version\": \"" << jsonutil::escape(version.empty() ? "unknown" : version) << "\",\n";
        if(type == PACKAGE_QMD) {
            out << "  \"order\": " << order << ",\n"
                << "  \"requires\": {\n"
                << "    \"extensions\": {\n"
                << "      \"qt-resource-rebuilder\": \">=0.3.0\"\n"
                << "    }\n"
                << "  },\n";
        } else {
            out << "  \"requires\": {\n"
                << "    \"xovi\": \">=0.3.0\"\n"
                << "  },\n";
        }
        out << "  \"entry\": \"" << jsonutil::escape(entry) << "\",\n"
            << "  \"enabled\": " << boolValue(enabled) << "\n"
            << "}\n";
        return out.str();
    }

    struct InstallRequest {
        std::string path;
        bool enabled = false;
        bool enabledProvided = false;
    };

    InstallRequest parseInstallRequest(const std::string &request) {
        InstallRequest parsed;
        std::string trimmed = trim(request);
        if(jsonutil::isObject(trimmed)) {
            parsed.path = jsonutil::readString(trimmed, "path", "");
            parsed.enabled = jsonutil::readBool(trimmed, "enabled", false);
            parsed.enabledProvided = jsonutil::hasKey(trimmed, "enabled");
        } else {
            parsed.path = trimmed;
        }
        return parsed;
    }

    struct PackageActionRequest {
        std::string raw;
        std::string id;
        std::string path;
        std::string typeName;
        std::string name;
        std::string version = "unknown";
        std::string entry;
        std::string manifestPath;
        PackageType type = PACKAGE_UNKNOWN;
        int order = 50;
        bool enabled = true;
        bool enabledProvided = false;
        bool orderProvided = false;
    };

    PackageActionRequest parsePackageActionRequest(const std::string &request) {
        PackageActionRequest parsed;
        std::string trimmed = trim(request);
        parsed.raw = trimmed;
        if(jsonutil::isObject(trimmed)) {
            parsed.id = jsonutil::readString(trimmed, "id", "");
            parsed.path = jsonutil::readString(trimmed, "path", "");
            parsed.typeName = jsonutil::readString(trimmed, "type", "");
            parsed.name = jsonutil::readString(trimmed, "name", "");
            parsed.version = jsonutil::readString(trimmed, "version", "unknown");
            parsed.entry = jsonutil::readString(trimmed, "entry", "");
            parsed.manifestPath = jsonutil::readString(trimmed, "manifestPath", "");
            parsed.order = jsonutil::readInt(trimmed, "order", 50);
            parsed.orderProvided = jsonutil::hasKey(trimmed, "order");
            parsed.enabled = jsonutil::readBool(trimmed, "enabled", true);
            parsed.enabledProvided = jsonutil::hasKey(trimmed, "enabled");
            parsed.type = packageTypeFromString(parsed.typeName);
        } else if(trimmed.find('/') != std::string::npos || hasSuffix(trimmed, ".so") || hasSuffix(trimmed, ".qmd")) {
            parsed.path = trimmed;
        } else {
            parsed.id = trimmed;
        }
        if(parsed.type == PACKAGE_UNKNOWN && !parsed.path.empty()) parsed.type = inferPackageType(parsed.path);
        if(parsed.type == PACKAGE_UNKNOWN && !parsed.entry.empty()) parsed.type = inferPackageType(parsed.entry);
        return parsed;
    }

    int findPackageByRequest(const Inventory &inventory, const PackageActionRequest &request) {
        if(request.type != PACKAGE_UNKNOWN) {
            for(size_t i = 0; i < inventory.extensions.size(); ++i) {
                const ExtensionManifest &extension = inventory.extensions[i];
                if(extension.type != request.type) continue;
                if((!request.id.empty() && (extension.id == request.id || extension.directoryName == request.id)) ||
                    (!request.path.empty() && (enabledEntryPath(extension) == request.path || sourceEntryPath(extension) == request.path ||
                        extension.entry == request.path))) {
                    return static_cast<int>(i);
                }
            }
            for(size_t i = 0; i < inventory.extensions.size(); ++i) {
                const ExtensionManifest &extension = inventory.extensions[i];
                if(extension.type != request.type || request.path.empty()) continue;
                if(fileName(enabledEntryPath(extension)) == fileName(request.path) ||
                    fileName(sourceEntryPath(extension)) == fileName(request.path)) {
                    return static_cast<int>(i);
                }
            }
        }
        if(!request.id.empty()) {
            int index = findExtension(inventory, request.id);
            if(index >= 0) return index;
        }
        if(!request.path.empty()) {
            int index = findExtension(inventory, request.path);
            if(index >= 0) return index;
            index = findExtension(inventory, fileName(request.path));
            if(index >= 0) return index;
        }
        if(!request.raw.empty()) return findExtension(inventory, request.raw);
        return -1;
    }

    std::string uniquePathInDirectory(const std::string &directory, const std::string &name) {
        std::string candidate = joinPath(directory, name);
        if(!isPathPresent(candidate)) return candidate;

        std::string base = name;
        std::string suffix;
        size_t dot = name.find_last_of('.');
        if(dot != std::string::npos && dot != 0) {
            base = name.substr(0, dot);
            suffix = name.substr(dot);
        }
        for(int i = 1; i < 1000; ++i) {
            std::ostringstream numbered;
            numbered << base << "-" << i << suffix;
            candidate = joinPath(directory, numbered.str());
            if(!isPathPresent(candidate)) return candidate;
        }
        return joinPath(directory, base + "-overflow" + suffix);
    }

    std::string legacyDisabledRoot(PackageType type) {
        std::string leaf = type == PACKAGE_QMD ? "qmd" : "extensions";
        return joinPath(joinPath(joinPath(xoviRoot(), "exthome"), "xovi-extension-manager/legacy-disabled"), leaf);
    }

    bool isActivePathForType(PackageType type, const std::string &path) {
        std::string parent = parentPath(path);
        if(type == PACKAGE_QMD) return parent == qmdActiveDir();
        if(type == PACKAGE_EXTENSION) return parent == joinPath(xoviRoot(), "extensions.d");
        return false;
    }

    bool isLegacyDirectoryPath(PackageType type, const std::string &path) {
        if(path.empty() || !isDirectory(path) || isPathPresent(joinPath(path, "manifest.json"))) return false;
        if(type == PACKAGE_QMD) return parentPath(path) == qmdPackageRoot();
        if(type == PACKAGE_EXTENSION) return parentPath(path) == extensionPackageRoot();
        return false;
    }

    bool removeRecursive(const std::string &path, std::string &error) {
        struct stat st;
        if(lstat(path.c_str(), &st) != 0) {
            error = std::strerror(errno);
            return false;
        }

        if(S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            DIR *dir = opendir(path.c_str());
            if(dir == nullptr) {
                error = std::strerror(errno);
                return false;
            }
            bool ok = true;
            struct dirent *entry;
            while((entry = readdir(dir)) != nullptr) {
                if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
                if(!removeRecursive(joinPath(path, entry->d_name), error)) {
                    ok = false;
                    break;
                }
            }
            closedir(dir);
            if(!ok) return false;
            if(rmdir(path.c_str()) != 0) {
                error = std::strerror(errno);
                return false;
            }
            return true;
        }

        if(unlink(path.c_str()) != 0) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    }

    bool isSelfPackage(const ExtensionManifest &manifest) {
        return manifest.id == "xovi-extension-manager";
    }

    bool disableLegacyActiveEntry(
        const ExtensionManifest &manifest,
        std::vector<std::string> &actions,
        std::string &preservedPath,
        std::string &error
    ) {
        std::string active = enabledEntryPath(manifest);
        struct stat st;
        if(lstat(active.c_str(), &st) != 0) {
            actions.push_back("legacy-active-entry-already-absent");
            return true;
        }

        if(S_ISLNK(st.st_mode)) {
            if(unlink(active.c_str()) != 0) {
                error = std::strerror(errno);
                return false;
            }
            actions.push_back("legacy-active-symlink-removed");
            markRestartPending(manifest);
            return true;
        }

        if(S_ISDIR(st.st_mode)) {
            error = "legacy active entry is a directory";
            return false;
        }

        std::string disabledDir = legacyDisabledRoot(manifest.type);
        if(!mkdirRecursive(disabledDir)) {
            error = "cannot create legacy-disabled directory";
            return false;
        }
        preservedPath = uniquePathInDirectory(disabledDir, fileName(active));
        if(rename(active.c_str(), preservedPath.c_str()) != 0) {
            error = std::strerror(errno);
            return false;
        }
        actions.push_back("legacy-active-file-moved");
        markRestartPending(manifest);
        return true;
    }

    bool removeIssue(std::vector<std::string> &issues, const std::string &issue) {
        auto found = std::find(issues.begin(), issues.end(), issue);
        if(found == issues.end()) return false;
        issues.erase(found);
        return true;
    }

    struct InstallPlan {
        std::string mode;
        bool touchesActivePackage = false;
        std::string previousQmdActivePath;
        bool previousQmdActiveMatchesSource = false;
        bool previousQmdActiveSymlink = false;
    };

    bool isQmdDependencyBlockingIssue(const std::string &issue) {
        return issue.rfind("missing-qmd-dependency:", 0) == 0 ||
            issue.rfind("disabled-qmd-dependency:", 0) == 0 ||
            issue.rfind("qmd-dependency-version-incompatible:", 0) == 0 ||
            issue.rfind("qmd-dependency-order-invalid:", 0) == 0 ||
            issue.rfind("qmd-dependency-cycle:", 0) == 0;
    }

    std::string proposedQmdDependencyConflict(
        const Inventory &inventory,
        size_t existingIndex,
        const ExtensionManifest &proposed
    ) {
        Inventory planned = inventory;
        ExtensionManifest plannedPackage = proposed;
        const ExtensionManifest &existing = inventory.extensions[existingIndex];
        plannedPackage.directoryName = existing.directoryName;
        plannedPackage.directoryPath = existing.directoryPath;
        plannedPackage.manifestPath = existing.manifestPath;
        plannedPackage.managed = true;
        plannedPackage.hasManifest = true;
        plannedPackage.enabled = true;
        planned.extensions[existingIndex] = plannedPackage;

        std::vector<std::string> dependencyIssues;
        for(const std::string &issue : computeIssues(planned, planned.extensions[existingIndex])) {
            if(isQmdDependencyBlockingIssue(issue)) dependencyIssues.push_back(issue);
        }
        if(dependencyIssues.empty()) return "";
        return "{\"ok\":false,\"error\":\"qmd-dependencies-invalid\",\"message\":\"proposed QMD package has blocking QMD dependencies\",\"issues\":" +
            jsonutil::stringArray(dependencyIssues) + "}";
    }

    std::string qmdInstallConflict(
        const Inventory &inventory,
        const ExtensionManifest &existing,
        const ExtensionManifest &proposed,
        bool desiredEnabled
    ) {
        std::vector<std::string> consumers = qmdRequiredBy(inventory, existing.id);
        if(consumers.empty()) return "";

        std::vector<std::string> dependencyIssues;
        if(!desiredEnabled) {
            dependencyIssues.push_back("disabled-qmd-dependency:" + existing.id);
        }
        for(const ExtensionManifest &candidate : inventory.extensions) {
            if(candidate.type != PACKAGE_QMD || !candidate.managed || !candidate.hasManifest) continue;
            if(!(candidate.enabled || effectiveEnabled(candidate))) continue;
            auto requirement = candidate.requiresQmd.find(existing.id);
            if(requirement == candidate.requiresQmd.end()) continue;
            if(!versionSatisfies(proposed.version, requirement->second)) {
                dependencyIssues.push_back("qmd-dependency-version-incompatible:" + existing.id);
            }
            if(proposed.order >= candidate.order) {
                dependencyIssues.push_back("qmd-dependency-order-invalid:" + existing.id);
            }
        }
        std::sort(dependencyIssues.begin(), dependencyIssues.end());
        dependencyIssues.erase(std::unique(dependencyIssues.begin(), dependencyIssues.end()), dependencyIssues.end());
        if(dependencyIssues.empty()) return "";

        std::vector<std::string> requiredByIssues;
        for(const std::string &consumer : consumers) requiredByIssues.push_back("qmd-required-by:" + consumer);
        return "{\"ok\":false,\"error\":\"qmd-required-by\",\"message\":\"proposed QMD package would invalidate enabled consumers\",\"issues\":" +
            jsonutil::stringArray(requiredByIssues) + ",\"dependencyIssues\":" + jsonutil::stringArray(dependencyIssues) + "}";
    }

    bool classifyInstallMode(
        const ExtensionManifest &manifest,
        bool desiredEnabled,
        InstallPlan &plan,
        std::vector<std::string> &actions,
        std::string &error
    ) {
        Inventory current = loadInventory();
        int existingIndex = findExtension(current, manifest.id);
        if(existingIndex < 0) {
            plan.mode = "new";
            actions.push_back("new-package");
            return true;
        }

        const ExtensionManifest &existing = current.extensions[static_cast<size_t>(existingIndex)];
        if(!existing.managed) {
            std::ostringstream out;
            out << "{\"ok\":false,"
                << "\"error\":\"installed-legacy-conflict\","
                << "\"message\":\"a legacy package with this id is already present; adopt or disable it before installing\","
                << "\"id\":\"" << jsonutil::escape(existing.id) << "\","
                << "\"type\":\"" << jsonutil::escape(existing.typeName) << "\","
                << "\"sourceEntryPath\":\"" << jsonutil::escape(sourceEntryPath(existing)) << "\","
                << "\"enabledEntryPath\":\"" << jsonutil::escape(enabledEntryPath(existing)) << "\"}";
            error = out.str();
            return false;
        }
        if(existing.type != manifest.type) {
            std::ostringstream out;
            out << "{\"ok\":false,"
                << "\"error\":\"installed-type-conflict\","
                << "\"message\":\"a package with this id is already installed with a different type\","
                << "\"id\":\"" << jsonutil::escape(manifest.id) << "\","
                << "\"installedType\":\"" << jsonutil::escape(existing.typeName) << "\","
                << "\"proposedType\":\"" << jsonutil::escape(manifest.typeName) << "\","
                << "\"installedVersion\":\"" << jsonutil::escape(existing.version) << "\","
                << "\"proposedVersion\":\"" << jsonutil::escape(manifest.version) << "\"}";
            error = out.str();
            return false;
        }

        if(existing.type == PACKAGE_QMD) {
            if(desiredEnabled && effectiveEnabled(existing)) {
                error = proposedQmdDependencyConflict(current, static_cast<size_t>(existingIndex), manifest);
                if(!error.empty()) return false;
            }
            error = qmdInstallConflict(current, existing, manifest, desiredEnabled);
            if(!error.empty()) return false;
            ActiveEntryState previousActive = inspectActiveEntry(existing);
            plan.previousQmdActivePath = enabledEntryPath(existing);
            plan.previousQmdActiveMatchesSource = previousActive.matchesSource;
            plan.previousQmdActiveSymlink = previousActive.symlink;
        }

        plan.touchesActivePackage = effectiveEnabled(existing) || existing.runtime.loadState == XOVI_EXTENSION_INITIALIZED;
        if(existing.version == manifest.version) {
            plan.mode = "reinstall";
            actions.push_back("reinstalling-existing-package");
            return true;
        }

        int comparison = compareSemver(existing.version, manifest.version);
        if(comparison < 0) {
            plan.mode = "upgrade";
            actions.push_back("upgrading-existing-package");
        } else if(comparison > 0) {
            plan.mode = "downgrade";
            actions.push_back("downgrading-existing-package");
        } else {
            plan.mode = "replace";
            actions.push_back("replacing-existing-package");
        }
        return true;
    }

    std::vector<std::string> installWarnings(const Inventory &inventory, const ExtensionManifest &manifest, bool enableSucceeded) {
        std::vector<std::string> warnings = computeIssues(inventory, manifest);
        if(!enableSucceeded) warnings.push_back("enable-after-install-blocked");
        std::sort(warnings.begin(), warnings.end());
        warnings.erase(std::unique(warnings.begin(), warnings.end()), warnings.end());
        return warnings;
    }

    std::string installTargetAndSetEnabled(
        const ExtensionManifest &manifest,
        bool desiredEnabled,
        const InstallPlan &plan,
        std::vector<std::string> &actions
    ) {
        if(manifest.type == PACKAGE_QMD) {
            std::string currentActivePath = enabledEntryPath(manifest);
            if(plan.previousQmdActiveMatchesSource && plan.previousQmdActiveSymlink &&
                !plan.previousQmdActivePath.empty() && isSymlink(plan.previousQmdActivePath)) {
                if(unlink(plan.previousQmdActivePath.c_str()) == 0) {
                    actions.push_back(plan.previousQmdActivePath == currentActivePath
                        ? "qmd-active-symlink-removed-before-enable"
                        : "previous-qmd-active-symlink-removed");
                    markRestartPending(manifest);
                } else {
                    return errorJson("active-entry-remove-failed", std::strerror(errno));
                }
            }

            ActiveEntryState currentActive = inspectActiveEntry(manifest);
            if(currentActive.matchesSource && currentActive.symlink) {
                if(unlink(currentActivePath.c_str()) == 0) {
                    actions.push_back("qmd-active-symlink-removed-before-enable");
                    markRestartPending(manifest);
                } else {
                    return errorJson("active-entry-remove-failed", std::strerror(errno));
                }
            }
        }

        std::string enabledResult = setExtensionEnabled(manifest.id, desiredEnabled);
        bool stateChanged = jsonutil::readBool(enabledResult, "ok", false);
        if(desiredEnabled && stateChanged) {
            actions.push_back("enabled-after-install");
        } else if(desiredEnabled) {
            actions.push_back("enable-after-install-blocked");
        } else {
            actions.push_back("left-disabled-after-install");
        }
        if(plan.touchesActivePackage) markRestartPending(manifest);

        Inventory after = loadInventory();
        int afterIndex = findExtension(after, manifest.id);
        bool restart = afterIndex >= 0 && requiresRestart(after.extensions[static_cast<size_t>(afterIndex)]);
        std::vector<std::string> warnings;
        if(afterIndex >= 0) {
            warnings = installWarnings(after, after.extensions[static_cast<size_t>(afterIndex)], !desiredEnabled || stateChanged);
        } else if(!stateChanged && desiredEnabled) {
            warnings.push_back("enable-after-install-blocked");
        }
        std::ostringstream out;
        out << "{\"ok\":true,"
            << "\"type\":\"" << jsonutil::escape(manifest.typeName) << "\","
            << "\"id\":\"" << jsonutil::escape(manifest.id) << "\","
            << "\"installMode\":\"" << jsonutil::escape(plan.mode) << "\","
            << "\"desiredEnabled\":" << boolValue(desiredEnabled) << ","
            << "\"actions\":" << jsonutil::stringArray(actions) << ","
            << "\"warnings\":" << jsonutil::stringArray(warnings) << ","
            << "\"requiresRestart\":" << boolValue(restart) << ","
            << "\"restartTarget\":\"xochitl\","
            << "\"enableResult\":" << enabledResult;
        if(afterIndex >= 0) {
            out << ",\"package\":" << extensionToJson(after, after.extensions[static_cast<size_t>(afterIndex)]);
        }
        out << "}";
        return out.str();
    }

    std::string installSingleFile(const InstallRequest &request) {
        if(!isPathPresent(request.path)) return errorJson("not-found", "install path was not found");

        PackageType type = inferPackageType(request.path);
        if(type == PACKAGE_UNKNOWN) return errorJson("unsupported-file", "single-file install supports .so and .qmd");

        std::string sourceName = fileName(request.path);
        std::string manifestPath = sidecarManifestPath(request.path);
        ExtensionManifest manifest;
        std::string manifestJson;
        bool desiredEnabled = false;
        if(!manifestPath.empty()) {
            manifest = parseManifest(parentPath(request.path), manifestPath, type);
            manifestJson = readFile(manifestPath);
            desiredEnabled = request.enabledProvided ? request.enabled : manifest.enabled;
            if(manifest.entry.empty()) manifest.entry = sourceName;
        } else {
            manifest.type = type;
            manifest.typeName = packageTypeName(type);
            manifest.id = idFromFileName(request.path);
            manifest.name = manifest.id;
            manifest.entry = sourceName;
            desiredEnabled = request.enabledProvided ? request.enabled : false;
            manifestJson = minimalManifestJson(type, manifest.id, manifest.entry, false);
        }

        if(!manifest.valid) return errorJson("invalid-manifest", jsonutil::stringArray(manifest.errors));
        if(!validId(manifest.id)) return errorJson("invalid-id", "package id is invalid");
        if(!validEntryForType(manifest.entry, manifest.type)) return errorJson("invalid-entry", "entry is invalid for package type");
        manifest.enabled = false;

        InstallPlan plan;
        std::vector<std::string> actions;
        if(isSelfPackage(manifest) && !desiredEnabled) {
            desiredEnabled = true;
            actions.push_back("self-package-forced-enabled");
        }
        std::string conflictError;
        if(!classifyInstallMode(manifest, desiredEnabled, plan, actions, conflictError)) return conflictError;

        std::string error;
        if(!replaceEnabledValue(manifestJson, false, error)) return errorJson("invalid-manifest", error);

        std::string targetDir = targetDirForPackage(manifest.type, manifest.id);
        char tempTemplate[] = "/tmp/xovi-extmgr-single-XXXXXX";
        char *tempDirRaw = mkdtemp(tempTemplate);
        if(tempDirRaw == nullptr) return errorJson("tempdir-failed", std::strerror(errno));
        std::string tempDir = tempDirRaw;

        std::string tempEntry = joinPath(tempDir, manifest.entry);
        if(!mkdirRecursive(parentPath(tempEntry))) {
            cleanupTempDir(tempDir);
            return errorJson("mkdir-failed", "temporary package directory could not be created");
        }

        if(!copyFile(request.path, tempEntry, error)) {
            cleanupTempDir(tempDir);
            return errorJson("copy-failed", error);
        }
        actions.push_back("entry-copied");
        if(!writeFile(joinPath(tempDir, "manifest.json"), manifestJson, error)) {
            cleanupTempDir(tempDir);
            return errorJson("write-failed", error);
        }
        actions.push_back(manifestPath.empty() ? "minimal-manifest-created" : "sidecar-manifest-copied");

        if(!replacePackageDirectory(tempDir, targetDir, isSelfPackage(manifest), actions, error)) {
            cleanupTempDir(tempDir);
            return errorJson("copy-failed", error);
        }
        cleanupTempDir(tempDir);

        manifest.directoryPath = targetDir;
        manifest.manifestPath = joinPath(targetDir, "manifest.json");
        return installTargetAndSetEnabled(manifest, desiredEnabled, plan, actions);
    }

    std::string installArchive(const InstallRequest &request) {
        char tempTemplate[] = "/tmp/xovi-extmgr-install-XXXXXX";
        char *tempDirRaw = mkdtemp(tempTemplate);
        if(tempDirRaw == nullptr) return errorJson("tempdir-failed", std::strerror(errno));
        std::string tempDir = tempDirRaw;

        std::string error;
        if(!extractArchive(request.path, tempDir, error)) {
            cleanupTempDir(tempDir);
            return errorJson("extract-failed", error);
        }

        std::string manifestPath = findManifestRecursive(tempDir);
        if(manifestPath.empty()) {
            cleanupTempDir(tempDir);
            return errorJson("missing-manifest", "archive does not contain manifest.json");
        }

        std::string packageDir = parentPath(manifestPath);
        ExtensionManifest manifest = parseManifest(packageDir, manifestPath, PACKAGE_UNKNOWN);
        bool desiredEnabled = request.enabledProvided ? request.enabled : manifest.enabled;
        if(!manifest.valid) {
            std::string result = errorJson("invalid-manifest", jsonutil::stringArray(manifest.errors));
            cleanupTempDir(tempDir);
            return result;
        }
        if(!isPathPresent(sourceEntryPath(manifest))) {
            cleanupTempDir(tempDir);
            return errorJson("entry-missing", "archive manifest entry file was not found");
        }

        std::string targetDir = targetDirForPackage(manifest.type, manifest.id);
        InstallPlan plan;
        std::vector<std::string> actions;
        if(isSelfPackage(manifest) && !desiredEnabled) {
            desiredEnabled = true;
            actions.push_back("self-package-forced-enabled");
        }
        std::string conflictError;
        if(!classifyInstallMode(manifest, desiredEnabled, plan, actions, conflictError)) {
            cleanupTempDir(tempDir);
            return conflictError;
        }

        std::string targetManifest = joinPath(packageDir, "manifest.json");
        std::string json = readFile(targetManifest);
        if(!replaceEnabledValue(json, false, error)) {
            cleanupTempDir(tempDir);
            return errorJson("invalid-manifest", error);
        }
        if(!writeFile(targetManifest, json, error)) {
            cleanupTempDir(tempDir);
            return errorJson("write-failed", error);
        }

        if(!replacePackageDirectory(packageDir, targetDir, isSelfPackage(manifest), actions, error)) {
            cleanupTempDir(tempDir);
            return errorJson("copy-failed", error);
        }
        actions.push_back("archive-extracted");
        actions.push_back("package-files-copied");

        cleanupTempDir(tempDir);
        manifest.directoryPath = targetDir;
        manifest.manifestPath = joinPath(targetDir, "manifest.json");
        manifest.enabled = false;
        return installTargetAndSetEnabled(manifest, desiredEnabled, plan, actions);
    }
}

std::string xoviRoot() {
    const char *root = std::getenv("XOVI_ROOT");
    std::string value = root == nullptr || *root == 0 ? XOVI_ROOT_DEFAULT : root;
    while(value.size() > 1 && value[value.size() - 1] == '/') value.resize(value.size() - 1);
    return value;
}

bool isPathPresent(const std::string &path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

std::string sourceEntryPath(const ExtensionManifest &manifest) {
    if(!manifest.sourceEntryPathOverride.empty()) return manifest.sourceEntryPathOverride;
    return joinPath(manifest.directoryPath, manifest.entry);
}

std::string enabledEntryPath(const ExtensionManifest &manifest) {
    if(!manifest.enabledEntryPathOverride.empty()) return manifest.enabledEntryPathOverride;
    if(manifest.type == PACKAGE_QMD) {
        return joinPath(qmdActiveDir(), qmdActiveFileName(manifest));
    }
    return joinPath(joinPath(xoviRoot(), "extensions.d"), manifest.id + ".so");
}

Inventory loadInventory(bool includeRuntime) {
    Inventory inventory;
    inventory.root = xoviRoot();
    inventory.architecture = currentArchitecture();
    inventory.xochitlVersion = currentXochitlVersion();

    std::string extensionRoot = extensionPackageRoot();
    DIR *root = opendir(extensionRoot.c_str());
    if(root != nullptr) {
        struct dirent *entry;
        while((entry = readdir(root)) != nullptr) {
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            if(entry->d_name[0] == '.') continue;
            std::string extensionDir = joinPath(extensionRoot, entry->d_name);
            if(!isDirectory(extensionDir)) continue;

            std::string manifestPath = joinPath(extensionDir, "manifest.json");
            if(isPathPresent(manifestPath)) {
                inventory.extensions.push_back(parseManifest(extensionDir, manifestPath, PACKAGE_EXTENSION));
            } else {
                inventory.extensions.push_back(legacyManifestFromDirectory(PACKAGE_EXTENSION, extensionDir));
            }
        }
        closedir(root);
    }

    std::string qmdRoot = qmdPackageRoot();
    DIR *qmdDir = opendir(qmdRoot.c_str());
    if(qmdDir != nullptr) {
        struct dirent *entry;
        while((entry = readdir(qmdDir)) != nullptr) {
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            if(entry->d_name[0] == '.') continue;
            std::string packageDir = joinPath(qmdRoot, entry->d_name);
            if(!isDirectory(packageDir)) continue;

            std::string manifestPath = joinPath(packageDir, "manifest.json");
            if(isPathPresent(manifestPath)) {
                inventory.extensions.push_back(parseManifest(packageDir, manifestPath, PACKAGE_QMD));
            } else {
                inventory.extensions.push_back(legacyManifestFromDirectory(PACKAGE_QMD, packageDir));
            }
        }
        closedir(qmdDir);
    }

    auto mergeLegacyActive = [&](const ExtensionManifest &legacy) {
        for(ExtensionManifest &existing : inventory.extensions) {
            if(existing.type != legacy.type || existing.id != legacy.id) continue;
            if(existing.managed) return;
            existing.entry = legacy.entry;
            existing.order = legacy.order;
            existing.enabled = true;
            existing.sourceEntryPathOverride = legacy.sourceEntryPathOverride;
            existing.enabledEntryPathOverride = legacy.enabledEntryPathOverride;
            if(existing.directoryPath.empty() || !isLegacyDirectoryPath(existing.type, existing.directoryPath)) {
                existing.directoryPath = legacy.directoryPath;
                existing.directoryName = legacy.directoryName;
            }
            existing.name = legacy.name;
            return;
        }
        inventory.extensions.push_back(legacy);
    };

    std::string activeExtensionsDir = joinPath(inventory.root, "extensions.d");
    DIR *activeExtensions = opendir(activeExtensionsDir.c_str());
    if(activeExtensions != nullptr) {
        struct dirent *entry;
        while((entry = readdir(activeExtensions)) != nullptr) {
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            std::string activePath = joinPath(activeExtensionsDir, entry->d_name);
            if(!hasSuffix(entry->d_name, ".so")) continue;
            mergeLegacyActive(legacyManifestFromActivePath(PACKAGE_EXTENSION, activePath));
        }
        closedir(activeExtensions);
    }

    std::string activeQmdDir = qmdActiveDir();
    DIR *activeQmd = opendir(activeQmdDir.c_str());
    if(activeQmd != nullptr) {
        struct dirent *entry;
        while((entry = readdir(activeQmd)) != nullptr) {
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            std::string activePath = joinPath(activeQmdDir, entry->d_name);
            if(!hasSuffix(entry->d_name, ".qmd")) continue;
            mergeLegacyActive(legacyManifestFromActivePath(PACKAGE_QMD, activePath));
        }
        closedir(activeQmd);
    }

    if(includeRuntime) attachRuntimeState(inventory);

    std::sort(inventory.extensions.begin(), inventory.extensions.end(), [](const ExtensionManifest &a, const ExtensionManifest &b) {
        return a.id < b.id;
    });
    return inventory;
}

std::string extensionToJson(const Inventory &inventory, const ExtensionManifest &extension) {
    std::ostringstream out;
    bool first = true;
    ActiveEntryState active = inspectActiveEntry(extension);
    out << "{";
    addStringField(out, first, "type", extension.typeName);
    addStringField(out, first, "source", extension.source);
    addBoolField(out, first, "managed", extension.managed);
    addStringField(out, first, "id", extension.id);
    addStringField(out, first, "name", extension.name);
    addStringField(out, first, "version", extension.version);
    addStringField(out, first, "author", extension.author);
    addStringField(out, first, "description", extension.description);
    addStringField(out, first, "license", extension.license);
    addStringField(out, first, "entry", extension.entry);
    addIntField(out, first, "order", extension.order);
    addStringField(out, first, "sourceEntryPath", sourceEntryPath(extension));
    addStringField(out, first, "enabledEntryPath", enabledEntryPath(extension));
    addBoolField(out, first, "sourceEntryExists", isPathPresent(sourceEntryPath(extension)));
    addBoolField(out, first, "activeEntryExists", active.present);
    addBoolField(out, first, "activeEntryIsSymlink", active.symlink);
    addBoolField(out, first, "activeEntryMatches", active.matchesSource);
    addStringField(out, first, "activeEntryTarget", active.target);
    addStringField(out, first, "activeEntryConflict", active.conflict);
    addBoolField(out, first, "effectiveEnabled", active.matchesSource);
    addBoolField(out, first, "enabled", extension.enabled);
    addBoolField(out, first, "hasManifest", extension.hasManifest);
    addBoolField(out, first, "valid", extension.valid);
    addStringField(out, first, "directoryPath", extension.directoryPath);
    addStringField(out, first, "packagePath", extension.directoryPath);
    addStringField(out, first, "dataPath", persistentDataDir(extension));
    addStringField(out, first, "manifestPath", extension.manifestPath);
    addComma(out, first);
    out << "\"requires\":{"
        << "\"xovi\":\"" << jsonutil::escape(extension.requiresXovi) << "\","
        << "\"extensions\":" << jsonutil::stringObject(extension.requiresExtensions) << ","
        << "\"qmd\":" << jsonutil::stringObject(extension.requiresQmd) << ","
        << "\"xochitl\":" << jsonutil::stringArray(extension.requiresXochitl) << ","
        << "\"architectures\":" << jsonutil::stringArray(extension.requiresArchitectures)
        << "}";
    addComma(out, first);
    out << "\"runtime\":{"
        << "\"seen\":" << boolValue(extension.runtime.seen) << ","
        << "\"loadState\":" << extension.runtime.loadState << ","
        << "\"loadStateName\":\"" << jsonutil::escape(extension.runtime.loadStateName) << "\","
        << "\"loadError\":\"" << jsonutil::escape(extension.runtime.loadError) << "\","
        << "\"version\":\"" << jsonutil::escape(extension.runtime.version) << "\""
        << "}";
    addBoolField(out, first, "requiresRestart", requiresRestart(extension));
    addComma(out, first);
    out << "\"warnings\":" << jsonutil::stringArray(extension.warnings);
    addComma(out, first);
    out << "\"errors\":" << jsonutil::stringArray(extension.errors);
    addComma(out, first);
    out << "\"issues\":" << jsonutil::stringArray(computeIssues(inventory, extension));
    addComma(out, first);
    out << "\"availableActions\":" << jsonutil::stringArray(availableActions(extension));
    out << "}";
    return out.str();
}

std::string inventoryToJson(const Inventory &inventory) {
    std::ostringstream out;
    out << "{\"ok\":true,"
        << "\"root\":\"" << jsonutil::escape(inventory.root) << "\","
        << "\"architecture\":\"" << jsonutil::escape(inventory.architecture) << "\","
        << "\"xoviVersion\":\"" << jsonutil::escape(XOVI_VERSION) << "\","
        << "\"xochitlVersion\":\"" << jsonutil::escape(inventory.xochitlVersion) << "\","
        << "\"count\":" << inventory.extensions.size() << ","
        << "\"packages\":[";
    for(size_t i = 0; i < inventory.extensions.size(); ++i) {
        if(i != 0) out << ",";
        out << extensionToJson(inventory, inventory.extensions[i]);
    }
    out << "],\"extensions\":[";
    bool firstExtension = true;
    for(size_t i = 0; i < inventory.extensions.size(); ++i) {
        if(inventory.extensions[i].type != PACKAGE_EXTENSION) continue;
        if(!firstExtension) out << ",";
        firstExtension = false;
        out << extensionToJson(inventory, inventory.extensions[i]);
    }
    out << "],\"qmd\":[";
    bool firstQmd = true;
    for(size_t i = 0; i < inventory.extensions.size(); ++i) {
        if(inventory.extensions[i].type != PACKAGE_QMD) continue;
        if(!firstQmd) out << ",";
        firstQmd = false;
        out << extensionToJson(inventory, inventory.extensions[i]);
    }
    out << "]}";
    return out.str();
}

int findExtension(const Inventory &inventory, const std::string &id) {
    std::string wanted = trim(id);
    for(size_t i = 0; i < inventory.extensions.size(); ++i) {
        const ExtensionManifest &extension = inventory.extensions[i];
        if(extension.id == wanted ||
            extension.directoryName == wanted ||
            extension.entry == wanted ||
            enabledEntryPath(extension) == wanted ||
            sourceEntryPath(extension) == wanted) {
            return static_cast<int>(i);
        }
    }
    for(size_t i = 0; i < inventory.extensions.size(); ++i) {
        const ExtensionManifest &extension = inventory.extensions[i];
        if(fileName(enabledEntryPath(extension)) == wanted ||
            fileName(sourceEntryPath(extension)) == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string errorJson(const std::string &code, const std::string &message) {
    return "{\"ok\":false,\"error\":\"" + jsonutil::escape(code) + "\",\"message\":\"" + jsonutil::escape(message) + "\"}";
}

std::string setExtensionEnabled(const std::string &id, bool enabled) {
    Inventory inventory = loadInventory();
    int index = findExtension(inventory, id);
    if(index < 0) return errorJson("not-found", "package was not found");

    ExtensionManifest &extension = inventory.extensions[static_cast<size_t>(index)];
    if(!extension.hasManifest || extension.manifestPath.empty()) {
        return errorJson("missing-manifest", "package has no manifest.json");
    }
    if(!enabled && isSelfPackage(extension)) {
        return errorJson("self-disable-blocked", "xovi-extension-manager cannot disable itself");
    }
    if(!enabled && extension.type == PACKAGE_QMD) {
        std::vector<std::string> issues = qmdRequiredByIssues(inventory, extension);
        if(!issues.empty()) {
            return "{\"ok\":false,\"error\":\"qmd-required-by\",\"message\":\"QMD package is required by enabled consumers\",\"issues\":" +
                jsonutil::stringArray(issues) + "}";
        }
    }
    if(!extension.valid) {
        return errorJson("invalid-manifest", "package manifest must be fixed before changing enabled state");
    }
    if(enabled) {
        std::vector<std::string> issues = computeIssues(inventory, extension);
        if(hasBlockingEnableIssue(issues)) {
            return "{\"ok\":false,\"error\":\"enable-blocked\",\"message\":\"package has blocking issues\",\"issues\":" + jsonutil::stringArray(issues) + "}";
        }
    }

    std::string json = readFile(extension.manifestPath);
    std::string error;
    if(!replaceEnabledValue(json, enabled, error)) return errorJson("invalid-manifest", error);
    if(!writeFile(extension.manifestPath, json, error)) return errorJson("write-failed", error);

    extension.enabled = enabled;
    std::vector<std::string> actions;
    actions.push_back(enabled ? "manifest-enabled" : "manifest-disabled");

    std::string source = sourceEntryPath(extension);
    std::string active = enabledEntryPath(extension);
    if(enabled) {
        std::string activeDir = extension.type == PACKAGE_QMD ? qmdActiveDir() : joinPath(xoviRoot(), "extensions.d");
        mkdirRecursive(activeDir);
        ActiveEntryState activeState = inspectActiveEntry(extension);
        if(activeState.present && activeState.matchesSource) {
            actions.push_back("active-entry-already-present");
        } else if(activeState.present) {
            actions.push_back("active-entry-conflict");
        } else if(!isPathPresent(source)) {
            actions.push_back("source-entry-missing");
        } else if(symlink(source.c_str(), active.c_str()) == 0) {
            actions.push_back("active-entry-symlink-created");
            markRestartPending(extension);
        } else {
            actions.push_back(std::string("active-entry-symlink-failed:") + std::strerror(errno));
        }
    } else if(isPathPresent(active)) {
        ActiveEntryState activeState = inspectActiveEntry(extension);
        if(activeState.present && !activeState.matchesSource) {
            actions.push_back("active-entry-conflict-left-in-place");
        } else if(isSymlink(active)) {
            if(unlink(active.c_str()) == 0) {
                actions.push_back("active-entry-symlink-removed");
                markRestartPending(extension);
            } else {
                actions.push_back(std::string("active-entry-remove-failed:") + std::strerror(errno));
            }
        } else {
            actions.push_back("active-entry-not-symlink-left-in-place");
        }
    } else {
        actions.push_back("active-entry-already-absent");
    }

    Inventory after = loadInventory();
    int afterIndex = findExtension(after, extension.id);
    std::ostringstream out;
    out << "{\"ok\":true,"
        << "\"type\":\"" << jsonutil::escape(extension.typeName) << "\","
        << "\"id\":\"" << jsonutil::escape(extension.id) << "\","
        << "\"enabled\":" << boolValue(enabled) << ","
        << "\"actions\":" << jsonutil::stringArray(actions) << ","
        << "\"requiresRestart\":" << boolValue(afterIndex >= 0 && requiresRestart(after.extensions[static_cast<size_t>(afterIndex)])) << ","
        << "\"restartTarget\":\"xochitl\"";
    if(afterIndex >= 0) {
        out << ",\"extension\":" << extensionToJson(after, after.extensions[static_cast<size_t>(afterIndex)]);
    }
    out << "}";
    return out.str();
}

std::string installPackage(const std::string &request) {
    InstallRequest parsed = parseInstallRequest(request);
    if(parsed.path.empty()) return errorJson("missing-path", "install request requires a path");
    if(!isPathPresent(parsed.path)) return errorJson("not-found", "install path was not found");

    if(hasSuffix(parsed.path, ".so") || hasSuffix(parsed.path, ".qmd")) {
        return installSingleFile(parsed);
    }
    if(hasSuffix(parsed.path, ".tar.gz") || hasSuffix(parsed.path, ".tgz") || hasSuffix(parsed.path, ".zip")) {
        return installArchive(parsed);
    }
    return errorJson("unsupported-package", "install supports .so, .qmd, .tar.gz, .tgz, and .zip");
}

std::string adoptLegacyPackage(const std::string &request) {
    PackageActionRequest parsed = parsePackageActionRequest(request);
    if(parsed.id.empty() && parsed.path.empty()) return errorJson("missing-target", "adopt requires an id or path");

    Inventory inventory = loadInventory();
    ExtensionManifest legacy;
    int legacyIndex = findPackageByRequest(inventory, parsed);
    if(legacyIndex >= 0) {
        legacy = inventory.extensions[static_cast<size_t>(legacyIndex)];
        if(legacy.managed) return errorJson("already-managed", "package is already managed");
    } else {
        if(parsed.path.empty()) return errorJson("not-found", "legacy package was not found");
        if(!isPathPresent(parsed.path)) return errorJson("not-found", "legacy path was not found");
        PackageType inferred = parsed.type == PACKAGE_UNKNOWN ? inferPackageType(parsed.path) : parsed.type;
        if(inferred == PACKAGE_UNKNOWN) return errorJson("unsupported-file", "adopt supports .so and .qmd files");
        if(!isActivePathForType(inferred, parsed.path)) return errorJson("not-active-legacy", "adopt path must be an active .so or .qmd entry");
        legacy = legacyManifestFromActivePath(inferred, parsed.path);
    }

    PackageType type = parsed.type == PACKAGE_UNKNOWN ? legacy.type : parsed.type;
    if(type == PACKAGE_UNKNOWN) return errorJson("invalid-type", "package type could not be inferred");
    if(legacy.type != PACKAGE_UNKNOWN && legacy.type != type) return errorJson("type-mismatch", "requested type does not match legacy package type");

    std::string source = sourceEntryPath(legacy);
    if(!isPathPresent(source)) return errorJson("entry-missing", "legacy source entry was not found");

    ExtensionManifest provided;
    bool hasProvidedManifest = false;
    std::string manifestJson;
    if(!parsed.manifestPath.empty()) {
        if(!isPathPresent(parsed.manifestPath)) return errorJson("manifest-not-found", "provided manifestPath was not found");
        provided = parseManifest(parentPath(parsed.manifestPath), parsed.manifestPath, type);
        if(!provided.valid) return errorJson("invalid-manifest", jsonutil::stringArray(provided.errors));
        if(provided.type != legacy.type) return errorJson("manifest-type-mismatch", "provided manifest type does not match legacy package type");
        hasProvidedManifest = true;
        manifestJson = readFile(parsed.manifestPath);
    }

    std::string id = parsed.id.empty() ? (hasProvidedManifest ? provided.id : legacy.id) : sanitizeId(parsed.id);
    if(!validId(id)) return errorJson("invalid-id", "package id is invalid");
    type = parsed.type == PACKAGE_UNKNOWN && hasProvidedManifest ? provided.type : type;
    std::string entry = parsed.entry.empty() ? (hasProvidedManifest ? provided.entry : fileName(source)) : parsed.entry;
    if(!validEntryForType(entry, type)) return errorJson("invalid-entry", "entry is invalid for package type");
    int order = parsed.orderProvided ? parsed.order : (hasProvidedManifest ? provided.order : legacy.order);
    if(type == PACKAGE_QMD && (order < 0 || order > 999)) return errorJson("invalid-order", "QMD order must be between 0 and 999");
    std::string name = parsed.name.empty() ? (hasProvidedManifest ? provided.name : legacy.name) : parsed.name;
    std::string version = parsed.version.empty() ? (hasProvidedManifest ? provided.version : "unknown") : parsed.version;
    bool desiredEnabled = parsed.enabledProvided ? parsed.enabled : effectiveEnabled(legacy);
    if(id == "xovi-extension-manager") desiredEnabled = true;

    if(hasProvidedManifest) {
        if(provided.id != id) return errorJson("manifest-id-mismatch", "provided manifest id does not match requested id");
        if(provided.type != type) return errorJson("manifest-type-mismatch", "provided manifest type does not match requested type");
        if(provided.entry != entry) return errorJson("manifest-entry-mismatch", "provided manifest entry does not match requested entry");
        if(type == PACKAGE_QMD && provided.order != order) return errorJson("manifest-order-mismatch", "provided manifest order does not match requested order");
    }

    for(const ExtensionManifest &existing : inventory.extensions) {
        if(existing.id != id) continue;
        if(existing.managed) return errorJson("installed-conflict", "a managed package with this id already exists");
        if(existing.type != type) return errorJson("installed-type-conflict", "a legacy package with this id already exists with a different type");
    }

    std::string targetDir = targetDirForPackage(type, id);
    std::string targetManifest = joinPath(targetDir, "manifest.json");
    if(isPathPresent(targetManifest)) return errorJson("installed-conflict", "target manifest already exists");
    if(!mkdirRecursive(targetDir)) return errorJson("mkdir-failed", "target package directory could not be created");

    if(!hasProvidedManifest) {
        manifestJson = minimalManifestJson(type, id, entry, false, order, name, version);
    }

    std::string error;
    if(!replaceEnabledValue(manifestJson, false, error)) return errorJson("invalid-manifest", error);

    std::string targetEntry = joinPath(targetDir, entry);
    if(!mkdirRecursive(parentPath(targetEntry))) return errorJson("mkdir-failed", "target entry directory could not be created");
    std::vector<std::string> actions;
    if(isPathPresent(targetEntry) && !pathsReferToSameFile(source, targetEntry)) {
        return errorJson("entry-conflict", "target entry already exists and is not the legacy source");
    }
    if(pathsReferToSameFile(source, targetEntry)) {
        actions.push_back("legacy-entry-already-in-package");
    } else if(!copyFile(source, targetEntry, error)) {
        return errorJson("copy-failed", error);
    } else {
        actions.push_back("legacy-entry-copied");
    }

    if(!writeFile(targetManifest, manifestJson, error)) return errorJson("write-failed", error);
    actions.push_back(hasProvidedManifest ? "manifest-copied" : "minimal-manifest-created");

    if(desiredEnabled) {
        Inventory planned = loadInventory();
        int plannedIndex = findExtension(planned, id);
        if(plannedIndex >= 0) {
            ExtensionManifest plannedPackage = planned.extensions[static_cast<size_t>(plannedIndex)];
            plannedPackage.enabled = true;
            std::vector<std::string> issues = computeIssues(planned, plannedPackage);
            if(enabledEntryPath(plannedPackage) == enabledEntryPath(legacy)) {
                removeIssue(issues, "active-entry-conflict");
                removeIssue(issues, "effective-disabled");
            }
            if(hasBlockingEnableIssue(issues)) {
                std::ostringstream out;
                out << "{\"ok\":false,"
                    << "\"error\":\"adopt-enable-blocked\","
                    << "\"message\":\"legacy package was copied but cannot be enabled until blocking issues are fixed\","
                    << "\"id\":\"" << jsonutil::escape(id) << "\","
                    << "\"actions\":" << jsonutil::stringArray(actions) << ","
                    << "\"issues\":" << jsonutil::stringArray(issues) << "}";
                return out.str();
            }
        }
    }

    std::string preservedPath;
    if(effectiveEnabled(legacy) && !disableLegacyActiveEntry(legacy, actions, preservedPath, error)) {
        return errorJson("disable-legacy-failed", error);
    }

    std::string enableResult = setExtensionEnabled(id, desiredEnabled);
    bool enabledOk = jsonutil::readBool(enableResult, "ok", false);
    if(desiredEnabled && enabledOk) {
        actions.push_back("enabled-after-adopt");
    } else if(desiredEnabled) {
        actions.push_back("enable-after-adopt-blocked");
    } else {
        actions.push_back("left-disabled-after-adopt");
    }

    Inventory after = loadInventory();
    int afterIndex = findExtension(after, id);
    bool restart = true;
    if(afterIndex >= 0) restart = requiresRestart(after.extensions[static_cast<size_t>(afterIndex)]);

    std::ostringstream out;
    out << "{\"ok\":" << boolValue(!desiredEnabled || enabledOk) << ","
        << "\"type\":\"" << jsonutil::escape(packageTypeName(type)) << "\","
        << "\"id\":\"" << jsonutil::escape(id) << "\","
        << "\"actions\":" << jsonutil::stringArray(actions) << ","
        << "\"preservedPath\":\"" << jsonutil::escape(preservedPath) << "\","
        << "\"desiredEnabled\":" << boolValue(desiredEnabled) << ","
        << "\"requiresRestart\":" << boolValue(restart) << ","
        << "\"restartTarget\":\"xochitl\","
        << "\"enableResult\":" << enableResult;
    if(afterIndex >= 0) {
        std::string package = extensionToJson(after, after.extensions[static_cast<size_t>(afterIndex)]);
        out << ",\"package\":" << package << ",\"extension\":" << package;
    }
    out << "}";
    return out.str();
}

std::string disableLegacyPackage(const std::string &request) {
    PackageActionRequest parsed = parsePackageActionRequest(request);
    if(parsed.id.empty() && parsed.path.empty()) return errorJson("missing-target", "disableLegacy requires an id or path");

    Inventory inventory = loadInventory();
    int index = findPackageByRequest(inventory, parsed);
    if(index < 0) return errorJson("not-found", "legacy package was not found");

    ExtensionManifest legacy = inventory.extensions[static_cast<size_t>(index)];
    if(legacy.managed) return errorJson("not-legacy", "disableLegacy only applies to unmanaged legacy packages");
    if(legacy.source != "legacy") return errorJson("not-active-legacy", "package is not a legacy active file");

    std::vector<std::string> actions;
    std::string preservedPath;
    std::string error;
    if(!disableLegacyActiveEntry(legacy, actions, preservedPath, error)) {
        return errorJson("disable-legacy-failed", error);
    }
    bool changed = std::find(actions.begin(), actions.end(), "legacy-active-entry-already-absent") == actions.end();

    std::ostringstream out;
    out << "{\"ok\":true,"
        << "\"type\":\"" << jsonutil::escape(legacy.typeName) << "\","
        << "\"id\":\"" << jsonutil::escape(legacy.id) << "\","
        << "\"actions\":" << jsonutil::stringArray(actions) << ","
        << "\"preservedPath\":\"" << jsonutil::escape(preservedPath) << "\","
        << "\"requiresRestart\":" << boolValue(changed) << ","
        << "\"restartTarget\":\"xochitl\"}";
    return out.str();
}

std::string removeManagedPackage(const std::string &request) {
    PackageActionRequest parsed = parsePackageActionRequest(request);
    if(parsed.id.empty() && parsed.path.empty()) return errorJson("missing-target", "remove requires an id or path");

    Inventory inventory = loadInventory();
    int index = findPackageByRequest(inventory, parsed);
    if(index < 0) return errorJson("not-found", "package was not found");

    ExtensionManifest package = inventory.extensions[static_cast<size_t>(index)];
    if(!package.managed || !package.hasManifest) {
        if(package.managed || package.source != "legacy") {
            return errorJson("unmanaged-package", "remove only applies to managed or legacy packages");
        }
        if(isSelfPackage(package)) return errorJson("self-remove-blocked", "xovi-extension-manager cannot remove itself");

        std::vector<std::string> actions;
        std::vector<std::string> preservedPaths;
        bool changed = false;
        std::string activePath = enabledEntryPath(package);

        struct stat activeStat;
        if(lstat(activePath.c_str(), &activeStat) == 0) {
            if(S_ISLNK(activeStat.st_mode)) {
                if(unlink(activePath.c_str()) != 0) return errorJson("active-entry-remove-failed", std::strerror(errno));
                actions.push_back("legacy-active-symlink-removed");
                changed = true;
            } else if(S_ISREG(activeStat.st_mode)) {
                std::string disabledDir = legacyDisabledRoot(package.type);
                if(!mkdirRecursive(disabledDir)) return errorJson("mkdir-failed", "legacy-disabled directory could not be created");
                std::string preservedPath = uniquePathInDirectory(disabledDir, fileName(activePath));
                if(rename(activePath.c_str(), preservedPath.c_str()) != 0) return errorJson("legacy-active-move-failed", std::strerror(errno));
                actions.push_back("legacy-active-file-moved");
                preservedPaths.push_back(preservedPath);
                changed = true;
            } else {
                return errorJson("unsafe-active-entry", "legacy active entry is not a regular file or symlink");
            }
        } else {
            actions.push_back("legacy-active-entry-already-absent");
        }

        if(isLegacyDirectoryPath(package.type, package.directoryPath)) {
            std::string disabledDir = legacyDisabledRoot(package.type);
            if(!mkdirRecursive(disabledDir)) return errorJson("mkdir-failed", "legacy-disabled directory could not be created");
            std::string preservedPath = uniquePathInDirectory(disabledDir, directoryName(package.directoryPath));
            if(rename(package.directoryPath.c_str(), preservedPath.c_str()) != 0) return errorJson("legacy-directory-move-failed", std::strerror(errno));
            actions.push_back("legacy-directory-moved");
            preservedPaths.push_back(preservedPath);
            changed = true;
        } else {
            actions.push_back("legacy-directory-already-absent-or-unsafe");
        }

        if(changed) markRestartPending(package);

        std::string removedPackage = extensionToJson(inventory, package);
        std::ostringstream out;
        out << "{\"ok\":true,"
            << "\"removedKind\":\"legacy\","
            << "\"type\":\"" << jsonutil::escape(package.typeName) << "\","
            << "\"id\":\"" << jsonutil::escape(package.id) << "\","
            << "\"actions\":" << jsonutil::stringArray(actions) << ","
            << "\"preservedPaths\":" << jsonutil::stringArray(preservedPaths) << ","
            << "\"requiresRestart\":" << boolValue(changed || package.runtime.loadState == XOVI_EXTENSION_INITIALIZED) << ","
            << "\"restartTarget\":\"xochitl\","
            << "\"package\":" << removedPackage << ","
            << "\"extension\":" << removedPackage << "}";
        return out.str();
    }
    if(isSelfPackage(package)) return errorJson("self-remove-blocked", "xovi-extension-manager cannot remove itself");
    if(package.directoryPath != targetDirForPackage(package.type, package.id)) {
        return errorJson("unsafe-package-path", "package directory is outside the manager-owned layout");
    }
    if(package.type == PACKAGE_QMD) {
        std::vector<std::string> issues = qmdRequiredByIssues(inventory, package);
        if(!issues.empty()) {
            return "{\"ok\":false,\"error\":\"qmd-required-by\",\"message\":\"QMD package is required by enabled consumers\",\"issues\":" +
                jsonutil::stringArray(issues) + "}";
        }
    }

    ActiveEntryState active = inspectActiveEntry(package);
    if(active.present && !active.matchesSource) return errorJson("active-entry-conflict", "active entry points somewhere else");

    std::vector<std::string> actions;
    bool restart = requiresRestart(package) || active.present;
    std::string activePath = enabledEntryPath(package);
    if(active.present) {
        if(unlink(activePath.c_str()) != 0) {
            return errorJson("active-entry-remove-failed", std::strerror(errno));
        }
        actions.push_back(active.symlink ? "active-entry-symlink-removed" : "active-entry-file-removed");
        markRestartPending(package);
    } else {
        actions.push_back("active-entry-already-absent");
    }

    std::string error;
    if(!removeRecursive(package.directoryPath, error)) return errorJson("remove-failed", error);
    actions.push_back("package-directory-removed");
    if(restart) markRestartPending(package);

    std::string removedPackage = extensionToJson(inventory, package);
    std::ostringstream out;
    out << "{\"ok\":true,"
        << "\"removedKind\":\"managed\","
        << "\"type\":\"" << jsonutil::escape(package.typeName) << "\","
        << "\"id\":\"" << jsonutil::escape(package.id) << "\","
        << "\"actions\":" << jsonutil::stringArray(actions) << ","
        << "\"requiresRestart\":" << boolValue(restart) << ","
        << "\"restartTarget\":\"xochitl\","
        << "\"package\":" << removedPackage << ","
        << "\"extension\":" << removedPackage << "}";
    return out.str();
}

std::string requiresRestartJson() {
    Inventory inventory = loadInventory();
    bool restart = PENDING_RESTART;
    std::ostringstream pending;
    pending << "[";
    bool first = true;
    for(const ExtensionManifest &extension : inventory.extensions) {
        bool packageRestart = requiresRestart(extension);
        if(!packageRestart) continue;
        restart = true;
        if(!first) pending << ",";
        first = false;
        pending << "{"
            << "\"type\":\"" << jsonutil::escape(extension.typeName) << "\","
            << "\"id\":\"" << jsonutil::escape(extension.id) << "\","
            << "\"enabled\":" << boolValue(extension.enabled) << ","
            << "\"effectiveEnabled\":" << boolValue(effectiveEnabled(extension)) << ","
            << "\"issues\":" << jsonutil::stringArray(computeIssues(inventory, extension))
            << "}";
    }
    pending << "]";
    return std::string("{\"ok\":true,\"requiresRestart\":") + boolValue(restart) +
        ",\"restartTarget\":\"xochitl\",\"pendingPackages\":" + pending.str() + "}";
}

void scanDependenciesAtStartup() {
    Inventory inventory = loadInventory(false);
    int blockingCount = 0;
    int warningCount = 0;
    for(const ExtensionManifest &extension : inventory.extensions) {
        std::vector<std::string> issues = computeIssues(inventory, extension);
        if(issues.empty()) continue;

        bool blocking = extension.enabled && hasBlockingEnableIssue(issues);
        bool qmdOrderWarning = false;
        for(const std::string &issue : issues) {
            if(issue.rfind("qmd-order-conflict:", 0) == 0) qmdOrderWarning = true;
        }
        if(!blocking && !qmdOrderWarning) continue;

        if(blocking) ++blockingCount;
        if(qmdOrderWarning) ++warningCount;
        std::fprintf(
            stderr,
            "[xovi-extension-manager] startup dependency scan: %s package %s has issues %s\n",
            blocking ? "enabled" : "qmd",
            extension.id.c_str(),
            jsonutil::stringArray(issues).c_str()
        );
    }
    if(blockingCount > 0 || warningCount > 0) {
        std::fprintf(
            stderr,
            "[xovi-extension-manager] startup dependency scan complete: blocking=%d warnings=%d\n",
            blockingCount,
            warningCount
        );
    }
}

std::string schemaJson() {
    return "{"
        "\"ok\":true,"
        "\"manifestFiles\":{"
            "\"extension\":\"/home/root/xovi/extensions.available/<id>/manifest.json\","
            "\"qmd\":\"/home/root/xovi/qmd.available/<id>/manifest.json\""
        "},"
        "\"packageFiles\":{"
            "\"extension\":\"/home/root/xovi/extensions.available/<id>/<entry>\","
            "\"qmd\":\"/home/root/xovi/qmd.available/<id>/<entry>\""
        "},"
        "\"dataDirectories\":{"
            "\"extension\":\"/home/root/xovi/exthome/<id>\","
            "\"qmd\":\"/home/root/xovi/exthome/<id>\""
        "},"
        "\"installSupports\":[\".so\",\".qmd\",\".tar.gz\",\".tgz\",\".zip\"],"
        "\"legacySupports\":[\"active .so in extensions.d\",\"active .qmd in exthome/qt-resource-rebuilder\"],"
        "\"actions\":{"
            "\"adopt\":\"copy a legacy active file into the managed layout and create manifest.json\","
            "\"disableLegacy\":\"remove a legacy active symlink or move a legacy active file into legacy-disabled\","
            "\"remove\":\"remove a managed package or move legacy files/directories into legacy-disabled\""
        "},"
        "\"required\":[\"id\"],"
        "\"xochitlVersionMatching\":\"exact string unless an entry contains *, where * matches any sequence; a single * matches any detected xochitl version\","
        "\"defaults\":{"
            "\"manifestVersion\":1,"
            "\"type\":\"inferred from entry suffix\","
            "\"name\":\"<id>\","
            "\"version\":\"unknown\","
            "\"entry\":\"safe relative path under package root; defaults to <id>.so for extension; hard error for qmd\","
            "\"order\":50,"
            "\"enabled\":false,"
            "\"requires\":{}"
        "},"
        "\"hardErrors\":[\"invalid-json-object\",\"missing-id\",\"invalid-id\",\"invalid-type\",\"invalid-entry\",\"invalid-order\"],"
        "\"examples\":{"
          "\"extension\":{"
            "\"manifestVersion\":1,"
            "\"type\":\"extension\","
            "\"id\":\"advanced-settings\","
            "\"name\":\"Advanced Settings\","
            "\"version\":\"0.1.0\","
            "\"author\":\"lurenmax\","
            "\"description\":\"Additional xochitl settings\","
            "\"license\":\"MIT\","
            "\"requires\":{"
                "\"xovi\":\">=0.3.0\","
                "\"extensions\":{\"qt-resource-rebuilder\":\">=0.3.0\"},"
                "\"xochitl\":[\"3.27.*\",\"3.28.*\"],"
                "\"architectures\":[\"aarch64\"]"
            "},"
            "\"entry\":\"advanced-settings.so\","
            "\"enabled\":true"
          "},"
          "\"qmd\":{"
            "\"manifestVersion\":1,"
            "\"type\":\"qmd\","
            "\"id\":\"hide-dev-icon\","
            "\"name\":\"Hide Developer Icon\","
            "\"version\":\"0.1.0\","
            "\"order\":50,"
            "\"requires\":{"
                "\"extensions\":{\"qt-resource-rebuilder\":\">=0.3.0\"},"
                "\"qmd\":{\"scroll-screen-up-or-down\":\">=0.1.2\"},"
                "\"xochitl\":[\"3.28.*\"],"
                "\"architectures\":[\"aarch64\"]"
            "},"
            "\"entry\":\"HideDevIcon.qmd\","
            "\"enabled\":true"
          "}"
        "}"
    "}";
}
