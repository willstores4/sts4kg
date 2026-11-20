#pragma once
#include "path_info.h"

#define HB_STORE_DATA_ROOT "/data/hb-store"

static inline const char *hbStoreDataRoot(void) {
    return HB_STORE_DATA_ROOT;
}

static inline const char *nullStoreDataRoot(void) {
    return HB_STORE_DATA_ROOT;
}
