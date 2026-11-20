#pragma once
#include "catalog.h"
#include "network_ps4.h"
#include <pthread.h>
#include <stdint.h>
#include <string>
#include <vector>

enum class InstallMode { RemoteBgft = 0, LocalStorageEx = 1 };

enum class InstallState {
    Queued = 0,
    Downloading = 1,
    Registering = 2,
    Registered = 3,
    Installing = 4,
    Paused = 5,
    Complete = 6,
    Error = 7,
    Unavailable = 8
};

struct InstallPackage {
    int id = 0;
    int itemId = 0;
    int taskId = -1;
    InstallMode mode = InstallMode::RemoteBgft;
    InstallState state = InstallState::Queued;
    std::string name;
    std::string url;
    std::string iconUrl;
    std::string path;
    std::string contentId;
    std::string titleId;
    std::string note;
    uint64_t bytes = 0;
    uint64_t doneBytes = 0;
    uint32_t contentType = 0x1a;
    uint32_t contentFlags = 0;
    bool patch = false;
    int percent = -1;
};

inline const char* installStateName(InstallState s) {
    switch (s) {
        case InstallState::Queued: return "Na fila";
        case InstallState::Downloading: return "Baixando";
        case InstallState::Registering: return "Registrando";
        case InstallState::Registered: return "Iniciando";
        case InstallState::Installing: return "Em Notificações";
        case InstallState::Paused: return "Pausado";
        case InstallState::Complete: return "Instalado";
        case InstallState::Error: return "Erro";
        default: return "Indisponível";
    }
}

inline std::string installStatusText(const InstallPackage& p) {
    if (p.state == InstallState::Complete) return "Instalado";
    if (p.state == InstallState::Downloading) {
        if (p.percent >= 0 && p.percent <= 100) return "Baixando  " + std::to_string(p.percent) + "%";
        return "Baixando";
    }
    if (p.state == InstallState::Installing) {
        if (p.percent > 0 && p.percent < 100) return "Notificações  " + std::to_string(p.percent) + "%";
        return "Baixando em Notificações";
    }
    return p.note.empty() ? std::string(installStateName(p.state)) : p.note;
}

struct InstallBackend {
    bool (*registerRemoteTask)(const InstallPackage&, int&, std::string&) = nullptr;
    bool (*registerStorageTask)(const InstallPackage&, int&, std::string&) = nullptr;
    bool (*startTask)(int, std::string&) = nullptr;
    bool (*pauseTask)(int, std::string&) = nullptr;
    bool (*resumeTask)(int, std::string&) = nullptr;
    bool (*stopTask)(int, std::string&) = nullptr;
    bool (*pollTask)(int, uint64_t&, uint64_t&, int&, bool&, std::string&) = nullptr;
    bool (*fetchPkgHeader)(const std::string&, std::string&, std::string&) = nullptr;
    bool (*downloadPkgFile)(const std::string&, const std::string&, uint64_t,
                            PortalDownloadProgressFn, PortalDownloadStopFn, void*, std::string&) = nullptr;
    void (*notifySystem)(const std::string&) = nullptr;
};

bool inspectPackage(const std::string& path, InstallPackage& pkg, std::string& error);
inline std::string storageInstallPath(const std::string& p) {
    return p.compare(0, 6, "/data/") == 0 ? "/user" + p : p;
}

class PackageInstaller {
    struct DownloadCtx {
        PackageInstaller* self = nullptr;
        int id = 0;
        uint64_t lastVersionBump = 0;
    };

    std::string root;
    InstallBackend backend;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t worker;
    bool started = false, quitting = false;
    int nextId = 1;
    uint64_t version = 1;
    std::vector<InstallPackage> tasks;
    std::string pendingNotice;

    static void* entry(void*);
    void loop();
    bool saveLocked();
    InstallPackage* findLocked(int id);
    void setNoticeLocked(const std::string& msg);
    static void onDownloadProgress(void* user, uint64_t done, uint64_t total);
    static int onDownloadStop(void* user);

public:
    PackageInstaller(std::string rootDir, InstallBackend backendApi);
    ~PackageInstaller();
    bool start();

    int enqueueHomebrew(const Game& g, InstallMode mode, std::string& outMessage);
    void togglePause(int id);
    void retryTask(int id);
    void cancelTask(int id);
    void removeTask(int id);
    bool snapshot(uint64_t& stamp, std::vector<InstallPackage>& out);
    bool takeNotice(std::string& out);
};

InstallBackend ps4InstallBackend();
