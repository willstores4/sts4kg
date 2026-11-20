#pragma once
#include <cstdint>
#include <string>

typedef void (*PortalDownloadProgressFn)(void* user, uint64_t done, uint64_t total);
typedef int (*PortalDownloadStopFn)(void* user);

bool portalNetworkInit();
bool portalNetworkFetch(const std::string& url, std::string& out, size_t cap, const std::string& referer = "");
bool portalNetworkFetchRange(const std::string& url, std::string& out, uint64_t offset, size_t length, std::string& error);
bool portalNetworkDownloadFile(const std::string& url, const std::string& destPath, uint64_t expectedBytes,
                               PortalDownloadProgressFn progressCb, PortalDownloadStopFn stopCb, void* user,
                               std::string& error);
void portalNetworkTrace(const char*, ...);
