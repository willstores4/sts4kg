#define PORTAL_HOST
#include "../src/installer_ps4.cpp"
#include <cassert>
#include <iostream>

static int initCalls = 0, registerRemoteCalls = 0, registerStoreCalls = 0, registerStorageCalls = 0, startCalls = 0, resumeCalls = 0, pauseCalls = 0, stopCalls = 0, initialCalls = 0, termCalls = 0, nativeCalls = 0;
static int initResult = 0, registerResult = 0, taskId = 42, foregroundResult = 0, mockForegroundUser = 7, initialResult = 0, initialUser = 9, progressResult = 0, startResult = 0, resumeResult = 0;
static uint32_t bgftModuleResult = 0;
static int nativeResult = 101, nativeStartResult = 0;
static std::string expectedType = "PS4GD", nativePath;
static int expectedUser = 7, bgftModuleCalls = 0;
static const char* sandboxWord = "testSandbox";
static bool globalMissing = false, sandboxMissing = false;
static std::vector<std::string> mappedNames = {"libSceLibcInternal"};
static Progress returnedProgress = {};

bool portalNetworkFetchRange(const std::string&, std::string& out, uint64_t, size_t, std::string&) {
    out = "range-ok";
    return true;
}
bool portalNetworkFetch(const std::string&, std::string& out, size_t, const std::string&) {
    out = "{}";
    return true;
}
bool portalNetworkDownloadFile(const std::string&, const std::string&, uint64_t,
                               PortalDownloadProgressFn, PortalDownloadStopFn, void*, std::string&) {
    return true;
}
bool portalNetworkAppendFile(const std::string&, const std::string&, uint64_t,
                             PortalDownloadProgressFn, PortalDownloadStopFn, void*, std::string&) {
    return true;
}
static std::string lastTcpHost, lastTcpData;
static uint16_t lastTcpPort = 0;
bool portalNetworkSendTcp(const std::string& host, uint16_t port, const std::string& data, std::string&) {
    lastTcpHost = host;
    lastTcpPort = port;
    lastTcpData = data;
    return true;
}

extern "C" int32_t sceKernelGetModuleList(uint32_t* handles, size_t size, size_t* count) {
    assert(size >= mappedNames.size());
    *count = mappedNames.size();
    for (size_t i = 0; i < *count; i++) handles[i] = (uint32_t)i + 20;
    return 0;
}
extern "C" int32_t sceKernelGetModuleInfo(uint32_t h, OrbisKernelModuleInfo* info) {
    assert(info->size == sizeof *info && h >= 20 && h - 20 < mappedNames.size());
    snprintf(info->name, sizeof info->name, "%s", mappedNames[h - 20].c_str());
    return 0;
}
extern "C" uint32_t sceSysmoduleLoadModuleInternal(enum OrbisSysModuleInternal module) {
    assert(module == ORBIS_SYSMODULE_INTERNAL_BGFT);
    ++bgftModuleCalls;
    return bgftModuleResult;
}
extern "C" const char* sceKernelGetFsSandboxRandomWord() { return sandboxWord; }
extern "C" uint32_t sceKernelLoadStartModule(const char* path, size_t, const void*, uint32_t, void*, void* result) {
    ++nativeCalls;
    nativePath = path;
    *(int*)result = nativeStartResult;
    bool global = nativePath.find("/system/") == 0;
    if ((global && globalMissing) || (!global && sandboxMissing)) return 0x80020002u;
    return (uint32_t)nativeResult;
}
extern "C" int sceKernelDlsym(int, const char*, void**) { return -1; }
extern "C" int32_t sceUserServiceGetForegroundUser(int32_t* user) { *user = mockForegroundUser; return foregroundResult; }
extern "C" int32_t sceUserServiceGetInitialUser(int32_t* user) { ++initialCalls; *user = initialUser; return initialResult; }
extern "C" int bgftInit(Init* p) {
    ++initCalls;
    assert(moduleReady && p->heap && p->size == 1024 * 1024 && ((unsigned char*)p->heap)[0] == 0);
    return initResult;
}
extern "C" int bgftTerm() { ++termCalls; return 0; }
extern "C" int bgftRegisterRemote(Params* p, int* id) {
    ++registerRemoteCalls;
    assert(p->userId == expectedUser && p->entitlementType == 5);
    assert(std::string(p->url) == "http://192.168.1.2:8765/pkgs/hb.pkg");
    assert(std::string(p->type) == expectedType && p->option == 0x10000);
    *id = taskId;
    return registerResult;
}
extern "C" int bgftRegisterStore(Params* p, int* id, void* errInfo) {
    ++registerStoreCalls;
    assert(p->userId == expectedUser && p->entitlementType == 5 && errInfo != nullptr);
    assert(std::string(p->url) == "http://192.168.1.2:8765/manifests/split.manifest.json");
    assert(std::string(p->type) == expectedType && p->option == 0x10000);
    *id = taskId;
    return registerResult;
}
extern "C" int bgftRegisterDebug(Params* p, int* id) {
    ++registerRemoteCalls;
    assert(std::string(p->type) == expectedType);
    *id = taskId;
    return registerResult;
}
extern "C" int bgftRegisterStorage(ParamsEx* p, int* id) {
    ++registerStorageCalls;
    assert(p->slot == 0 && p->params.userId == expectedUser && p->params.entitlementType == 5);
    assert(std::string(p->params.url) == "/user/data/hb-store/downloads/7/base.pkg");
    assert(std::string(p->params.type) == expectedType && p->params.size == 12ULL * 1024 * 1024 * 1024);
    *id = taskId;
    return registerResult;
}
extern "C" int bgftStart(int id) { assert(id == 42); ++startCalls; return startResult; }
extern "C" int bgftPause(int id) { assert(id == 42); ++pauseCalls; return 0; }
extern "C" int bgftResume(int id) { assert(id == 42); ++resumeCalls; return resumeResult; }
extern "C" int bgftStop(int id) { assert(id == 42); ++stopCalls; return 0; }
extern "C" int bgftProgress(int id, Progress* p) { assert(id == 42); *p = returnedProgress; return progressResult; }

static void resetInit() {
    finalize();
    moduleReady = false;
    initCalls = termCalls = nativeCalls = bgftModuleCalls = 0;
    bgftModuleResult = 0;
    nativeResult = 101;
    nativeStartResult = 0;
    nativePath.clear();
    sandboxWord = "testSandbox";
    globalMissing = sandboxMissing = false;
    mappedNames = {"libSceLibcInternal"};
}

int main() {
    auto api = ps4InstallBackend();
    InstallPackage pkg;
    pkg.path = "/data/hb-store/downloads/7/base.pkg";
    pkg.url = "http://192.168.1.2:8765/pkgs/hb.pkg";
    pkg.name = "Homebrew";
    pkg.contentId = "IV0000-TEST00001_00-TESTPACKAGE00001";
    pkg.titleId = "TEST00001";
    pkg.contentType = 0x1a;
    pkg.bytes = 12ULL * 1024 * 1024 * 1024;

    std::string error;
    int id = 123;
    assert(api.appendPkgFile != nullptr);
    assert(api.sendBinLoaderElf != nullptr);
    assert(api.registerRemoteTask(pkg, id, error) && id == 42 && registerRemoteCalls == 1 && initCalls == 1);
    InstallPackage storePkg = pkg;
    storePkg.url = "http://192.168.1.2:8765/manifests/split.manifest.json";
    assert(api.registerStoreTask(storePkg, id, error) && id == 42 && registerStoreCalls == 1);
    assert(api.registerStorageTask(pkg, id, error) && id == 42 && registerStorageCalls == 1 && initCalls == 1);
    for (auto type : {0x1a, 0x1b, 0x1c, 0x1e}) {
        pkg.contentType = type;
        expectedType = NullBGFT::packageType(type);
        assert(api.registerStorageTask(pkg, id, error));
    }
    pkg.contentType = 0x1a;
    expectedType = "PS4GD";

    foregroundResult = -1;
    expectedUser = 9;
    assert(api.registerRemoteTask(pkg, id, error) && initialCalls == 1);
    foregroundResult = 0;
    mockForegroundUser = 7;
    expectedUser = 7;

    assert(api.startTask(42, error) && startCalls == 1);
    assert(api.pauseTask(42, error) && pauseCalls == 1);
    assert(api.resumeTask(42, error) && resumeCalls == 1);
    assert(api.stopTask(42, error) && stopCalls == 1);

    uint64_t doneBytes = 0, totalBytes = 0;
    int percent = 0;
    bool complete = false;
    returnedProgress.lengthTotal = 4096;
    returnedProgress.transferredTotal = 1024;
    returnedProgress.localCopy = 25;
    returnedProgress.preparing = 100;
    assert(api.pollTask(42, doneBytes, totalBytes, percent, complete, error) && percent == 25 && !complete);
    returnedProgress.transferredTotal = 4096;
    returnedProgress.localCopy = 100;
    assert(api.pollTask(42, doneBytes, totalBytes, percent, complete, error) && complete && percent == 100);

    resetInit();
    bgftModuleResult = 0x80020008u;
    sandboxWord = "anotherMount";
    globalMissing = true;
    assert(api.registerRemoteTask(pkg, id, error) && nativeCalls == 1 && nativePath == "/anotherMount/common/lib/libSceBgft.sprx");
    resetInit();

    std::cout << "PS4 BGFT remote + storage registration, pause/resume/stop and progress polling passed.\n";
    return 0;
}
