#pragma once
#include "path_info.h"

#define NULL_STORE_DATA_ROOT "/data/null-store"
#define NULL_STORE_LEGACY_DATA_ROOT "/data/portal-store"

static inline const char *nullStoreDataRoot(void) {
    struct stat info;
    // Preserve absolute download paths and any system installs still using them.
    if (portalPathInfo(NULL_STORE_DATA_ROOT, &info) != 0 && errno == ENOENT &&
        portalPathInfo(NULL_STORE_LEGACY_DATA_ROOT, &info) == 0 && S_ISDIR(info.st_mode)) {
        return NULL_STORE_LEGACY_DATA_ROOT;
    }
    return NULL_STORE_DATA_ROOT;
}
