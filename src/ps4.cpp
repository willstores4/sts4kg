#include "ui.h"
#include "display_ps4.h"
#include "network_ps4.h"
#include "path_info.h"
#include "text_input_ps4.h"
#include "startup_ps4.h"
#include "data_root.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

static std::string DATA;
static pthread_mutex_t syncLock = PTHREAD_MUTEX_INITIALIZER;
static bool syncWorking = false, syncReady = false;
static std::string syncStatusMsg, syncServerUrl;
static CatalogData syncedCatalog;
static bool clearCoversOnPublish = false;

static void setSyncStatus(const std::string& s) {
    pthread_mutex_lock(&syncLock);
    syncStatusMsg = s;
    pthread_mutex_unlock(&syncLock);
}

static void cleanStaleCovers(const std::string& coversDir, const std::vector<Game>& items) {
    std::set<int> validIds;
    for (const auto& g : items) validIds.insert(g.id);

    DIR* d = opendir(coversDir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        int id = atoi(name.c_str());
        if (id > 0 && !validIds.count(id)) {
            unlink((coversDir + "/" + name).c_str());
        }
    }
    closedir(d);
}

static void* syncThread(void*) {
    pthread_mutex_lock(&syncLock);
    std::string server = syncServerUrl;
    pthread_mutex_unlock(&syncLock);

    setSyncStatus("Sincronizando com " + urlHostLabel(server) + "...");
    std::string body;
    std::string baseUrl = serverBaseFromCatalogUrl(server);
    bool fetched = false;
    if (endsWithJson(server)) {
        fetched = portalNetworkFetch(server, body, 8 * 1024 * 1024);
    } else {
        fetched = portalNetworkFetch(server + "/api/catalog.json", body, 8 * 1024 * 1024) ||
                  portalNetworkFetch(server + "/catalog.json", body, 8 * 1024 * 1024);
    }
    if (!fetched) {
        setSyncStatus("Servidor offline (" + urlHostLabel(server) + ") | Cache local mantido");
        pthread_mutex_lock(&syncLock);
        syncWorking = false;
        pthread_mutex_unlock(&syncLock);
        return nullptr;
    }

    CatalogData parsed;
    std::string err;
    if (!parseCatalogJson(body, baseUrl, parsed, err)) {
        setSyncStatus("Erro no catálogo: " + err);
        pthread_mutex_lock(&syncLock);
        syncWorking = false;
        pthread_mutex_unlock(&syncLock);
        return nullptr;
    }

    writeFile(DATA + "/store-name.txt", parsed.storeName);
    writeFile(DATA + "/categories.tsv", serializeCategories(parsed.categories));
    writeFile(DATA + "/catalog.tsv", serialize(parsed.items));

    std::string coversDir = DATA + "/covers";
    mkdir(coversDir.c_str(), 0777);
    cleanStaleCovers(coversDir, parsed.items);

    pthread_mutex_lock(&syncLock);
    syncedCatalog = parsed;
    syncReady = true;
    clearCoversOnPublish = true;
    syncStatusMsg = "Sincronizado: " + std::to_string(parsed.items.size()) + " homebrews | Baixando capas...";
    pthread_mutex_unlock(&syncLock);

    int fetchedCovers = 0;
    for (const auto& g : parsed.items) {
        std::string imgPath = coversDir + "/" + std::to_string(g.id) + ".jpg";
        std::string stampPath = coversDir + "/" + std::to_string(g.id) + ".stamp";
        if (!isHttpUrl(g.image)) {
            unlink(imgPath.c_str());
            unlink(stampPath.c_str());
            continue;
        }
        std::string expectedStamp = g.image + "|" + std::to_string(g.updatedAt);

        struct stat st;
        if (stat(imgPath.c_str(), &st) == 0 && readFile(stampPath) == expectedStamp) {
            continue;
        }

        std::string bytes;
        if (portalNetworkFetch(g.image, bytes, 4 * 1024 * 1024)) {
            int w = 0, h = 0, c = 0;
            if (stbi_info_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &c) &&
                w > 0 && h > 0 && w <= 2048 && h <= 2048) {
                writeFile(imgPath, bytes);
                writeFile(stampPath, expectedStamp);
                ++fetchedCovers;
                if (fetchedCovers % 4 == 0) {
                    pthread_mutex_lock(&syncLock);
                    clearCoversOnPublish = true;
                    syncStatusMsg = "Sincronizado (" + std::to_string(parsed.items.size()) + " homebrews) | Capas atualizadas";
                    pthread_mutex_unlock(&syncLock);
                }
            }
        }
    }

    pthread_mutex_lock(&syncLock);
    if (fetchedCovers > 0) clearCoversOnPublish = true;
    syncStatusMsg = "Sincronizado com " + urlHostLabel(server) + " (" + std::to_string(parsed.items.size()) + " itens)";
    syncWorking = false;
    pthread_mutex_unlock(&syncLock);
    return nullptr;
}

static void startSync(const std::string& serverUrl) {
    pthread_mutex_lock(&syncLock);
    if (syncWorking) {
        pthread_mutex_unlock(&syncLock);
        return;
    }
    syncWorking = true;
    syncServerUrl = serverUrl;
    pthread_mutex_unlock(&syncLock);

    pthread_t th;
    if (pthread_create(&th, nullptr, syncThread, nullptr) == 0) {
        pthread_detach(th);
    } else {
        pthread_mutex_lock(&syncLock);
        syncWorking = false;
        syncStatusMsg = "Erro ao iniciar sincronização; tente com OPTIONS.";
        pthread_mutex_unlock(&syncLock);
    }
}

int main() {
    DATA = hbStoreDataRoot();
    mkdir(DATA.c_str(), 0777);
    if (!loadStartupModules()) return 1;
    mkdir((DATA + "/covers").c_str(), 0777);

    UI ui("/app0/assets", DATA);
    PackageInstaller installer(DATA, ps4InstallBackend());
    bool installerReady = installer.start();
    uint64_t installVersion = 0;
    if (installerReady) {
        installer.reconcileCompletedTasks();
        installer.snapshot(installVersion, ui.tasks);
    }

    Display display;
    if (!display.init()) {
        installLog("Startup display initialization failed");
        return 1;
    }
    installLog("Startup display ready");

    scePadInit();
    OrbisUserServiceInitializeParams param = {};
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    int user = -1;
    sceUserServiceGetInitialUser(&user);
    int pad = scePadOpen(user, 0, 0, 0);

    uint32_t prev = 0;
    int held = 0;
    NativeTextInput textInput;
    UI::KeyboardTarget nativeTarget = UI::KbNone;
    bool keyboardFallback = false, waitPadRelease = false;

    startSync(ui.serverUrl);

    for (int frame = 0;; ++frame) {
        OrbisPadData p = {};
        if (pad < 0 || scePadReadState(pad, &p) < 0 || !p.connected) {
            if (ui.notice.empty()) {
                ui.notice = "Conecte um controle DualShock 4 para navegar.";
                ui.dirty = true;
            }
            if (frame % 120 == 0) {
                sceUserServiceGetInitialUser(&user);
                if (pad >= 0) scePadClose(pad);
                pad = scePadOpen(user, 0, 0, 0);
            }
            prev = 0;
        } else {
            if (ui.notice == "Conecte um controle DualShock 4 para navegar.") {
                ui.notice.clear();
                ui.dirty = true;
            }
            uint32_t b = p.buttons;
            if (p.leftStick.x < 55) b |= ORBIS_PAD_BUTTON_LEFT;
            if (p.leftStick.x > 200) b |= ORBIS_PAD_BUTTON_RIGHT;
            if (p.leftStick.y < 55) b |= ORBIS_PAD_BUTTON_UP;
            if (p.leftStick.y > 200) b |= ORBIS_PAD_BUTTON_DOWN;
            uint32_t edge = b & ~prev;
            held = (b == prev) ? held + 1 : 0;
            if (held > 24 && held % 7 == 0) edge |= b & 0xf0;
            prev = b;

            if (textInput.isOpen() || waitPadRelease) {
                edge = 0;
                held = 0;
                if (!textInput.isOpen() && b == 0) waitPadRelease = false;
            }

            struct Bind { uint32_t b; Action a; };
            const Bind binds[] = {
                {ORBIS_PAD_BUTTON_UP, UP},
                {ORBIS_PAD_BUTTON_DOWN, DOWN},
                {ORBIS_PAD_BUTTON_LEFT, LEFT},
                {ORBIS_PAD_BUTTON_RIGHT, RIGHT},
                {ORBIS_PAD_BUTTON_CROSS, CONFIRM},
                {ORBIS_PAD_BUTTON_CIRCLE, BACK},
                {ORBIS_PAD_BUTTON_TRIANGLE, SEARCH},
                {ORBIS_PAD_BUTTON_SQUARE, FAVORITE},
                {ORBIS_PAD_BUTTON_L1, TAB_PREV},
                {ORBIS_PAD_BUTTON_R1, TAB_NEXT},
                {ORBIS_PAD_BUTTON_TOUCH_PAD, SORT},
                {ORBIS_PAD_BUTTON_L2, PAGE_PREV},
                {ORBIS_PAD_BUTTON_R2, PAGE_NEXT},
                {ORBIS_PAD_BUTTON_R3, DOWNLOADS}
            };
            for (const auto& x : binds) {
                if (edge & x.b) ui.action(x.a);
            }

            const int dlTab = ui.downloadsTab();
            if (edge & ORBIS_PAD_BUTTON_L3) {
                if ((ui.detail || ui.tab == dlTab) && !ui.keyboard && !ui.serverPicker && !ui.filesModal && !ui.deleteConfirm) {
                    ui.action(LOCAL_INSTALL);
                } else if (ui.tab != dlTab && !ui.detail && !ui.keyboard && !ui.serverPicker && !ui.filesModal && !ui.deleteConfirm) {
                    ui.action(SERVER_CONFIG);
                }
            }
            if (edge & ORBIS_PAD_BUTTON_OPTIONS) {
                if (ui.filesModal && !ui.deleteConfirm) {
                    ui.action(REFRESH);
                } else if (ui.tab == dlTab && !ui.deleteConfirm) {
                    ui.action(REMOVE_TASK);
                } else if (!ui.keyboard && !ui.serverPicker && !ui.filesModal && !ui.deleteConfirm) {
                    if (installerReady) {
                        int cleared = installer.reconcileCompletedTasks();
                        if (installer.snapshot(installVersion, ui.tasks)) ui.dirty = true;
                        if (ui.detail) ui.refreshDetailFileProbe();
                        if (cleared > 0) {
                            ui.notice = "Atualizado: " + std::to_string(cleared) +
                                        " item(ns) removido(s) de 100% (apagado/desinstalado).";
                            ui.dirty = true;
                        }
                    }
                    startSync(ui.serverUrl);
                }
            }
        }

        // Teclado nativo PS4 para Busca ou URL do Servidor
        if (!ui.keyboard && !ui.serverEditRequest) keyboardFallback = false;
        if ((ui.keyboard || ui.serverEditRequest) && !keyboardFallback && !textInput.isOpen()) {
            UI::KeyboardTarget reqTarget = ui.serverEditRequest ? UI::KbServer : ui.kbTarget;
            std::string initial = (reqTarget == UI::KbServer) ? ui.serverUrl : ui.query;
            std::string title = (reqTarget == UI::KbServer) ? "Endereço do Painel Web" : "Buscar homebrew";
            std::string ph = (reqTarget == UI::KbServer) ? "http://192.168.1.2:8765" : "Nome do homebrew";
            bool urlMode = (reqTarget == UI::KbServer);

            ui.keyboard = false;
            ui.serverEditRequest = false;
            std::string error;
            if (!textInput.begin(user, initial, error, title, ph, urlMode)) {
                ui.keyboard = true;
                ui.kbTarget = reqTarget;
                ui.kbBuffer = initial;
                keyboardFallback = true;
                ui.notice = error;
            } else {
                nativeTarget = reqTarget;
            }
            ui.dirty = true;
        }

        if (textInput.isOpen()) {
            std::string text, error;
            auto result = textInput.poll(text, error);
            if (result != NativeTextInput::Pending) {
                waitPadRelease = true;
                held = 0;
                ui.dirty = true;
                if (result == NativeTextInput::Accepted) {
                    if (nativeTarget == UI::KbServer) {
                        ui.newServerUrl = text;
                        ui.serverSaveRequest = true;
                    } else {
                        ui.query = text;
                        ui.selected = 0;
                        ui.filter();
                    }
                } else if (result == NativeTextInput::Failed) {
                    ui.notice = error;
                }
                nativeTarget = UI::KbNone;
            }
        }

        // Ações de Servidor
        if (ui.serverResetRequest) {
            ui.serverResetRequest = false;
            unlink((DATA + "/server.txt").c_str());
            ui.serverUrl = readDefaultServerUrl("/app0/assets", DATA);
            ui.covers.clear();
            ui.notice = "Servidor restaurado para " + ui.serverUrl;
            startSync(ui.serverUrl);
            ui.dirty = true;
        }
        if (ui.serverSaveRequest) {
            ui.serverSaveRequest = false;
            std::string normalized = normalizeServerUrl(ui.newServerUrl);
            if (normalized.empty()) {
                ui.notice = "URL inválida. Use o formato http://192.168.1.2:8765";
            } else {
                writeFile(DATA + "/server.txt", normalized + "\n");
                ui.serverUrl = normalized;
                ui.covers.clear();
                ui.notice = "Servidor salvo: " + normalized;
                startSync(ui.serverUrl);
            }
            ui.dirty = true;
        }
        if (ui.serverSyncRequest) {
            ui.serverSyncRequest = false;
            startSync(ui.serverUrl);
        }

        // Ações de Instalação (Notificações BGFT, Local StorageEx, BinLoader ELF ou Download Direto configurado no painel)
        if (ui.installBgRequest || ui.installLocalRequest || ui.installBinLoaderRequest) {
            bool reqLocal = ui.installLocalRequest;
            bool reqBin = ui.installBinLoaderRequest;
            ui.installBgRequest = ui.installLocalRequest = ui.installBinLoaderRequest = false;
            if (!installerReady) {
                ui.notice = "Instalador indisponível. Reinicie o app.";
            } else {
                std::vector<Game> batch = std::move(ui.installBatch);
                if (batch.empty()) batch.push_back(ui.installItem);
                std::string msg;
                int queuedCount = 0;
                for (const auto& item : batch) {
                    if (!reqLocal && !reqBin && item.directDownload) {
                        std::string dest = item.directPath.empty() ? std::string("/data/pkg") : item.directPath;
                        if (installer.enqueueDirectDownload(item, dest, false, msg) > 0) ++queuedCount;
                    } else {
                        InstallMode itemMode = reqLocal ? InstallMode::LocalStorageEx
                                             : (reqBin || item.useBinloader) ? InstallMode::BinLoaderElf
                                             : InstallMode::RemoteBgft;
                        if (installer.enqueueHomebrew(item, itemMode, msg) > 0) ++queuedCount;
                    }
                }
                ui.notice = batch.size() > 1
                    ? std::to_string(queuedCount) + " links adicionados à fila!"
                    : msg;
            }
            ui.installBatch.clear();
            ui.dirty = true;
        }

        if (ui.taskToggleRequest) { ui.taskToggleRequest = false; installer.togglePause(ui.actionTaskId); }
        if (ui.taskRetryRequest) { ui.taskRetryRequest = false; installer.retryTask(ui.actionTaskId); }
        if (ui.taskCancelRequest) { ui.taskCancelRequest = false; installer.cancelTask(ui.actionTaskId); }
        if (ui.taskRemoveRequest) { ui.taskRemoveRequest = false; installer.removeTask(ui.actionTaskId); }

        if (ui.deleteExecuteRequest) {
            ui.deleteExecuteRequest = false;
            std::string delMsg;
            if (installerReady) {
                installer.deleteDownloadedFile(ui.deleteTargetPath, ui.deleteTargetTaskId, delMsg);
                installer.reconcileCompletedTasks();
                installer.snapshot(installVersion, ui.tasks);
            } else {
                uint64_t sz = 0;
                if (portalRegularFileStatus(ui.deleteTargetPath, sz) == 1 && unlink(ui.deleteTargetPath.c_str()) != 0) {
                    delMsg = "Não foi possível excluir " + ui.deleteTargetPath;
                } else {
                    delMsg = "Arquivo excluído: " + ui.deleteTargetPath;
                }
            }
            ui.notice = delMsg;
            if (ui.detail) ui.refreshDetailFileProbe();
            if (ui.filesModal) ui.refreshFilesListing();
            ui.dirty = true;
        }

        if (ui.reconcileStatusRequest) {
            ui.reconcileStatusRequest = false;
            if (installerReady) {
                int cleared = installer.reconcileCompletedTasks();
                installer.snapshot(installVersion, ui.tasks);
                if (cleared > 0 && ui.notice.empty()) {
                    ui.notice = "Atualizado: " + std::to_string(cleared) +
                                " item(ns) removido(s) de 100% (apagado/desinstalado).";
                }
            }
            if (ui.detail) ui.refreshDetailFileProbe();
            if (ui.filesModal) ui.refreshFilesListing();
            ui.dirty = true;
        }

        if (ui.filesRefreshRequest) {
            ui.filesRefreshRequest = false;
            ui.refreshFilesListing();
            ui.dirty = true;
        }

        if (ui.detailProbeRequest) {
            ui.detailProbeRequest = false;
            ui.refreshDetailFileProbe();
            ui.dirty = true;
        }

        std::string instNotice;
        if (installer.takeNotice(instNotice)) {
            ui.notice = instNotice;
            ui.dirty = true;
        }
        if (installer.snapshot(installVersion, ui.tasks)) {
            if (ui.detail) ui.refreshDetailFileProbe();
            if (ui.filesModal) ui.refreshFilesListing();
            ui.dirty = true;
        }

        // Atualiza UI quando sincronização termina ou baixa novas capas
        pthread_mutex_lock(&syncLock);
        bool wasSyncing = ui.syncing;
        ui.syncing = syncWorking;
        if (ui.syncing != wasSyncing) ui.dirty = true;
        if (!syncStatusMsg.empty() && syncStatusMsg != ui.status) {
            ui.status = syncStatusMsg;
            ui.dirty = true;
        }
        if (clearCoversOnPublish) {
            ui.covers.clear();
            clearCoversOnPublish = false;
            ui.dirty = true;
        }
        if (syncReady) {
            ui.storeName = syncedCatalog.storeName;
            ui.storeSubtitle = syncedCatalog.storeSubtitle;
            ui.categories = syncedCatalog.categories;
            ui.games = syncedCatalog.items;
            ui.filter();
            if (ui.detail) ui.refreshDetailFileProbe();
            syncReady = false;
        }
        pthread_mutex_unlock(&syncLock);

        bool animSpin = ui.hasSpinnerActivity();
        bool animTick = animSpin && ((ui.tab == ui.downloadsTab() || ui.detail) ? (frame & 1) == 0 : (frame % 3) == 0);
        if (ui.dirty || animTick) {
            ui.animFrame = frame;
            ui.draw();
        }
        display.flip(ui.c, frame);
    }
}
