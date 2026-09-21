#pragma once
#include <algorithm>
#include <cctype>
#include <string>

// Stable machine codes and recovery actions; UI translates summary/recovery.
// Restart is deliberately absent: it applies a pending change, not an error fix.
namespace xem {
struct Diagnostic {
    std::string code, causeCode, severity="error", category="unknown", action="inspect";
    std::string summary="Operation failed.", recovery="Check the details before trying again.", detail;
    bool retryable=false;
};
inline bool oneOf(const std::string &s, const std::string &list) {
    return ("|"+list+"|").find("|"+s+"|") != std::string::npos;
}
inline Diagnostic classify(const std::string &code, const std::string &detail={}, const std::string &context="operation") {
    Diagnostic d; d.code=code; d.detail=detail;
    auto key=code.substr(0,code.find(':'));
    auto set=[&](const char *category,const char *action,const char *summary,const char *recovery,const char *severity="error",bool retry=false) {
        d.category=category; d.action=action; d.summary=summary; d.recovery=recovery; d.severity=severity; d.retryable=retry;
    };
    if(context=="warning" && key!="qmd-order-conflict") {
        set("metadata","inspect","Plugin information is incomplete or uncertain.","Check compatibility if the plugin does not work.","warning");
    } else if(key=="runtime-dlopen-failed") {
        std::string lower=detail; std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
        set("runtime","inspect","Plugin loading failed.","Check the loader error and install a compatible build.");
        if(lower.find("wrong elf class")!=std::string::npos || lower.find("elfclass")!=std::string::npos) {
            d.causeCode="elf-class-mismatch";
            set("compatibility","replace-build","Plugin or dependency ELF bitness does not match this process.","Install a build matching the host process and its dependencies.");
        } else if(lower.find("undefined symbol")!=std::string::npos || (lower.find("version")!=std::string::npos && lower.find("not found")!=std::string::npos)) {
            d.causeCode="abi-mismatch";
            set("compatibility","replace-build","Plugin or dependency ABI is incompatible.","Install matching plugin and dependency versions.");
        } else if(lower.find("permission denied")!=std::string::npos || lower.find("operation not permitted")!=std::string::npos) {
            d.causeCode="loader-permission-denied";
            set("filesystem","check-files","Access to a plugin or dependency was denied.","Check file permissions and mount restrictions.");
        } else if(lower.find("no such file")!=std::string::npos || lower.find("cannot open shared object")!=std::string::npos) {
            d.causeCode="loader-file-missing";
            set("dependency","restore-dependency","A plugin file or shared library is missing.","Restore the missing file or install the required dependency.");
        } else if(lower.find("invalid elf")!=std::string::npos || lower.find("file too short")!=std::string::npos) {
            d.causeCode="invalid-elf";
            set("compatibility","replace-build","The plugin or dependency file is not a valid ELF binary.","Reinstall an intact, compatible build.");
        }
    } else if(oneOf(key,"architecture-mismatch|xovi-version-incompatible|xochitl-version-incompatible|runtime-link-failed")) {
        set("compatibility","replace-build","Plugin is incompatible with this environment.","Install matching plugin and dependency versions.");
    } else if(oneOf(key,"runtime-shouldload-failed|runtime-condition-failed")) {
        set("runtime","check-conditions","Plugin loading was skipped by a load condition.","Check the supported device, process and loading conditions.","warning");
    } else if(key.find("dependency")!=std::string::npos || oneOf(key,"qmd-dependencies-invalid|qmd-required-by")) {
        set("dependency","restore-dependency","Plugin dependencies do not satisfy this operation.","Check dependency availability, versions and QMD ordering.");
    } else if(oneOf(key,"effective-disabled|effective-enabled-while-disabled|active-entry-dangling")) {
        set("activation","repair-activation","Plugin activation does not match its settings.","Repair the plugin entry, then apply any pending change.");
    } else if(oneOf(key,"active-entry-conflict|active-entry-unreadable|unsafe-active-entry|unsafe-package-path")) {
        set("activation","check-files","A plugin entry conflicts with an existing path.","Inspect the conflicting path; existing files must be preserved.");
    } else if(oneOf(key,"busy|settings-busy|policy-busy|revision-conflict|stale-notification")) {
        set("concurrency","refresh-retry","Settings or requested content have changed or are busy.","Refresh and try again after the current operation finishes.","warning",true);
    } else if(oneOf(key,"self-disable-blocked|self-remove-blocked|last-manager-entry")) {
        set("protection","keep-entry","This operation would remove access to the manager.","Keep a working manager entry enabled.","warning");
    } else if(oneOf(key,"ui-not-ready|manager-unavailable|rebuilder-unavailable|snapshot-unavailable|navigation-unavailable")) {
        set("availability","check-service","A required service is not ready or unavailable.","Wait for initialization or check whether the service loaded.","warning",true);
    } else if(oneOf(key,"page-not-found|page-unavailable|not-found|launcher-entry-not-found|action-not-available")) {
        set("availability","refresh","The requested item is unavailable.","Refresh and check whether the plugin or page is enabled.","warning",true);
    } else if(oneOf(key,"component-limit|registration-limit|status-limit|action-queue-full|revision-exhausted")) {
        set("capacity","check-plugin","A service limit has been reached.","Release unused registrations or queued actions and check the plugin.");
    } else if(oneOf(key,"action-already-pending")) {
        set("concurrency","wait","The action is already pending.","Wait for the plugin to finish handling it.","info");
    } else if(oneOf(key,"invalid-settings-file|invalid-launcher-file|invalid-policy-file|policy-invalid")) {
        set("configuration","repair-config","A configuration file is invalid.","Back up and repair the configuration; do not discard it silently.");
    } else if(oneOf(key,"invalid-component-context|invalid-component|invalid-owner|page-already-registered|registration-not-owned|duplicate-id")) {
        set("page","check-plugin","Page registration or object lifetime is invalid.","Check registration ownership, identifiers and the QML context.");
    } else if(oneOf(key,"entry-missing|source-entry-missing|manifest-not-found")) {
        set("filesystem","restore-files","A required plugin file is missing.","Restore the missing file or reinstall the plugin.");
    } else if(oneOf(key,"enable-after-install-blocked|adopt-enable-blocked|enable-blocked|repair-blocked")) {
        set("operation","resolve-issues","The operation could not complete because of blocking issues.","Resolve the listed issues; completed installation steps are retained.");
    } else if(oneOf(key,"shadowed-by-qrr|replacement-conflict|qmd-order-conflict")) {
        set("injection","check-conflict","Multiple patches target the same resource.","Check whether the replacement or ordering is intentional.","warning");
    } else if(oneOf(key,"registered|processed|resource-registered|disabled|no-applicable-changes|target-not-seen|hashtab-mode")) {
        set("injection","none","Injection stage recorded.","Resource registration does not prove that a settings page renders.","info");
    } else if(key=="no-matching-block") {
        set("injection","check-patch","No matching QML block was found.","Check whether the patch matches this firmware.","warning");
    } else if(oneOf(key,"parse-failed|parse-panic|registration-failed|registration-closed|invalid-utf8|invalid-output|apply-failed")) {
        set("injection","check-patch","QML injection could not be completed.","Check patch syntax, registration timing and firmware compatibility.");
    } else if(oneOf(key,"allocation-failed|invalid-compressed-size|decompression-failed|unsupported-compression|rcc-registration-failed|resource-registration-failed|rebuilder-disabled")) {
        set("resource","check-resources","Resource processing failed.","Check memory, resource files and rebuilder compatibility.");
    } else if(oneOf(key,"already-managed|not-legacy|not-active-legacy|unmanaged-package|installed-conflict|installed-type-conflict|installed-legacy-conflict|entry-conflict")) {
        set("operation","refresh","The current package state does not permit this operation.","Refresh and select an operation matching the package state.","warning");
    } else if(key=="activation-cleanup-failed") {
        set("activation","check-files","Disabled, but a plugin entry could not be removed.","Inspect the conflicting path; existing files must be preserved.");
    } else if(key=="navigation-failed") {
        set("page","check-plugin","The settings page could not be opened.","Check the navigation callback and its error details.");
    } else if(key.find("invalid")!=std::string::npos || key.find("missing-")==0 || key.find("unsupported-")==0 || oneOf(key,"request-too-large|unknown-command|type-mismatch|manifest-id-mismatch|manifest-type-mismatch|manifest-entry-mismatch|manifest-order-mismatch")) {
        set("request","correct-request","The request or package declaration is invalid.","Correct the reported field or package format.");
    } else if(oneOf(key,"copy-failed|extract-failed|mkdir-failed|tempdir-failed|write-failed|remove-failed|active-directory-failed|active-entry-remove-failed|active-entry-repair-failed|disable-legacy-failed|legacy-active-move-failed|legacy-directory-move-failed")) {
        set("filesystem","check-files","A file operation failed.","Check permissions, free space, file integrity and the original error.");
    }
    return d;
}
inline std::string quote(const std::string &s) {
    std::string out="\""; const char *hex="0123456789abcdef";
    for(unsigned char c:s) {
        if(c=='"' || c=='\\') {out+='\\';out+=c;}
        else if(c<32) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=c;
    }
    return out+'"';
}
inline std::string diagnosticJson(const Diagnostic &d) {
    return "{\"code\":"+quote(d.code)+",\"causeCode\":"+quote(d.causeCode)+",\"severity\":"+quote(d.severity)+",\"category\":"+quote(d.category)+",\"action\":"+quote(d.action)+",\"summary\":"+quote(d.summary)+",\"recovery\":"+quote(d.recovery)+",\"detail\":"+quote(d.detail)+",\"retryable\":"+(d.retryable?"true":"false")+"}";
}
}
