#include <stddef.h>
#include <stdint.h>
#include "struct.h"

#ifndef PKG_URL
#define PKG_URL "https://example.com/pkg.pkg"
#endif

#ifndef PKG_NAME
#define PKG_NAME "Package"
#endif

#ifndef PKG_ID
#define PKG_ID "IV0000-WGST00001_00-PACKAGE000000000"
#endif

#ifndef PKG_ICON
#define PKG_ICON ""
#endif

#ifndef PKG_TYPE
#define PKG_TYPE "PS4GD"
#endif

#ifndef PKG_SIZE
#define PKG_SIZE 0ULL
#endif

void* memset(void* dst, int c, size_t n) {
	unsigned char* d = (unsigned char*)dst;
	for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)c;
	return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
	unsigned char* d = (unsigned char*)dst;
	const unsigned char* s = (const unsigned char*)src;
	for (size_t i = 0; i < n; ++i) d[i] = s[i];
	return dst;
}

__attribute__((used)) volatile struct WillPayloadConfig g_will_cfg = {
	WILL_PAYLOAD_MAGIC,
	(uint64_t)(PKG_SIZE),
	PKG_URL,
	PKG_NAME,
	PKG_ID,
	PKG_ICON,
	PKG_TYPE
};

int get_pkg_info(struct bgft_download_param* params) {
	volatile const struct WillPayloadConfig* cfg = &g_will_cfg;
	params->id = (const char*)(uintptr_t)cfg->id;
	params->content_url = (const char*)(uintptr_t)cfg->url;
	params->content_name = (const char*)(uintptr_t)cfg->name;
	params->icon_path = (const char*)(uintptr_t)cfg->icon;
	params->package_type = (const char*)(uintptr_t)cfg->type;
	params->package_size = (unsigned long)cfg->size;
	return 0;
}
