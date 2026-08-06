#pragma once

#include <stdbool.h>

#define XOVI_VERSION "0.3.0"

#define XOVI_EXTENSION_DISCOVERED 1
#define XOVI_EXTENSION_DLOPEN_FAILED 2
#define XOVI_EXTENSION_SHOULDLOAD_FAILED 3
#define XOVI_EXTENSION_CONDITION_FAILED 4
#define XOVI_EXTENSION_DEPENDENCY_FAILED 5
#define XOVI_EXTENSION_LINK_FAILED 6
#define XOVI_EXTENSION_INITIALIZED 7

struct XoviMetadataEntry;
struct ExtensionMetadataIterator;

struct XoViEnvironment {
    char *(*getExtensionDirectory)(const char *family);
    void (*requireExtension)(const char *name, unsigned char major, unsigned char minor, unsigned char patch);
    int (*getExtensionCount)();
    int (*getExtensionNames)(const char **table, int maxCount);
    int (*getExtensionFunctionCount)(const char *key);
    int (*getExtensionFunctionNames)(const char *extension, const char **table, int maxCount);
    int (*getMetadataEntriesCountForFunction)(const char *extension, const char *function, int functionType);
    XoviMetadataEntry **(*getMetadataChainForFunction)(const char *extension, const char *function, int functionType);
    XoviMetadataEntry *(*getMetadataEntryForFunction)(const char *extension, const char *function, int functionType, const char *metadataEntryName);
    void (*createMetadataSearchingIterator)(ExtensionMetadataIterator *iterator, const char *metadataEntryName);
    XoviMetadataEntry *(*nextFunctionMetadataEntry)(ExtensionMetadataIterator *iterator);
    int (*getScannedExtensionCount)();
    int (*getScannedExtensionNames)(const char **table, int maxCount);
    int (*getExtensionVersion)(const char *extension, unsigned char *major, unsigned char *minor, unsigned char *patch);
    XoviMetadataEntry *(*getExtensionMetadataEntry)(const char *extension, const char *metadataEntryName);
    int (*getExtensionLoadState)(const char *extension);
    const char *(*getExtensionLoadError)(const char *extension);
};

extern const XoViEnvironment *Environment;
