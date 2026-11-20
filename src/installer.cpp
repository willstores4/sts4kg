#include "installer.h"
#include "bgft_abi.h"
#include "install_log.h"
#include "path_info.h"
#include "queue_store.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

bool inspectPackage(const std::string& path, InstallPackage& pkg, std::string& error) {
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path.c_str(), flags);
    if (fd < 0) {
        error = "Não foi possível abrir o PKG";
        installLog("open %s: errno %d", path.c_str(), errno);
        return false;
    }
    struct stat st;
    unsigned char h[0x1000];
    bool ok = portalDescriptorInfo(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0x1000;
    size_t got = 0;
    while (ok && got < sizeof h) {
        ssize_t n = pread(fd, h + got, sizeof h - got, (off_t)got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        got += (size_t)n;
    }
    close(fd);
    if (!ok) {
        error = "Arquivo PKG inválido ou incompleto";
        return false;
    }
    std::string cid, tid;
    uint64_t size = 0;
    uint32_t type = 0, cflags = 0;
    if (!parsePkgHeaderBytes(h, got, cid, tid, size, type, cflags, error)) return false;
    if (size != (uint64_t)st.st_size) {
        error = "Tamanho do PKG diverge do cabeçalho (" + std::to_string((unsigned long long)st.st_size) +
                " vs " + std::to_string((unsigned long long)size) + ")";
        return false;
    }
    pkg.path = path;
    pkg.bytes = size;
    pkg.contentId = cid;
    pkg.titleId = tid;
    pkg.contentType = type;
    pkg.contentFlags = cflags;
    pkg.patch = NullBGFT::isPatch(type, cflags);
    return true;
}

static bool removeDirTree(const std::string& path) {
    struct stat st;
    if (portalPathInfo(path.c_str(), &st)) return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (!d) return false;
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            removeDirTree(path + "/" + e->d_name);
        }
        closedir(d);
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

PackageInstaller::PackageInstaller(std::string rootDir, InstallBackend backendApi)
    : root(std::move(rootDir)), backend(backendApi) {
    pthread_mutex_init(&mutex, nullptr);
    pthread_cond_init(&changed, nullptr);
    mkdir(root.c_str(), 0777);
    mkdir((root + "/downloads").c_str(), 0777);

    std::string raw = loadQueue(root + "/tasks.tsv");
    size_t p = 0;
    while (p < raw.size() && tasks.size() < 256) {
        auto end = raw.find('\n', p);
        if (end == std::string::npos) end = raw.size();
        std::string line = raw.substr(p, end - p);
        p = end + 1;
        std::vector<std::string> f;
        size_t k = 0;
        for (;;) {
            auto e = line.find('\t', k);
            f.push_back(line.substr(k, e - k));
            if (e == std::string::npos) break;
            k = e + 1;
        }
        if (f.size() < 15) continue;
        InstallPackage t;
        t.id = atoi(f[0].c_str());
        t.itemId = atoi(f[1].c_str());
        t.taskId = atoi(f[2].c_str());
        int m = atoi(f[3].c_str());
        t.mode = m == 1 ? InstallMode::LocalStorageEx
               : m == 2 ? InstallMode::BinLoaderElf
               : InstallMode::RemoteBgft;
        int st = atoi(f[4].c_str());
        if (t.id <= 0 || st < 0 || st > 8) continue;
        t.state = InstallState(st);
        t.name = f[5];
        t.url = f[6];
        t.iconUrl = f[7];
        t.path = f[8];
        t.contentId = f[9];
        t.titleId = t.contentId.size() == 36 ? t.contentId.substr(7, 9) : "";
        t.bytes = strtoull(f[10].c_str(), nullptr, 10);
        t.doneBytes = strtoull(f[11].c_str(), nullptr, 10);
        t.contentType = (uint32_t)strtoul(f[12].c_str(), nullptr, 10);
        if (!t.contentType) t.contentType = 0x1a;
        t.percent = std::max(-1, std::min(100, atoi(f[13].c_str())));
        t.note = f[14];
        if (f.size() >= 16 && !f[15].empty()) {
            MiniJson::Value sv;
            if (MiniJson::parse(f[15], sv) && sv.type == MiniJson::Value::Array) {
                for (const auto& el : sv.arrVal) {
                    std::string u = el.asString();
                    if (isHttpUrl(u)) t.splitUrls.push_back(u);
                }
            }
        }
        if (t.state == InstallState::Downloading || t.state == InstallState::Registering) {
            t.state = InstallState::Queued;
            t.note = "Aguardando retomada";
        }
        tasks.push_back(std::move(t));
        nextId = std::max(nextId, tasks.back().id + 1);
    }
}

PackageInstaller::~PackageInstaller() {
    pthread_mutex_lock(&mutex);
    quitting = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    if (started) pthread_join(worker, nullptr);
    pthread_cond_destroy(&changed);
    pthread_mutex_destroy(&mutex);
}

bool PackageInstaller::start() {
    if (started) return true;
    started = pthread_create(&worker, nullptr, entry, this) == 0;
    return started;
}

void* PackageInstaller::entry(void* p) {
    static_cast<PackageInstaller*>(p)->loop();
    return nullptr;
}

InstallPackage* PackageInstaller::findLocked(int id) {
    for (auto& t : tasks) if (t.id == id) return &t;
    return nullptr;
}

void PackageInstaller::setNoticeLocked(const std::string& msg) {
    pendingNotice = msg;
}

static std::string serializeSplitUrls(const std::vector<std::string>& urls) {
    if (urls.empty()) return "";
    std::string out = "[";
    for (size_t i = 0; i < urls.size(); ++i) {
        if (i) out += ",";
        out += "\"" + jsonEscape(urls[i]) + "\"";
    }
    out += "]";
    return out;
}

bool PackageInstaller::saveLocked() {
    std::string s;
    for (const auto& t : tasks) {
        s += std::to_string(t.id) + "\t" + std::to_string(t.itemId) + "\t" +
             std::to_string(t.taskId) + "\t" + std::to_string(int(t.mode)) + "\t" +
             std::to_string(int(t.state)) + "\t" + cleanField(t.name) + "\t" +
             cleanField(t.url) + "\t" + cleanField(t.iconUrl) + "\t" +
             cleanField(t.path) + "\t" + cleanField(t.contentId) + "\t" +
             std::to_string((unsigned long long)t.bytes) + "\t" +
             std::to_string((unsigned long long)t.doneBytes) + "\t" +
             std::to_string(t.contentType) + "\t" + std::to_string(t.percent) + "\t" +
             cleanField(t.note) + "\t" + cleanField(serializeSplitUrls(t.splitUrls)) + "\n";
    }
    return saveQueue(root + "/tasks.tsv", s, 0644);
}

int PackageInstaller::enqueueHomebrew(const Game& g, InstallMode mode, std::string& outMessage) {
    pthread_mutex_lock(&mutex);
    for (const auto& t : tasks) {
        if (t.url == g.url &&
            (t.state == InstallState::Queued || t.state == InstallState::Downloading ||
             t.state == InstallState::Registering || t.state == InstallState::Registered ||
             t.state == InstallState::Installing)) {
            outMessage = "Este homebrew já está na fila de Notificações.";
            int existingId = t.id;
            pthread_mutex_unlock(&mutex);
            return existingId;
        }
    }
    if (tasks.size() >= 128) {
        outMessage = "Lista de tarefas cheia. Remova itens concluídos com OPTIONS.";
        pthread_mutex_unlock(&mutex);
        return 0;
    }

    InstallPackage p;
    p.id = nextId++;
    p.itemId = g.id;
    p.mode = mode;
    p.state = InstallState::Queued;
    p.name = g.title;
    p.url = g.url;
    p.iconUrl = g.image;
    p.contentId = g.contentId;
    p.titleId = g.contentId.size() == 36 ? g.contentId.substr(7, 9) : "";
    p.bytes = g.sizeBytes;
    p.contentType = g.contentType ? g.contentType : 0x1a;
    p.contentFlags = g.contentFlags;
    p.patch = NullBGFT::isPatch(p.contentType, p.contentFlags);
    p.splitUrls = g.splitUrls;
    p.note = mode == InstallMode::RemoteBgft
                 ? "Enviando para Notificações do PS4..."
                 : mode == InstallMode::BinLoaderElf
                       ? "Gerando ELF para BinLoader (127.0.0.1:9090)..."
                       : "Aguardando download local...";
    if (mode == InstallMode::LocalStorageEx) {
        p.path = root + "/downloads/" + std::to_string(p.id) + "/package.pkg";
    }
    tasks.insert(tasks.begin(), p);
    saveLocked();
    ++version;
    outMessage = mode == InstallMode::RemoteBgft
                     ? "Adicionado! Iniciando instalação em background nas Notificações do PS4..."
                     : mode == InstallMode::BinLoaderElf
                           ? "Gerando payload .ELF e enviando para BinLoader (127.0.0.1:9090)..."
                           : "Adicionado à fila de download local.";
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    return p.id;
}

void PackageInstaller::togglePause(int id) {
    pthread_mutex_lock(&mutex);
    auto* t = findLocked(id);
    if (!t) { pthread_mutex_unlock(&mutex); return; }

    if (t->taskId >= 0) {
        int bgftId = t->taskId;
        bool isPaused = t->state == InstallState::Paused || t->state == InstallState::Error;
        pthread_mutex_unlock(&mutex);
        std::string err;
        bool ok = isPaused
                      ? (backend.resumeTask ? backend.resumeTask(bgftId, err) : (backend.startTask && backend.startTask(bgftId, err)))
                      : (backend.pauseTask && backend.pauseTask(bgftId, err));
        pthread_mutex_lock(&mutex);
        t = findLocked(id);
        if (t) {
            if (ok) {
                t->state = isPaused ? InstallState::Installing : InstallState::Paused;
                t->note = isPaused ? "Retomado em Notificações" : "Pausado (pressione X para retomar)";
            } else if (!err.empty()) {
                t->note = err;
                setNoticeLocked(err);
            }
            saveLocked();
            ++version;
        }
        pthread_mutex_unlock(&mutex);
        return;
    }

    if (t->state == InstallState::Queued || t->state == InstallState::Downloading) {
        t->state = InstallState::Paused;
        t->note = "Pausado";
    } else if (t->state == InstallState::Paused || t->state == InstallState::Error) {
        t->state = InstallState::Queued;
        t->note = "Na fila para retomar";
        pthread_cond_broadcast(&changed);
    }
    saveLocked();
    ++version;
    pthread_mutex_unlock(&mutex);
}

void PackageInstaller::retryTask(int id) {
    pthread_mutex_lock(&mutex);
    auto* t = findLocked(id);
    if (!t) { pthread_mutex_unlock(&mutex); return; }
    if (t->state == InstallState::Complete) {
        setNoticeLocked("Este pacote já foi instalado.");
        pthread_mutex_unlock(&mutex);
        return;
    }
    if (t->taskId >= 0) {
        int bgftId = t->taskId;
        pthread_mutex_unlock(&mutex);
        std::string err;
        bool ok = backend.resumeTask ? backend.resumeTask(bgftId, err) : (backend.startTask && backend.startTask(bgftId, err));
        pthread_mutex_lock(&mutex);
        t = findLocked(id);
        if (t) {
            if (ok) {
                t->state = InstallState::Installing;
                t->note = "Retomado em Notificações";
            } else {
                t->state = InstallState::Error;
                t->note = err.empty() ? "Falha ao retomar tarefa" : err;
                setNoticeLocked(t->note);
            }
            saveLocked();
            ++version;
        }
        pthread_mutex_unlock(&mutex);
        return;
    }
    t->state = InstallState::Queued;
    t->note = "Tentando novamente...";
    saveLocked();
    ++version;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
}

void PackageInstaller::cancelTask(int id) {
    pthread_mutex_lock(&mutex);
    auto* t = findLocked(id);
    if (!t) { pthread_mutex_unlock(&mutex); return; }
    int bgftId = t->taskId;
    t->state = InstallState::Error;
    t->note = "Cancelado pelo usuário";
    saveLocked();
    ++version;
    pthread_mutex_unlock(&mutex);

    if (bgftId >= 0 && backend.stopTask) {
        std::string err;
        backend.stopTask(bgftId, err);
    }
}

void PackageInstaller::removeTask(int id) {
    pthread_mutex_lock(&mutex);
    auto it = std::find_if(tasks.begin(), tasks.end(), [&](const InstallPackage& p) { return p.id == id; });
    if (it == tasks.end()) {
        pthread_mutex_unlock(&mutex);
        return;
    }
    int bgftId = it->taskId;
    bool wasActive = it->state == InstallState::Installing || it->state == InstallState::Downloading;
    std::string dir = root + "/downloads/" + std::to_string(id);
    tasks.erase(it);
    saveLocked();
    ++version;
    pthread_mutex_unlock(&mutex);

    if (bgftId >= 0 && wasActive && backend.stopTask) {
        std::string err;
        backend.stopTask(bgftId, err);
    }
    removeDirTree(dir);
}

bool PackageInstaller::snapshot(uint64_t& stamp, std::vector<InstallPackage>& out) {
    pthread_mutex_lock(&mutex);
    bool diff = stamp != version;
    if (diff) {
        out = tasks;
        stamp = version;
    }
    pthread_mutex_unlock(&mutex);
    return diff;
}

bool PackageInstaller::takeNotice(std::string& out) {
    pthread_mutex_lock(&mutex);
    bool has = !pendingNotice.empty();
    if (has) {
        out = pendingNotice;
        pendingNotice.clear();
    }
    pthread_mutex_unlock(&mutex);
    return has;
}

void PackageInstaller::onDownloadProgress(void* user, uint64_t done, uint64_t total) {
    auto* ctx = static_cast<DownloadCtx*>(user);
    if (!ctx || !ctx->self) return;
    pthread_mutex_lock(&ctx->self->mutex);
    if (auto* t = ctx->self->findLocked(ctx->id)) {
        uint64_t overallDone = ctx->baseOffset + done;
        uint64_t overallTotal = ctx->totalOverride > 0 ? ctx->totalOverride : total;
        t->doneBytes = overallDone;
        if (overallTotal > 0) t->bytes = overallTotal;
        int pct = overallTotal > 0 ? (int)std::min<uint64_t>(99, overallDone * 100 / overallTotal) : -1;
        if (pct != t->percent || overallDone < ctx->lastVersionBump || overallDone - ctx->lastVersionBump >= 512 * 1024) {
            t->percent = pct;
            ctx->lastVersionBump = overallDone;
            ++ctx->self->version;
        }
    }
    pthread_mutex_unlock(&ctx->self->mutex);
}

int PackageInstaller::onDownloadStop(void* user) {
    auto* ctx = static_cast<DownloadCtx*>(user);
    if (!ctx || !ctx->self) return 1;
    pthread_mutex_lock(&ctx->self->mutex);
    auto* t = ctx->self->findLocked(ctx->id);
    bool stop = ctx->self->quitting || !t || t->state != InstallState::Downloading;
    pthread_mutex_unlock(&ctx->self->mutex);
    return stop ? 1 : 0;
}

void PackageInstaller::loop() {
    for (;;) {
        int nextJobId = 0;
        InstallPackage job;

        pthread_mutex_lock(&mutex);
        if (quitting) {
            pthread_mutex_unlock(&mutex);
            return;
        }
        for (auto& t : tasks) {
            if (t.state == InstallState::Queued) {
                nextJobId = t.id;
                t.state = t.mode == InstallMode::LocalStorageEx ? InstallState::Downloading : InstallState::Registering;
                t.note = t.mode == InstallMode::LocalStorageEx ? "Baixando pacote PKG..."
                       : t.mode == InstallMode::BinLoaderElf   ? "Gerando ELF para BinLoader (127.0.0.1:9090)..."
                                                               : "Preparando tarefa em Notificações...";
                job = t;
                saveLocked();
                ++version;
                break;
            }
        }
        pthread_mutex_unlock(&mutex);

        if (nextJobId > 0) {
            std::string error;
            bool ok = true;

            std::string manifestUrl;
            std::vector<PkgPiece> pieces;
            uint64_t manifestSizeTotal = 0;
            // Se o link for um manifesto .json, resolve para o link direto do .pkg (até 2 saltos)
            for (int hop = 0; hop < 2 && ok && endsWithJson(job.url); ++hop) {
                if (!backend.fetchUrl) {
                    error = "Busca de manifesto JSON indisponível";
                    ok = false;
                    break;
                }
                manifestUrl = job.url;
                std::string jsonBody;
                if (!backend.fetchUrl(job.url, jsonBody, 1024 * 1024, error)) {
                    if (error.empty()) error = "Não foi possível baixar o manifesto JSON";
                    ok = false;
                    break;
                }
                std::string resolvedUrl, manifestCid;
                uint64_t manifestSize = 0;
                std::vector<PkgPiece> hopPieces;
                if (!extractPkgManifestFromJson(jsonBody, serverBaseFromCatalogUrl(job.url), resolvedUrl, hopPieces, manifestCid, manifestSize, error)) {
                    ok = false;
                    break;
                }
                job.url = resolvedUrl;
                pieces = hopPieces;
                if (manifestSize > 0) manifestSizeTotal = manifestSize;
                if (!validPkgContentId(job.contentId) && validPkgContentId(manifestCid)) {
                    job.contentId = manifestCid;
                    job.titleId = manifestCid.substr(7, 9);
                }
                if (manifestSize > 0 && (job.bytes == 0 || pieces.size() > 1)) job.bytes = manifestSize;
                if (pieces.size() > 1) break;
            }
            if (ok && pieces.empty() && job.splitUrls.size() > 1) {
                for (const auto& u : job.splitUrls) {
                    PkgPiece pc;
                    pc.url = u;
                    pieces.push_back(std::move(pc));
                }
                job.url = pieces[0].url;
            }

            if (ok && (job.mode == InstallMode::RemoteBgft || job.mode == InstallMode::BinLoaderElf)) {
                // Garante Content ID e tamanho real do cabeçalho PKG antes de enviar ao BGFT/BinLoader
                if ((!validPkgContentId(job.contentId) || job.bytes == 0) && backend.fetchPkgHeader) {
                    std::string header;
                    if (backend.fetchPkgHeader(job.url, header, error)) {
                        std::string trimmed = trimStr(header);
                        if (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[') && backend.fetchUrl) {
                            manifestUrl = job.url;
                            std::string jsonBody, resolvedUrl, manifestCid;
                            uint64_t manifestSize = 0;
                            std::vector<PkgPiece> sniffPieces;
                            if (backend.fetchUrl(job.url, jsonBody, 1024 * 1024, error) &&
                                extractPkgManifestFromJson(jsonBody, serverBaseFromCatalogUrl(job.url), resolvedUrl, sniffPieces, manifestCid, manifestSize, error)) {
                                job.url = resolvedUrl;
                                pieces = sniffPieces;
                                if (validPkgContentId(manifestCid)) {
                                    job.contentId = manifestCid;
                                    job.titleId = manifestCid.substr(7, 9);
                                }
                                if (manifestSize > 0) {
                                    manifestSizeTotal = manifestSize;
                                    job.bytes = manifestSize;
                                }
                                header.clear();
                                if (!backend.fetchPkgHeader(job.url, header, error)) ok = false;
                            } else {
                                ok = false;
                            }
                        }
                        if (ok) {
                            std::string cid, tid;
                            uint64_t sz = 0;
                            uint32_t ctype = 0x1a, cflags = 0;
                            if (parsePkgHeaderBytes((const unsigned char*)header.data(), header.size(), cid, tid, sz, ctype, cflags, error)) {
                                job.contentId = cid;
                                job.titleId = tid;
                                if (sz > 0) job.bytes = sz;
                                job.contentType = ctype;
                                job.contentFlags = cflags;
                                job.patch = NullBGFT::isPatch(ctype, cflags);
                            } else {
                                ok = false;
                            }
                        }
                    } else {
                        if (error.empty()) error = "Não foi possível ler cabeçalho do PKG na URL";
                        ok = false;
                    }
                }
                if (ok && (!validPkgContentId(job.contentId) || job.bytes == 0)) {
                    error = "Content ID ou tamanho do PKG ausente";
                    ok = false;
                }
                if (ok) {
                    if (pieces.size() > 1 && !manifestUrl.empty()) {
                        if (job.mode == InstallMode::BinLoaderElf) {
                            error = "Manifesto dividido requer 'Fazer proxy pelo painel' ou instalação local (L3)";
                            ok = false;
                        } else {
                            job.url = manifestUrl;
                            if (job.bytes == 0 && manifestSizeTotal > 0) job.bytes = manifestSizeTotal;
                            if (backend.registerStoreTask) {
                                ok = backend.registerStoreTask(job, job.taskId, error) && job.taskId >= 0;
                                if (!ok && error.find("proxy") == std::string::npos) {
                                    error += " — Ative 'Fazer proxy pelo painel' no painel web ou use L3 (Local)";
                                }
                            } else {
                                error = "Manifesto dividido requer 'Fazer proxy pelo painel' no painel web ou instalação local (L3)";
                                ok = false;
                            }
                        }
                    } else if (pieces.size() > 1) {
                        error = "Pacote dividido requer 'Fazer proxy pelo painel' no painel web ou instalação local (L3)";
                        ok = false;
                    } else if (job.mode == InstallMode::BinLoaderElf) {
                        if (backend.sendBinLoaderElf) {
                            ok = backend.sendBinLoaderElf(job, error);
                        } else {
                            error = "Envio BinLoader ELF indisponível";
                            ok = false;
                        }
                    } else {
                        ok = backend.registerRemoteTask && backend.registerRemoteTask(job, job.taskId, error) && job.taskId >= 0;
                    }
                }
            } else if (ok) {
                std::string dir = root + "/downloads/" + std::to_string(job.id);
                mkdir(dir.c_str(), 0777);
                if (pieces.size() > 1) {
                    uint64_t totalSz = job.bytes > 0 ? job.bytes : manifestSizeTotal;
                    if (totalSz == 0) {
                        for (const auto& pc : pieces) totalSz += pc.size;
                    }
                    uint64_t offsetAccum = 0;
                    for (size_t i = 0; i < pieces.size() && ok; ++i) {
                        DownloadCtx ctx;
                        ctx.self = this;
                        ctx.id = job.id;
                        ctx.lastVersionBump = 0;
                        ctx.baseOffset = offsetAccum;
                        ctx.totalOverride = totalSz;
                        bool pieceOk = false;
                        if (i == 0) {
                            pieceOk = backend.downloadPkgFile &&
                                      backend.downloadPkgFile(pieces[i].url, job.path, pieces[i].size,
                                                              onDownloadProgress, onDownloadStop, &ctx, error);
                        } else {
                            pieceOk = backend.appendPkgFile &&
                                      backend.appendPkgFile(pieces[i].url, job.path, pieces[i].size,
                                                            onDownloadProgress, onDownloadStop, &ctx, error);
                        }
                        if (!pieceOk) {
                            if (error.empty()) error = "Falha ao baixar parte " + std::to_string(i + 1) + " do PKG";
                            ok = false;
                        } else if (pieces[i].size > 0) {
                            offsetAccum += pieces[i].size;
                        } else {
                            struct stat st;
                            if (stat(job.path.c_str(), &st) == 0 && st.st_size >= 0) {
                                offsetAccum = (uint64_t)st.st_size;
                            }
                        }
                    }
                    if (ok) {
                        ok = inspectPackage(job.path, job, error) &&
                             backend.registerStorageTask &&
                             backend.registerStorageTask(job, job.taskId, error) &&
                             job.taskId >= 0;
                    }
                } else {
                    DownloadCtx ctx;
                    ctx.self = this;
                    ctx.id = job.id;
                    ctx.lastVersionBump = 0;
                    if (!backend.downloadPkgFile ||
                        !backend.downloadPkgFile(job.url, job.path, job.bytes, onDownloadProgress, onDownloadStop, &ctx, error)) {
                        ok = false;
                    } else {
                        ok = inspectPackage(job.path, job, error) &&
                             backend.registerStorageTask &&
                             backend.registerStorageTask(job, job.taskId, error) &&
                             job.taskId >= 0;
                    }
                }
            }

            if (ok) {
                pthread_mutex_lock(&mutex);
                if (auto* t = findLocked(nextJobId)) {
                    t->taskId = job.taskId;
                    t->contentId = job.contentId;
                    t->titleId = job.titleId;
                    t->bytes = job.bytes;
                    t->contentType = job.contentType;
                    t->contentFlags = job.contentFlags;
                    t->patch = job.patch;
                    if (job.mode == InstallMode::BinLoaderElf) {
                        t->state = InstallState::Complete;
                        t->percent = 100;
                        t->note = "ELF enviado para BinLoader (127.0.0.1:9090)";
                    } else {
                        t->state = InstallState::Registered;
                        t->note = "Registrado no PS4; iniciando...";
                    }
                    saveLocked();
                    ++version;
                }
                pthread_mutex_unlock(&mutex);

                if (job.mode != InstallMode::BinLoaderElf) {
                    ok = backend.startTask && backend.startTask(job.taskId, error);
                }
            }

            pthread_mutex_lock(&mutex);
            if (auto* t = findLocked(nextJobId)) {
                if (ok) {
                    if (job.mode == InstallMode::BinLoaderElf) {
                        t->state = InstallState::Complete;
                        t->percent = 100;
                        t->note = "ELF enviado para BinLoader (127.0.0.1:9090)";
                        std::string toast = "WILLGAME Store: " + t->name + " enviado via BinLoader (.ELF)";
                        setNoticeLocked(t->name + ": ELF enviado para BinLoader (127.0.0.1:9090)!");
                        if (backend.notifySystem) {
                            pthread_mutex_unlock(&mutex);
                            backend.notifySystem(toast);
                            pthread_mutex_lock(&mutex);
                        }
                    } else {
                        t->state = InstallState::Installing;
                        t->note = "Baixando em Notificações do PS4";
                        std::string toast = "WILLGAME Store: " + t->name + " adicionado às Notificações";
                        setNoticeLocked(t->name + ": adicionado às Notificações do PS4!");
                        if (backend.notifySystem) {
                            pthread_mutex_unlock(&mutex);
                            backend.notifySystem(toast);
                            pthread_mutex_lock(&mutex);
                        }
                    }
                } else {
                    if (t->state != InstallState::Paused) {
                        t->state = InstallState::Error;
                        t->note = error.empty() ? "Falha ao registrar instalação" : error;
                        setNoticeLocked(t->name + ": " + t->note);
                    }
                }
                saveLocked();
                ++version;
            }
            pthread_mutex_unlock(&mutex);
            continue;
        }

        // Atualiza progresso de tarefas BGFT ativas
        pthread_mutex_lock(&mutex);
        std::vector<InstallPackage> toPoll;
        for (const auto& t : tasks) {
            if (t.taskId >= 0 && (t.state == InstallState::Registered ||
                                  t.state == InstallState::Installing ||
                                  t.state == InstallState::Unavailable)) {
                toPoll.push_back(t);
            }
        }
        pthread_mutex_unlock(&mutex);

        for (const auto& item : toPoll) {
            std::string error;
            bool ok = true;
            if (item.state == InstallState::Registered) {
                ok = backend.resumeTask ? backend.resumeTask(item.taskId, error)
                                        : (backend.startTask && backend.startTask(item.taskId, error));
            }
            uint64_t done = item.doneBytes, total = item.bytes;
            int pct = item.percent;
            bool complete = false;
            if (ok && backend.pollTask) {
                ok = backend.pollTask(item.taskId, done, total, pct, complete, error);
            }

            pthread_mutex_lock(&mutex);
            if (auto* t = findLocked(item.id)) {
                if (t->state != InstallState::Paused && t->state != InstallState::Error) {
                    InstallState nextState = !ok ? InstallState::Unavailable : complete ? InstallState::Complete : InstallState::Installing;
                    int nextPct = complete ? 100 : std::max(-1, std::min(99, pct));
                    std::string nextNote = !ok ? error : complete ? "Instalado com sucesso!" : "Baixando em Notificações";
                    bool diff = t->state != nextState || t->percent != nextPct || t->doneBytes != done || t->note != nextNote;
                    t->state = nextState;
                    t->percent = nextPct;
                    if (done > 0) t->doneBytes = done;
                    if (total > 0) t->bytes = total;
                    t->note = nextNote;
                    if (complete && item.mode == InstallMode::LocalStorageEx) {
                        std::string dir = root + "/downloads/" + std::to_string(item.id);
                        pthread_mutex_unlock(&mutex);
                        removeDirTree(dir);
                        pthread_mutex_lock(&mutex);
                    }
                    if (diff) {
                        saveLocked();
                        ++version;
                    }
                }
            }
            pthread_mutex_unlock(&mutex);
        }

        pthread_mutex_lock(&mutex);
        if (!quitting) {
            bool hasQueued = false;
            for (const auto& t : tasks) if (t.state == InstallState::Queued) { hasQueued = true; break; }
            if (!hasQueued) {
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_sec += 1;
                pthread_cond_timedwait(&changed, &mutex, &until);
            }
        }
        pthread_mutex_unlock(&mutex);
    }
}
