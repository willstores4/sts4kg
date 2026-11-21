#pragma once
#include "catalog.h"
#include "network_ps4.h"
#include "path_info.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

enum class InstallMode { RemoteBgft = 0, LocalStorageEx = 1, BinLoaderElf = 2, DirectSave = 3 };

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
    std::vector<std::string> splitUrls;
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

inline std::string installStateLabel(const InstallPackage& p) {
    if (p.state == InstallState::Complete) {
        if (p.mode == InstallMode::DirectSave) return "Salvo";
        if (p.mode == InstallMode::BinLoaderElf) return "Enviado";
    }
    return installStateName(p.state);
}

inline std::string installStatusText(const InstallPackage& p) {
    if (p.state == InstallState::Complete) {
        if (p.mode == InstallMode::DirectSave) {
            return p.note.empty() ? ("Salvo em " + p.path) : p.note;
        }
        if (p.mode == InstallMode::BinLoaderElf && !p.note.empty()) return p.note;
        return "Instalado";
    }
    if (p.state == InstallState::Downloading) {
        if (p.mode == InstallMode::DirectSave && p.percent >= 0 && p.percent <= 100) {
            return "Baixando  " + std::to_string(p.percent) + "%";
        }
        return p.mode == InstallMode::DirectSave ? "Baixando" : "Preparando download local";
    }
    if (p.state == InstallState::Installing) {
        return "Baixando em Notificações do PS4";
    }
    return p.note.empty() ? std::string(installStateName(p.state)) : p.note;
}

inline int directHexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string sanitizeDownloadFilename(const std::string& raw, const std::string& fallback = "arquivo.bin") {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : trimStr(raw)) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out += '_';
        } else {
            out += char(c);
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty() || out == "." || out == "..") return fallback;
    if (out.size() > 160) {
        auto dot = out.rfind('.');
        std::string ext = (dot != std::string::npos && dot > 0 && out.size() - dot <= 16) ? out.substr(dot) : "";
        out = out.substr(0, 160 - ext.size()) + ext;
    }
    return out;
}

inline std::string extractDownloadFilename(const std::string& url, const std::string& fallbackTitle) {
    size_t end = url.find_first_of("?#");
    std::string cleanUrl = end == std::string::npos ? url : url.substr(0, end);
    size_t scheme = cleanUrl.find("://");
    size_t pathStart = (scheme != std::string::npos) ? cleanUrl.find('/', scheme + 3) : 0;
    std::string urlPath = (pathStart != std::string::npos) ? cleanUrl.substr(pathStart) : "";

    auto fallbackPkgOrBin = [&](const char* ext) {
        std::string base = trimStr(fallbackTitle);
        if (base.empty()) base = "arquivo";
        return sanitizeDownloadFilename(base + ext, std::string("arquivo") + ext);
    };

    // Synthetic proxy routes (/api/pkg/12.pkg) and .manifest.json descriptors don't carry the final file name
    if (urlPath.compare(0, 9, "/api/pkg/") == 0 && !fallbackTitle.empty()) {
        size_t nextSlash = urlPath.find('/', 9);
        if (nextSlash == std::string::npos) {
            return fallbackPkgOrBin(".pkg");
        }
    }
    std::string lowUrlPath = lower(urlPath);
    if (lowUrlPath.size() >= 14 &&
        lowUrlPath.compare(lowUrlPath.size() - 14, 14, ".manifest.json") == 0 &&
        !fallbackTitle.empty()) {
        return fallbackPkgOrBin(".pkg");
    }

    size_t slash = urlPath.rfind('/');
    std::string rawName = (slash != std::string::npos) ? urlPath.substr(slash + 1) : "";
    std::string decoded;
    for (size_t i = 0; i < rawName.size(); ++i) {
        if (rawName[i] == '%' && i + 2 < rawName.size()) {
            int hi = directHexDigit(rawName[i + 1]);
            int lo = directHexDigit(rawName[i + 2]);
            if (hi >= 0 && lo >= 0 && (hi != 0 || lo != 0)) {
                decoded += char((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        decoded += rawName[i];
    }
    std::string safe = sanitizeDownloadFilename(decoded, "");
    if (!safe.empty()) return safe;
    return fallbackPkgOrBin(".bin");
}

inline std::string normalizePs4PathInput(std::string raw, bool& outTrailingSlash, std::string& error) {
    outTrailingSlash = false;
    error.clear();
    raw = trimStr(raw);
    for (char& c : raw) {
        if ((unsigned char)c < 0x20) { error = "Caminho contém caracteres inválidos"; return ""; }
        if (c == '\\') c = '/';
    }
    if (raw.empty()) {
        error = "Digite um caminho no PS4 (ex: /data/ ou /data/pkg)";
        return "";
    }
    if (raw[0] != '/') {
        error = "O caminho deve começar com / (ex: /data/ ou /data/pkg)";
        return "";
    }
    outTrailingSlash = (raw.size() > 1 && raw.back() == '/');

    std::vector<std::string> parts;
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && raw[i] == '/') ++i;
        if (i >= raw.size()) break;
        size_t j = raw.find('/', i);
        if (j == std::string::npos) j = raw.size();
        std::string seg = raw.substr(i, j - i);
        i = j;
        if (seg == ".") continue;
        if (seg == "..") {
            error = "Caminho inválido (não use '..')";
            return "";
        }
        parts.push_back(seg);
    }
    if (parts.empty()) {
        error = "Informe uma subpasta como /data/ ou /data/pkg";
        return "";
    }
    std::string out;
    for (const auto& s : parts) out += "/" + s;
    return out;
}

inline std::string normalizeDirectDownloadDirInput(const std::string& raw, std::string& error) {
    bool trailingSlash = false;
    std::string clean = normalizePs4PathInput(raw, trailingSlash, error);
    if (clean.empty()) return "";
    return trailingSlash ? (clean + "/") : clean;
}

inline bool resolveDirectDownloadTarget(const std::string& rawInput, const std::string& url,
                                        const std::string& fallbackTitle, bool forceDir,
                                        std::string& outDir, std::string& outFilePath,
                                        bool& outExplicitFile, std::string& error,
                                        bool probeExisting = true) {
    outDir.clear();
    outFilePath.clear();
    outExplicitFile = false;
    bool trailingSlash = false;
    std::string clean = normalizePs4PathInput(rawInput, trailingSlash, error);
    if (clean.empty()) return false;

    bool existsDir = false, existsReg = false;
    if (probeExisting) {
        int fd = open(clean.c_str(), O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            if (portalDescriptorInfo(fd, &st) == 0) {
                existsDir = S_ISDIR(st.st_mode);
                existsReg = !existsDir;
            }
            close(fd);
        }
    }

    size_t lastSlash = clean.rfind('/');
    std::string lastSeg = clean.substr(lastSlash + 1);
    size_t dot = lastSeg.rfind('.');
    bool hasExt = (dot != std::string::npos && dot > 0 && dot + 1 < lastSeg.size());

    bool isDir = false;
    if (forceDir) {
        if (existsReg) {
            error = "Para vários arquivos, informe uma pasta (ex: /data/pkg)";
            return false;
        }
        isDir = true;
    } else if (trailingSlash || existsDir) {
        isDir = true;
    } else if (existsReg) {
        isDir = false;
    } else {
        isDir = !hasExt;
    }

    if (isDir) {
        outDir = clean;
        outFilePath = clean + "/" + extractDownloadFilename(url, fallbackTitle);
        outExplicitFile = false;
        return true;
    }
    if (lastSlash == 0) {
        error = "Escolha uma pasta gravável como /data/ ou /data/pkg";
        return false;
    }
    outDir = clean.substr(0, lastSlash);
    outFilePath = clean;
    outExplicitFile = true;
    return true;
}

struct PortalFileEntry {
    std::string name;
    std::string path;
    uint64_t size = 0;
};

// Returns 1 if regular file exists, 0 if definitely missing/not regular, -1 if access error.
inline int portalRegularFileStatus(const std::string& path, uint64_t& outSize) {
    outSize = 0;
    if (path.empty()) return 0;
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    int fd = open(path.c_str(), flags);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) return 0;
        if (errno == ELOOP) return 0;
        return -1;
    }
    struct stat st;
    int rc = portalDescriptorInfo(fd, &st);
    close(fd);
    if (rc != 0) return -1;
    if (!S_ISREG(st.st_mode)) return 0;
    outSize = st.st_size > 0 ? uint64_t(st.st_size) : 0;
    return 1;
}

inline bool portalListRegularFiles(const std::string& rawDir, std::vector<PortalFileEntry>& outFiles, std::string& error) {
    outFiles.clear();
    error.clear();
    bool trailingSlash = false;
    std::string dir = normalizePs4PathInput(rawDir, trailingSlash, error);
    if (dir.empty()) return false;

    DIR* d = opendir(dir.c_str());
    if (!d) {
        if (errno == ENOENT) return true;
        error = "Não foi possível abrir a pasta " + dir;
        return false;
    }
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) continue;
        std::string fullPath = (dir == "/" ? "" : dir) + "/" + name;
        uint64_t sz = 0;
        if (portalRegularFileStatus(fullPath, sz) == 1) {
            PortalFileEntry entry;
            entry.name = name;
            entry.path = fullPath;
            entry.size = sz;
            outFiles.push_back(std::move(entry));
        }
    }
    closedir(d);

    std::sort(outFiles.begin(), outFiles.end(), [](const PortalFileEntry& a, const PortalFileEntry& b) {
        return lower(a.name) < lower(b.name);
    });
    return true;
}

struct InstallBackend {
    bool (*registerRemoteTask)(const InstallPackage&, int&, std::string&) = nullptr;
    bool (*registerStoreTask)(const InstallPackage&, int&, std::string&) = nullptr;
    bool (*registerStorageTask)(const InstallPackage&, int&, std::string&) = nullptr;
    bool (*sendBinLoaderElf)(const InstallPackage&, std::string&) = nullptr;
    bool (*startTask)(int, std::string&) = nullptr;
    bool (*pauseTask)(int, std::string&) = nullptr;
    bool (*resumeTask)(int, std::string&) = nullptr;
    bool (*stopTask)(int, std::string&) = nullptr;
    bool (*pollTask)(int, uint64_t&, uint64_t&, int&, bool&, std::string&) = nullptr;
    bool (*isAppInstalled)(const std::string& titleId, const std::string& contentId, uint32_t contentType,
                           bool& outInstalled, std::string& error) = nullptr;
    bool (*fetchPkgHeader)(const std::string&, std::string&, std::string&) = nullptr;
    bool (*fetchUrl)(const std::string&, std::string&, size_t, std::string&) = nullptr;
    bool (*downloadPkgFile)(const std::string&, const std::string&, uint64_t,
                            PortalDownloadProgressFn, PortalDownloadStopFn, void*, std::string&) = nullptr;
    bool (*appendPkgFile)(const std::string&, const std::string&, uint64_t,
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
        uint64_t baseOffset = 0;
        uint64_t totalOverride = 0;
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
    int enqueueDirectDownload(const Game& g, const std::string& destInput, bool forceDir, std::string& outMessage);
    void togglePause(int id);
    void retryTask(int id);
    void cancelTask(int id);
    void removeTask(int id);
    int reconcileCompletedTasks();
    bool deleteDownloadedFile(const std::string& filePath, int taskId, std::string& outMessage);
    bool snapshot(uint64_t& stamp, std::vector<InstallPackage>& out);
    bool takeNotice(std::string& out);
};

InstallBackend ps4InstallBackend();
