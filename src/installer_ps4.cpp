#include "installer.h"
#include "install_log.h"
#include "bgft_abi.h"
#include "network_ps4.h"
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#ifndef PORTAL_HOST
#include <orbis/libkernel.h>
#else
#include <orbis/_types/kernel.h>
extern "C" int32_t sceKernelGetModuleList(uint32_t*, size_t, size_t*);
extern "C" int32_t sceKernelGetModuleInfo(uint32_t, OrbisKernelModuleInfo*);
extern "C" uint32_t sceKernelLoadStartModule(const char*, size_t, const void*, uint32_t, void*, void*);
extern "C" int sceKernelDlsym(int, const char*, void**);
extern "C" const char* sceKernelGetFsSandboxRandomWord();
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

using NullBGFT::Init;
using NullBGFT::Params;
using NullBGFT::ParamsEx;
using NullBGFT::Progress;

extern "C" int bgftInit(Init*) __asm__("sceBgftServiceInit");
extern "C" int bgftTerm() __asm__("sceBgftServiceTerm");
extern "C" int bgftRegisterRemote(Params*, int*) __asm__("sceBgftServiceDownloadRegisterTask");
extern "C" int bgftRegisterDebug(Params*, int*) __asm__("sceBgftServiceIntDebugDownloadRegisterPkg");
extern "C" int bgftRegisterStorage(ParamsEx*, int*) __asm__("sceBgftServiceIntDownloadRegisterTaskByStorageEx");
extern "C" int bgftStart(int) __asm__("sceBgftServiceDownloadStartTask");
extern "C" int bgftPause(int) __asm__("sceBgftServiceDownloadPauseTask");
extern "C" int bgftResume(int) __asm__("sceBgftServiceDownloadResumeTask");
extern "C" int bgftStop(int) __asm__("sceBgftServiceDownloadStopTask");
extern "C" int bgftProgress(int, Progress*) __asm__("sceBgftServiceDownloadGetProgress");

static bool bgftReady = false, moduleReady = false, cleanupRegistered = false;
static bool appInstReady = false;
static void* heap = nullptr;
static std::map<int, int> progressReports;
static int (*sysNotifyFn)(int, const char*) = nullptr;

static std::string apiError(const char* what, int code) {
    installLog("%s: 0x%08X", what, (unsigned)code);
    char s[240];
    snprintf(s, sizeof s, "%s (0x%08X)", what, (unsigned)code);
    return s;
}

static void finalize() {
    if (bgftReady) {
        int r = bgftTerm();
        installLog("sceBgftServiceTerm: 0x%08X", (unsigned)r);
        bgftReady = false;
    }
    free(heap);
    heap = nullptr;
}

static int loadedModule(const char* path) {
    static_assert(sizeof(OrbisKernelModuleInfo) == 352, "PS4 module-info ABI");
    uint32_t handles[256] = {};
    size_t count = 0;
    int r = sceKernelGetModuleList(handles, 256, &count);
    if (r) {
        installLog("Installer module inventory failed: 0x%08X", (unsigned)r);
        return -1;
    }
    std::string target = path;
    auto slash = target.find_last_of('/');
    if (slash != std::string::npos) target = target.substr(slash + 1);
    auto dot = target.find('.');
    if (dot != std::string::npos) target.resize(dot);
    for (size_t i = 0; i < std::min(count, size_t(256)); i++) {
        OrbisKernelModuleInfo info = {};
        info.size = sizeof info;
        if (sceKernelGetModuleInfo(handles[i], &info)) continue;
        info.name[sizeof info.name - 1] = 0;
        std::string name = info.name;
        auto end = name.find_last_of('/');
        if (end != std::string::npos) name = name.substr(end + 1);
        end = name.find('.');
        if (end != std::string::npos) name.resize(end);
        if (name == target) {
            installLog("Installer reusing mapped module %s: handle=0x%08X", target.c_str(), handles[i]);
            return (int)handles[i];
        }
    }
    return -1;
}

static bool loadNativeModuleHandle(const char* name, const char* path, int& outHandle, std::string& error) {
    outHandle = loadedModule(path);
    if (outHandle >= 0) return true;

    const char* word = sceKernelGetFsSandboxRandomWord();
    if (word && *word) {
        const char* file = strrchr(path, '/');
        file = file ? file + 1 : path;
        std::string sandboxPath = "/" + std::string(word) + "/common/lib/" + file;
        int entryResult = 0;
        int handle = (int)sceKernelLoadStartModule(sandboxPath.c_str(), 0, 0, 0, 0, &entryResult);
        installLog("Installer module %s: sandbox path=%s handle=0x%08X start=0x%08X",
                   name, sandboxPath.c_str(), (unsigned)handle, (unsigned)entryResult);
        if (handle >= 0 && entryResult >= 0) { outHandle = handle; return true; }
        int code = handle < 0 ? handle : entryResult;
        if ((uint32_t)code != 0x80020002u && (uint32_t)code != 0x80020014u) {
            std::string stage = "Erro ao carregar módulo ";
            stage += name;
            error = apiError(stage.c_str(), code);
            return false;
        }
    }
    int entryResult = 0;
    int handle = (int)sceKernelLoadStartModule(path, 0, 0, 0, 0, &entryResult);
    installLog("Installer module %s: native path=%s handle=0x%08X start=0x%08X",
               name, path, (unsigned)handle, (unsigned)entryResult);
    if (handle >= 0 && entryResult >= 0) { outHandle = handle; return true; }
    std::string stage = "Erro ao carregar módulo ";
    stage += name;
    error = apiError(stage.c_str(), handle < 0 ? handle : entryResult);
    return false;
}

static bool loadModule(OrbisSysModuleInternal id, const char* name, const char* path, std::string& error) {
    if (loadedModule(path) >= 0) return true;
    int r = (int)sceSysmoduleLoadModuleInternal(id);
    installLog("Installer module %s id=0x%08X: sysmodule=0x%08X", name, (unsigned)id, (unsigned)r);
    if (!r) return true;
    int handle = -1;
    return loadNativeModuleHandle(name, path, handle, error);
}

static void initOptionalHelpers() {
    if (!appInstReady) {
        std::string ignore;
        int appInstHandle = -1;
        if (loadNativeModuleHandle("AppInstUtil", "/system/common/lib/libSceAppInstUtil.sprx", appInstHandle, ignore) && appInstHandle >= 0) {
            void* fn = nullptr;
            if (sceKernelDlsym(appInstHandle, "sceAppInstUtilInitialize", &fn) == 0 && fn) {
                int r = ((int (*)())fn)();
                installLog("sceAppInstUtilInitialize => 0x%08X", (unsigned)r);
                appInstReady = true;
            }
        }
    }
    if (!sysNotifyFn) {
        std::string ignore;
        int sysUtilHandle = -1;
        if (loadNativeModuleHandle("SysUtil", "/system/common/lib/libSceSysUtil.sprx", sysUtilHandle, ignore) && sysUtilHandle >= 0) {
            void* fn = nullptr;
            if (sceKernelDlsym(sysUtilHandle, "sceSysUtilSendSystemNotificationWithText", &fn) == 0 && fn) {
                sysNotifyFn = (int (*)(int, const char*))fn;
            }
        }
    }
}

static void notifySystem(const std::string& msg) {
#ifndef PORTAL_HOST
    initOptionalHelpers();
    if (sysNotifyFn && !msg.empty()) {
        sysNotifyFn(222, msg.c_str());
    }
#else
    (void)msg;
#endif
}

static bool initialize(std::string& error) {
    if (bgftReady) return true;
    installLog("HB_Store BGFT init (moduleReady=%d)", moduleReady);
    if (!moduleReady) {
        if (!loadModule(ORBIS_SYSMODULE_INTERNAL_BGFT, "BGFT", "/system/common/lib/libSceBgft.sprx", error)) return false;
        moduleReady = true;
    }
#ifndef PORTAL_HOST
    initOptionalHelpers();
#endif
    if (!cleanupRegistered) {
        if (atexit(finalize)) {
            error = "Falha ao registrar limpeza do instalador";
            return false;
        }
        cleanupRegistered = true;
    }
    const size_t bytes = 1024 * 1024;
    heap = calloc(1, bytes);
    if (!heap) {
        error = "Sem memória para o serviço BGFT";
        return false;
    }
    Init params = {heap, bytes};
    int r = bgftInit(&params);
    installLog("sceBgftServiceInit: heap=%zu result=0x%08X", bytes, (unsigned)r);
    if (r && (uint32_t)r != 0x80990001u) {
        free(heap);
        heap = nullptr;
        error = apiError("Falha ao inicializar BGFT", r);
        return false;
    }
    bgftReady = true;
    return true;
}

static bool foregroundUser(int& user, std::string& error) {
    user = -1;
    int r = sceUserServiceGetForegroundUser(&user);
    if (r || user < 0) {
        user = -1;
        r = sceUserServiceGetInitialUser(&user);
    }
    if (r) {
        error = apiError("Não foi possível obter usuário do PS4", r);
        return false;
    }
    if (user < 0) {
        error = "Faça login em um usuário no PS4 antes de instalar.";
        return false;
    }
    return true;
}

static std::string translateBgftError(int r) {
    switch ((uint32_t)r) {
        case 0x80990088u: return apiError("Conteúdo já está instalado", r);
        case 0x80990086u: return apiError("Já existe uma tarefa pendente em Notificações", r);
        case 0x80990015u:
        case 0x80990004u: return apiError("Falha de rede ou URL inacessível pelo PS4", r);
        case 0x80990039u:
        case 0x80A30026u:
        case 0x80990085u: return apiError("Espaço insuficiente no PS4", r);
        default: return apiError("Não foi possível registrar instalação", r);
    }
}

static bool registerRemoteTask(const InstallPackage& pkg, int& id, std::string& error) {
    id = -1;
    const char* type = NullBGFT::packageType(pkg.contentType);
    if (!type) { error = "Tipo de pacote PS4 não suportado"; return false; }
    if (!isHttpUrl(pkg.url)) { error = "URL do PKG inválida"; return false; }
    if (!validPkgContentId(pkg.contentId)) { error = "Content ID inválido"; return false; }
    if (!initialize(error)) return false;
    int user = -1;
    if (!foregroundUser(user, error)) return false;

    NullBGFT::Params params = {};
    params.userId = user;
    params.entitlementType = 5;
    params.id = pkg.contentId.c_str();
    params.url = pkg.url.c_str();
    params.name = pkg.name.c_str();
    params.icon = pkg.iconUrl.c_str();
    params.scenario = "0";
    params.option = NullBGFT::DisableCDNQuery;
    params.type = type;
    params.subtype = "";
    params.size = pkg.bytes;

    installLog("BGFT Remote register: url=%s content=%s type=%s size=%llu user=%d",
               pkg.url.c_str(), pkg.contentId.c_str(), type, (unsigned long long)pkg.bytes, user);

    int r = pkg.patch ? bgftRegisterDebug(&params, &id) : bgftRegisterRemote(&params, &id);
    installLog("BGFT Remote result: 0x%08X task=%d", (unsigned)r, id);
    if (r && !pkg.patch && (uint32_t)r != 0x80990088u && (uint32_t)r != 0x80990086u) {
        int r2 = bgftRegisterDebug(&params, &id);
        installLog("BGFT Debug fallback result: 0x%08X task=%d", (unsigned)r2, id);
        if (!r2) r = 0;
    }
    if (r) {
        id = -1;
        error = translateBgftError(r);
        return false;
    }
    if (id < 0) {
        error = "PS4 não retornou um ID de tarefa válido";
        return false;
    }
    return true;
}

static bool registerStorageTask(const InstallPackage& pkg, int& id, std::string& error) {
    id = -1;
    const char* type = NullBGFT::packageType(pkg.contentType);
    if (!type) { error = "Tipo de pacote PS4 não suportado"; return false; }
    if (!initialize(error)) return false;
    if (!validPkgContentId(pkg.contentId) || pkg.titleId.size() != 9 || pkg.contentId.substr(7, 9) != pkg.titleId) {
        error = "Identidade do PKG inconsistente";
        return false;
    }
    int user = -1;
    if (!foregroundUser(user, error)) return false;

    std::string path = storageInstallPath(pkg.path);
    ParamsEx params = {};
    params.params.userId = user;
    params.params.entitlementType = 5;
    params.params.id = pkg.contentId.c_str();
    params.params.url = path.c_str();
    params.params.name = pkg.name.c_str();
    params.params.icon = "";
    params.params.scenario = "0";
    params.params.option = NullBGFT::DisableCDNQuery;
    params.params.type = type;
    params.params.subtype = "";
    params.params.size = pkg.bytes;
    params.slot = 0;

    int r = bgftRegisterStorage(&params, &id);
    installLog("StorageEx register: %s => 0x%08X task=%d", path.c_str(), (unsigned)r, id);
    if (r) {
        id = -1;
        error = translateBgftError(r);
        return false;
    }
    if (id < 0) {
        error = "PS4 não retornou um ID de tarefa válido";
        return false;
    }
    return true;
}

static bool startTask(int id, std::string& error) {
    if (id < 0) { error = "ID de tarefa inválido"; return false; }
    if (!initialize(error)) return false;
    int r = bgftStart(id);
    installLog("sceBgftServiceDownloadStartTask: task=%d result=0x%08X", id, (unsigned)r);
    if (r) {
        error = apiError("Não foi possível iniciar tarefa no PS4", r);
        return false;
    }
    return true;
}

static bool pauseTask(int id, std::string& error) {
    if (id < 0) { error = "ID de tarefa inválido"; return false; }
    if (!initialize(error)) return false;
    int r = bgftPause(id);
    installLog("sceBgftServiceDownloadPauseTask: task=%d result=0x%08X", id, (unsigned)r);
    if (r) {
        error = apiError("Não foi possível pausar tarefa", r);
        return false;
    }
    return true;
}

static bool resumeTask(int id, std::string& error) {
    if (id < 0) { error = "ID de tarefa inválido"; return false; }
    if (!initialize(error)) return false;
    int r = bgftResume(id);
    installLog("sceBgftServiceDownloadResumeTask: task=%d result=0x%08X", id, (unsigned)r);
    if (r) return startTask(id, error);
    return true;
}

static bool stopTask(int id, std::string& error) {
    if (id < 0) { error = "ID de tarefa inválido"; return false; }
    if (!initialize(error)) return false;
    int r = bgftStop(id);
    installLog("sceBgftServiceDownloadStopTask: task=%d result=0x%08X", id, (unsigned)r);
    if (r) {
        error = apiError("Não foi possível cancelar tarefa", r);
        return false;
    }
    return true;
}

static bool pollTask(int id, uint64_t& doneOut, uint64_t& totalOut, int& percent, bool& complete, std::string& error) {
    complete = false;
    if (id < 0) { error = "ID de tarefa inválido"; return false; }
    if (!initialize(error)) return false;
    Progress p = {};
    int r = bgftProgress(id, &p);
    if (r || p.error) {
        installLog("BGFT progress failed: task=%d result=0x%08X error=0x%08X", id, (unsigned)r, (unsigned)p.error);
        error = apiError("Erro na tarefa em Notificações", r ? r : p.error);
        return false;
    }
    const uint64_t length = p.lengthTotal ? p.lengthTotal : p.length;
    const uint64_t done = p.lengthTotal ? p.transferredTotal : p.transferred;
    doneOut = done;
    if (length > 0) totalOut = length;
    complete = length > 0 && done >= length && p.preparing == 100 && p.localCopy == 100;
    percent = -1;
    if (complete) percent = 100;
    else if (length > 0 && done < length) percent = int((long double)done * 100 / length);
    else if (p.localCopy > 0 && p.localCopy < 100) percent = p.localCopy;

    int bucket = percent < 0 ? -1 : percent / 10;
    if (!progressReports.count(id) || progressReports[id] != bucket || complete) {
        installLog("BGFT progress: task=%d bytes=%llu/%llu prep=%d copy=%d pct=%d done=%d",
                   id, (unsigned long long)done, (unsigned long long)length, p.preparing, p.localCopy, percent, complete);
        if (progressReports.size() > 256) progressReports.clear();
        progressReports[id] = bucket;
    }
    return true;
}

static bool fetchPkgHeader(const std::string& url, std::string& outBytes, std::string& error) {
    return portalNetworkFetchRange(url, outBytes, 0, 4096, error);
}

InstallBackend ps4InstallBackend() {
    InstallBackend api;
    api.registerRemoteTask = registerRemoteTask;
    api.registerStorageTask = registerStorageTask;
    api.startTask = startTask;
    api.pauseTask = pauseTask;
    api.resumeTask = resumeTask;
    api.stopTask = stopTask;
    api.pollTask = pollTask;
    api.fetchPkgHeader = fetchPkgHeader;
    api.downloadPkgFile = portalNetworkDownloadFile;
    api.notifySystem = notifySystem;
    return api;
}
