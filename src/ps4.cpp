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
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

static std::string DATA;
static pthread_mutex_t syncLock = PTHREAD_MUTEX_INITIALIZER;
static bool syncWorking = false, syncReady = false;
static std::string syncStatusMsg, syncServerUrl, syncToken;
static CatalogData syncedCatalog;
static bool clearCoversOnPublish = false;

static bool syncAuthRequired = false, syncOfflineRequired = false, syncClearSession = false, syncAllowBypassLogin = false;
static std::string syncAuthMessage, syncStoreName, syncStoreSubtitle;

static bool syncQrUpdate = false, syncQrAvailable = false;
static std::string syncQrVersion, syncQrLabel, syncQrBytes;
static std::string syncLastQrServer, syncLastQrVersion, syncLastQrBytes;
static bool qrCheckWorking = false;
static std::string qrCheckServer;

static bool codeReqWorking = false, codeReqDone = false, codeReqOk = false, codeReqNetworkFail = false;
static std::string codeReqServer, codeReqResultErr;
static DeviceCodeResponse codeReqResult;

static bool codePollWorking = false, codePollDone = false;
static std::string codePollServer, codePollCode;
static DeviceCodePollResult codePollResult;

static void setSyncStatus(const std::string& s) {
    pthread_mutex_lock(&syncLock);
    syncStatusMsg = s;
    pthread_mutex_unlock(&syncLock);
}

static bool cleanStaleCovers(const std::string& coversDir, const std::vector<Game>& items) {
    std::set<std::string> validNames;
    for (const auto& g : items) {
        std::string fn = coverCacheFilename(g);
        if (!fn.empty()) validNames.insert(fn);
    }

    bool removed = false;
    DIR* d = opendir(coversDir.c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (!validNames.count(name)) {
            if (unlink((coversDir + "/" + name).c_str()) == 0) removed = true;
        }
    }
    closedir(d);
    return removed;
}

static void publishLoginQrClear() {
    pthread_mutex_lock(&syncLock);
    syncQrUpdate = true;
    syncQrAvailable = false;
    syncQrVersion.clear();
    syncQrLabel.clear();
    syncQrBytes.clear();
    pthread_mutex_unlock(&syncLock);
}

static void refreshLoginQrWithAuthInfo(const std::string& baseUrl, const AppAuthInfo& ai) {
    if (!ai.loginQrEnabled || ai.loginQrUrl.empty()) {
        pthread_mutex_lock(&syncLock);
        syncLastQrVersion.clear();
        syncLastQrBytes.clear();
        syncQrUpdate = true;
        syncQrAvailable = false;
        syncQrVersion.clear();
        syncQrLabel.clear();
        syncQrBytes.clear();
        pthread_mutex_unlock(&syncLock);
        return;
    }

    std::string cachedBytes;
    pthread_mutex_lock(&syncLock);
    if (syncLastQrServer == baseUrl && syncLastQrVersion == ai.loginQrVersion && !syncLastQrBytes.empty()) {
        cachedBytes = syncLastQrBytes;
    }
    pthread_mutex_unlock(&syncLock);

    if (cachedBytes.empty()) {
        std::string dlBytes;
        std::string qrUrl = resolveServerUrl(baseUrl, ai.loginQrUrl);
        if (portalNetworkFetch(qrUrl, dlBytes, 512 * 1024)) {
            int w = 0, h = 0, ch = 0;
            if (stbi_info_from_memory((const unsigned char*)dlBytes.data(), (int)dlBytes.size(), &w, &h, &ch) &&
                w > 0 && h > 0 && w <= 2048 && h <= 2048) {
                cachedBytes.swap(dlBytes);
            }
        }
    }

    pthread_mutex_lock(&syncLock);
    syncQrUpdate = true;
    if (!cachedBytes.empty()) {
        syncLastQrServer = baseUrl;
        syncLastQrVersion = ai.loginQrVersion;
        syncLastQrBytes = cachedBytes;
        syncQrAvailable = true;
        syncQrVersion = ai.loginQrVersion;
        syncQrLabel = ai.loginQrLabel;
        syncQrBytes = std::move(cachedBytes);
    } else {
        syncQrAvailable = false;
        syncQrVersion.clear();
        syncQrLabel.clear();
        syncQrBytes.clear();
    }
    pthread_mutex_unlock(&syncLock);
}

static void* qrCheckThread(void*) {
    pthread_mutex_lock(&syncLock);
    std::string server = qrCheckServer;
    std::string token = syncToken;
    pthread_mutex_unlock(&syncLock);

    std::string baseUrl = serverBaseFromCatalogUrl(server);
    std::string authBody, authErr;
    int authCode = 0;
    if (portalNetworkFetchAuth(baseUrl + "/api/app/auth-info", token, authBody, 64 * 1024, authCode, authErr)) {
        AppAuthInfo ai;
        if (parseAppAuthInfoJson(authBody, ai)) {
            refreshLoginQrWithAuthInfo(baseUrl, ai);
        } else {
            publishLoginQrClear();
        }
    } else {
        publishLoginQrClear();
    }

    pthread_mutex_lock(&syncLock);
    qrCheckWorking = false;
    pthread_mutex_unlock(&syncLock);
    return nullptr;
}

static void startQrRefresh(const std::string& serverUrl, const std::string& token = "") {
    if (endsWithJson(serverUrl)) return;
    pthread_mutex_lock(&syncLock);
    if (qrCheckWorking) {
        pthread_mutex_unlock(&syncLock);
        return;
    }
    qrCheckWorking = true;
    qrCheckServer = serverUrl;
    syncToken = token;
    pthread_mutex_unlock(&syncLock);

    pthread_t th;
    if (pthread_create(&th, nullptr, qrCheckThread, nullptr) == 0) {
        pthread_detach(th);
    } else {
        pthread_mutex_lock(&syncLock);
        qrCheckWorking = false;
        pthread_mutex_unlock(&syncLock);
    }
}

static void* syncThread(void*) {
    pthread_mutex_lock(&syncLock);
    std::string server = syncServerUrl;
    std::string token = syncToken;
    pthread_mutex_unlock(&syncLock);

    setSyncStatus("Sincronizando catálogo...");
    std::string body;
    std::string baseUrl = serverBaseFromCatalogUrl(server);

    if (!endsWithJson(server)) {
        std::string authBody, authErr;
        int authCode = 0;
        if (portalNetworkFetchAuth(baseUrl + "/api/app/auth-info", token, authBody, 64 * 1024, authCode, authErr)) {
            AppAuthInfo ai;
            if (parseAppAuthInfoJson(authBody, ai)) {
                refreshLoginQrWithAuthInfo(baseUrl, ai);
                pthread_mutex_lock(&syncLock);
                if (!ai.storeName.empty()) syncStoreName = ai.storeName;
                if (!ai.storeSubtitle.empty()) syncStoreSubtitle = ai.storeSubtitle;
                if (ai.loginRequired && !ai.authenticated) {
                    syncAuthRequired = true;
                    syncClearSession = !token.empty();
                    syncAuthMessage = token.empty() ? "" : "Sessão expirada ou bloqueada. Entre novamente.";
                    syncStatusMsg = "Autorização necessária";
                    syncWorking = false;
                    pthread_mutex_unlock(&syncLock);
                    return nullptr;
                }
                if (!ai.loginRequired) syncAllowBypassLogin = true;
                pthread_mutex_unlock(&syncLock);
            } else {
                publishLoginQrClear();
            }
        } else {
            publishLoginQrClear();
        }
    } else {
        publishLoginQrClear();
    }

    int catCode = 0;
    std::string catErr;
    bool fetched = false;
    if (endsWithJson(server)) {
        fetched = portalNetworkFetchAuth(server, token, body, 8 * 1024 * 1024, catCode, catErr);
    } else {
        fetched = portalNetworkFetchAuth(server + "/api/catalog.json", token, body, 8 * 1024 * 1024, catCode, catErr);
        if (!fetched && catCode != 401 && catCode != 403) {
            fetched = portalNetworkFetchAuth(server + "/catalog.json", token, body, 8 * 1024 * 1024, catCode, catErr);
        }
    }
    if (!fetched && (catCode == 401 || catCode == 403)) {
        std::string apiErr = parseApiErrorJson(body);
        pthread_mutex_lock(&syncLock);
        syncAuthRequired = true;
        syncClearSession = !token.empty();
        syncAuthMessage = !apiErr.empty() ? apiErr : "Autorize este console para acessar a loja.";
        syncStatusMsg = "Autorização necessária";
        syncWorking = false;
        pthread_mutex_unlock(&syncLock);
        return nullptr;
    }
    if (!fetched) {
        pthread_mutex_lock(&syncLock);
        syncOfflineRequired = true;
        syncQrUpdate = true;
        syncQrAvailable = false;
        syncQrVersion.clear();
        syncQrLabel.clear();
        syncQrBytes.clear();
        syncStatusMsg = "Servidor offline | Conecte a internet";
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

    std::string coversDir = DATA + "/covers";
    ensureDirectoryTree(coversDir);
    bool changedCovers = cleanStaleCovers(coversDir, parsed.items);

    pthread_mutex_lock(&syncLock);
    syncedCatalog = parsed;
    syncReady = true;
    clearCoversOnPublish = true;
    syncStatusMsg = "Sincronizado: " + std::to_string(parsed.items.size()) + " homebrews | Baixando capas...";
    pthread_mutex_unlock(&syncLock);

    int fetchedCovers = 0;
    for (const auto& g : parsed.items) {
        std::string fn = coverCacheFilename(g);
        if (fn.empty()) continue;
        std::string imgPath = coversDir + "/" + fn;

        struct stat st;
        if (stat(imgPath.c_str(), &st) == 0 && st.st_size > 0) {
            continue;
        }

        std::string bytes;
        if (portalNetworkFetch(g.image, bytes, 4 * 1024 * 1024)) {
            int w = 0, h = 0, c = 0;
            if (stbi_info_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &c) &&
                w > 0 && h > 0 && w <= 4096 && h <= 4096) {
                if (writeFile(imgPath, bytes)) {
                    ++fetchedCovers;
                    changedCovers = true;
                    if (fetchedCovers % 4 == 0) {
                        pthread_mutex_lock(&syncLock);
                        clearCoversOnPublish = true;
                        syncStatusMsg = "Sincronizado (" + std::to_string(parsed.items.size()) + " homebrews) | Capas atualizadas";
                        pthread_mutex_unlock(&syncLock);
                    }
                }
            }
        }
    }

    pthread_mutex_lock(&syncLock);
    if (changedCovers) clearCoversOnPublish = true;
    syncStatusMsg = "Sincronizado (" + std::to_string(parsed.items.size()) + " itens)";
    syncWorking = false;
    pthread_mutex_unlock(&syncLock);
    return nullptr;
}

static void startSync(const std::string& serverUrl, const std::string& token = "") {
    pthread_mutex_lock(&syncLock);
    if (syncWorking) {
        pthread_mutex_unlock(&syncLock);
        return;
    }
    syncWorking = true;
    syncServerUrl = serverUrl;
    syncToken = token;
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

static void* codeReqThread(void*) {
    pthread_mutex_lock(&syncLock);
    std::string server = codeReqServer;
    pthread_mutex_unlock(&syncLock);

    std::string baseUrl = serverBaseFromCatalogUrl(server);
    std::string devId = portalNetworkDeviceId();
    std::string respBody, netErr;
    int httpCode = 0;
    bool posted = portalNetworkPostJson(
        baseUrl + "/api/app/device-code/request", buildDeviceCodeRequestJson(devId), "", respBody, 64 * 1024, httpCode, netErr);

    DeviceCodeResponse parsed;
    std::string errMsg;
    bool ok = false;
    if (posted) {
        ok = parseDeviceCodeResponseJson(respBody, parsed, errMsg);
        if (ok && parsed.deviceId.empty()) parsed.deviceId = devId;
    } else {
        std::string apiErr = parseApiErrorJson(respBody);
        errMsg = !apiErr.empty() ? apiErr : (!netErr.empty() ? netErr : "Falha ao solicitar código ao servidor.");
    }

    pthread_mutex_lock(&syncLock);
    codeReqWorking = false;
    codeReqDone = true;
    codeReqOk = ok;
    codeReqNetworkFail = (!posted && httpCode == 0);
    codeReqResultErr = errMsg;
    codeReqResult = parsed;
    pthread_mutex_unlock(&syncLock);
    return nullptr;
}

static void startDeviceCodeRequest(const std::string& serverUrl) {
    pthread_mutex_lock(&syncLock);
    if (codeReqWorking) {
        pthread_mutex_unlock(&syncLock);
        return;
    }
    codeReqWorking = true;
    codeReqDone = false;
    codeReqServer = serverUrl;
    pthread_mutex_unlock(&syncLock);

    pthread_t th;
    if (pthread_create(&th, nullptr, codeReqThread, nullptr) == 0) {
        pthread_detach(th);
    } else {
        pthread_mutex_lock(&syncLock);
        codeReqWorking = false;
        codeReqDone = true;
        codeReqOk = false;
        codeReqResultErr = "Não foi possível solicitar código de autorização.";
        pthread_mutex_unlock(&syncLock);
    }
}

static void* codePollThread(void*) {
    pthread_mutex_lock(&syncLock);
    std::string server = codePollServer;
    std::string pollCode = codePollCode;
    pthread_mutex_unlock(&syncLock);

    std::string baseUrl = serverBaseFromCatalogUrl(server);
    std::string devId = portalNetworkDeviceId();
    std::string respBody, netErr;
    int httpCode = 0;
    bool fetched = portalNetworkFetchAuth(
        baseUrl + "/api/app/device-code/poll?code=" + pollCode + "&device_id=" + devId,
        "", respBody, 64 * 1024, httpCode, netErr);

    DeviceCodePollResult res;
    if (fetched) {
        parseDeviceCodePollJson(respBody, res);
        if (res.status == "authorized" && !res.session.token.empty()) {
            writeFile(DATA + "/session.tsv", serializeAppSession(res.session));
        } else if (res.status.empty()) {
            res.status = "pending";
        }
    } else if (httpCode == 400 || httpCode == 403 || httpCode == 404 || httpCode == 410) {
        parseDeviceCodePollJson(respBody, res);
        if (res.status.empty()) {
            res.status = (httpCode == 410 || httpCode == 404) ? "expired" : "rejected";
        }
        if (res.error.empty()) {
            std::string apiErr = parseApiErrorJson(respBody);
            res.error = !apiErr.empty() ? apiErr : (!netErr.empty() ? netErr : "Código inválido ou recusado.");
        }
    } else {
        res.status = "network_error";
        res.error = !netErr.empty() ? netErr : "Aguardando servidor para validar o código...";
    }

    pthread_mutex_lock(&syncLock);
    codePollWorking = false;
    codePollDone = true;
    codePollResult = res;
    pthread_mutex_unlock(&syncLock);
    return nullptr;
}

static void startDeviceCodePoll(const std::string& serverUrl, const std::string& code) {
    if (code.empty()) return;
    pthread_mutex_lock(&syncLock);
    if (codePollWorking || codeReqWorking) {
        pthread_mutex_unlock(&syncLock);
        return;
    }
    codePollWorking = true;
    codePollDone = false;
    codePollServer = serverUrl;
    codePollCode = code;
    pthread_mutex_unlock(&syncLock);

    pthread_t th;
    if (pthread_create(&th, nullptr, codePollThread, nullptr) == 0) {
        pthread_detach(th);
    } else {
        pthread_mutex_lock(&syncLock);
        codePollWorking = false;
        pthread_mutex_unlock(&syncLock);
    }
}

struct LogoutArgs {
    std::string url;
    std::string token;
};

static void* logoutThread(void* raw) {
    LogoutArgs* args = static_cast<LogoutArgs*>(raw);
    if (args) {
        std::string out, err;
        int code = 0;
        portalNetworkPostJson(args->url, "{}", args->token, out, 4096, code, err);
        delete args;
    }
    return nullptr;
}

static void startLogoutAsync(const std::string& serverUrl, const std::string& token) {
    if (token.empty()) return;
    LogoutArgs* args = new LogoutArgs{serverBaseFromCatalogUrl(serverUrl) + "/api/app/logout", token};
    pthread_t th;
    if (pthread_create(&th, nullptr, logoutThread, args) == 0) {
        pthread_detach(th);
    } else {
        delete args;
    }
}

int main() {
    DATA = hbStoreDataRoot();
    ensureDirectoryTree(DATA);
    if (!loadStartupModules()) return 1;
    ensureDirectoryTree(DATA + "/covers");

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
    ui.deviceId = portalNetworkDeviceId();

    std::string activeToken;
    AppSession savedSess = loadAppSession(readFile(DATA + "/session.tsv"));
    if (!savedSess.token.empty()) {
        activeToken = savedSess.token;
        ui.loggedUsername = savedSess.username;
        ui.loggedDisplayName = savedSess.displayName;
    }

    // Sempre inicia bloqueado na tela de autorização até o servidor confirmar acesso válido ou loja aberta
    ui.loginScreen = true;
    ui.dirty = true;

    bool autoCodeRequested = false;
    bool offlineWaitReconnect = false;
    int codePollFrameAccum = 0;

    startSync(ui.serverUrl, activeToken);

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
                if (ui.loginScreen && !ui.keyboard && !ui.serverPicker) {
                    ui.action(SERVER_CONFIG);
                } else if ((ui.detail || ui.tab == dlTab) && !ui.keyboard && !ui.serverPicker && !ui.filesModal && !ui.deleteConfirm) {
                    ui.action(LOCAL_INSTALL);
                } else if (ui.tab != dlTab && !ui.detail && !ui.keyboard && !ui.serverPicker && !ui.filesModal && !ui.deleteConfirm) {
                    ui.action(SERVER_CONFIG);
                }
            }
            if (edge & ORBIS_PAD_BUTTON_OPTIONS) {
                if (ui.loginScreen && !ui.keyboard && !ui.serverPicker) {
                    ui.action(REFRESH);
                } else if (ui.filesModal && !ui.deleteConfirm) {
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
                    startSync(ui.serverUrl, activeToken);
                }
            }
        }

        // Teclado nativo PS4 para Busca (IP fixo em HB_STORE_FIXED_SERVER_URL)
        if (!ui.keyboard) keyboardFallback = false;
        ui.serverEditRequest = false;
        if (ui.keyboard && !keyboardFallback && !textInput.isOpen()) {
            UI::KeyboardTarget reqTarget = ui.kbTarget;
            std::string initial = ui.query;
            std::string title = "Buscar homebrew";
            std::string ph = "Nome do homebrew";

            ui.keyboard = false;
            std::string error;
            if (!textInput.begin(user, initial, error, title, ph, false)) {
                ui.keyboard = true;
                ui.kbTarget = reqTarget;
                ui.kbBuffer = initial;
                ui.kbShift = false;
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
                    if (nativeTarget == UI::KbSearch) {
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

        // Ações de Servidor fixo / Código de Ativação
        ui.serverResetRequest = false;
        ui.serverSaveRequest = false;
        if (ui.serverSyncRequest) {
            ui.serverSyncRequest = false;
            startSync(ui.serverUrl, activeToken);
        }
        if (ui.requestNewCodeSignal) {
            ui.requestNewCodeSignal = false;
            offlineWaitReconnect = false;
            autoCodeRequested = true;
            ui.codeRequesting = true;
            codePollFrameAccum = 0;
            startDeviceCodeRequest(ui.serverUrl);
        }
        if (ui.pollCodeSignal) {
            ui.pollCodeSignal = false;
            if (!ui.activationCode.empty()) {
                codePollFrameAccum = 0;
                startDeviceCodePoll(ui.serverUrl, ui.activationCode);
            }
        }
        if (ui.logoutRequest) {
            ui.logoutRequest = false;
            startLogoutAsync(ui.serverUrl, activeToken);
            activeToken.clear();
            unlink((DATA + "/session.tsv").c_str());
            ui.loggedUsername.clear();
            ui.loggedDisplayName.clear();
            ui.activationCode.clear();
            ui.activationExpiresIn = 0;
            ui.activationError.clear();
            ui.activationField = 0;
            ui.loginScreen = true;
            autoCodeRequested = false;
            offlineWaitReconnect = false;
            codePollFrameAccum = 0;
            ui.detail = false;
            ui.filesModal = false;
            ui.deleteConfirm = false;
            ui.tab = 0;
            ui.notice = "Sessão encerrada neste console.";
            ui.dirty = true;
            startQrRefresh(ui.serverUrl, "");
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

        // Atualiza UI quando sincronização / código / polling termina ou baixa novas capas
        static bool qrLostOnOffline = false;
        bool triggerPostLoginSync = false;
        bool triggerQrRefresh = false;
        pthread_mutex_lock(&syncLock);
        bool wasSyncing = ui.syncing;
        ui.syncing = syncWorking;
        if (ui.syncing != wasSyncing) ui.dirty = true;
        if (!syncStoreName.empty() && syncStoreName != ui.storeName) {
            ui.storeName = syncStoreName;
            ui.dirty = true;
        }
        if (!syncStoreSubtitle.empty() && syncStoreSubtitle != ui.storeSubtitle) {
            ui.storeSubtitle = syncStoreSubtitle;
            ui.dirty = true;
        }
        if (syncAllowBypassLogin) {
            offlineWaitReconnect = false;
            syncOfflineRequired = false;
            if (ui.loginScreen) {
                ui.loginScreen = false;
                ui.activationCode.clear();
                ui.activationError.clear();
                ui.activationExpiresIn = 0;
                ui.dirty = true;
            }
            syncAllowBypassLogin = false;
        }
        if (syncAuthRequired) {
            bool hadSavedSession = syncClearSession;
            syncAuthRequired = false;
            syncOfflineRequired = false;
            offlineWaitReconnect = false;
            if (syncClearSession) {
                syncClearSession = false;
                activeToken.clear();
                unlink((DATA + "/session.tsv").c_str());
                ui.loggedUsername.clear();
                ui.loggedDisplayName.clear();
            }
            bool wasHidden = !ui.loginScreen;
            ui.loginScreen = true;
            ui.detail = false;
            ui.filesModal = false;
            ui.deleteConfirm = false;
            if (!syncAuthMessage.empty() && (hadSavedSession || ui.activationCode.empty())) {
                ui.activationError = syncAuthMessage;
            } else if (ui.activationCode.empty()) {
                ui.activationError.clear();
            }
            syncAuthMessage.clear();
            if (wasHidden) {
                autoCodeRequested = false;
                codePollFrameAccum = 0;
            }
            ui.dirty = true;
        }
        if (syncOfflineRequired) {
            syncOfflineRequired = false;
            offlineWaitReconnect = true;
            qrLostOnOffline = true;
            ui.loginScreen = true;
            ui.loginQrVisible = false;
            ui.loginQr = Bitmap();
            ui.loginQrVersion.clear();
            ui.loginQrLabel.clear();
            ui.detail = false;
            ui.filesModal = false;
            ui.deleteConfirm = false;
            ui.activationError = "Sem conexão com a internet / servidor offline. Conecte a internet e pressione OPTIONS.";
            ui.dirty = true;
        }
        if (syncQrUpdate) {
            syncQrUpdate = false;
            if (syncQrAvailable && !syncQrBytes.empty()) {
                Bitmap qrBm = loadBitmapFromMemory((const unsigned char*)syncQrBytes.data(), (int)syncQrBytes.size());
                if (!qrBm.p.empty()) {
                    ui.loginQr = std::move(qrBm);
                    ui.loginQrVersion = syncQrVersion;
                    ui.loginQrLabel = syncQrLabel;
                    ui.loginQrVisible = true;
                    qrLostOnOffline = false;
                } else {
                    ui.loginQrVisible = false;
                    ui.loginQr = Bitmap();
                    ui.loginQrVersion.clear();
                    ui.loginQrLabel.clear();
                }
            } else {
                ui.loginQrVisible = false;
                ui.loginQr = Bitmap();
                ui.loginQrVersion.clear();
                ui.loginQrLabel.clear();
            }
            syncQrBytes.clear();
            ui.dirty = true;
        }
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
            offlineWaitReconnect = false;
            syncOfflineRequired = false;
            ui.storeName = syncedCatalog.storeName;
            ui.storeSubtitle = syncedCatalog.storeSubtitle;
            ui.categories = syncedCatalog.categories;
            ui.games = syncedCatalog.items;
            ui.loginScreen = false;
            ui.activationCode.clear();
            ui.activationError.clear();
            ui.activationExpiresIn = 0;
            ui.filter();
            if (ui.detail) ui.refreshDetailFileProbe();
            syncReady = false;
        }
        if (codeReqDone) {
            codeReqDone = false;
            ui.codeRequesting = false;
            if (codeReqOk && ui.loginScreen) {
                offlineWaitReconnect = false;
                ui.activationCode = codeReqResult.code;
                ui.activationExpiresIn = codeReqResult.expiresIn;
                if (!codeReqResult.deviceId.empty()) ui.deviceId = codeReqResult.deviceId;
                ui.activationError.clear();
                codePollFrameAccum = 0;
                if (qrLostOnOffline || !ui.loginQrVisible) triggerQrRefresh = true;
            } else if (!codeReqOk) {
                ui.activationCode.clear();
                ui.activationExpiresIn = 0;
                ui.activationError = codeReqResultErr;
                if (codeReqNetworkFail) {
                    offlineWaitReconnect = true;
                    qrLostOnOffline = true;
                    ui.loginQrVisible = false;
                    ui.loginQr = Bitmap();
                    ui.loginQrVersion.clear();
                    ui.loginQrLabel.clear();
                }
            }
            ui.dirty = true;
        }
        if (codePollDone) {
            codePollDone = false;
            if (codePollResult.status == "authorized" && !codePollResult.session.token.empty()) {
                activeToken = codePollResult.session.token;
                ui.loggedUsername = codePollResult.session.username;
                ui.loggedDisplayName = codePollResult.session.displayName;
                ui.activationCode.clear();
                ui.activationExpiresIn = 0;
                ui.activationError.clear();
                ui.loginScreen = false;
                autoCodeRequested = false;
                offlineWaitReconnect = false;
                qrLostOnOffline = false;
                codePollFrameAccum = 0;
                std::string greet = ui.loggedDisplayName.empty() ? ui.loggedUsername : ui.loggedDisplayName;
                ui.notice = "Console autorizado! Bem-vindo, " + greet + "!";
                triggerPostLoginSync = true;
                ui.dirty = true;
            } else if (codePollResult.status == "rejected" || codePollResult.status == "expired") {
                ui.activationCode.clear();
                ui.activationExpiresIn = 0;
                ui.activationError = !codePollResult.error.empty()
                    ? codePollResult.error
                    : (codePollResult.status == "rejected"
                           ? "Código recusado no painel. Pressione X para gerar outro."
                           : "Código expirado. Pressione X para gerar um novo.");
                if (qrLostOnOffline && !ui.syncing) triggerQrRefresh = true;
                ui.dirty = true;
            } else if (codePollResult.status == "network_error") {
                qrLostOnOffline = true;
                if (ui.loginQrVisible || !ui.loginQr.p.empty()) {
                    ui.loginQrVisible = false;
                    ui.loginQr = Bitmap();
                    ui.loginQrVersion.clear();
                    ui.loginQrLabel.clear();
                    ui.dirty = true;
                }
                if (ui.loginScreen && ui.activationError != codePollResult.error) {
                    ui.activationError = !codePollResult.error.empty()
                        ? codePollResult.error
                        : "Sem conexão com o servidor para verificar o código.";
                    ui.dirty = true;
                }
            } else if (codePollResult.status == "pending") {
                if (codePollResult.expiresIn > 0) {
                    int prevMins = std::max(1, (ui.activationExpiresIn + 59) / 60);
                    int nextMins = std::max(1, (codePollResult.expiresIn + 59) / 60);
                    ui.activationExpiresIn = codePollResult.expiresIn;
                    if (prevMins != nextMins) ui.dirty = true;
                }
                if (!ui.activationError.empty()) {
                    ui.activationError.clear();
                    ui.dirty = true;
                }
                if (qrLostOnOffline && !ui.syncing) triggerQrRefresh = true;
            }
        }
        pthread_mutex_unlock(&syncLock);
        if (triggerPostLoginSync) startSync(ui.serverUrl, activeToken);
        else if (triggerQrRefresh) startQrRefresh(ui.serverUrl, activeToken);

        // Na tela de autorização: gera o código ao abrir e consulta aprovação (~3s); após entrar, sincronização somente manual (OPTIONS / L3)
        if (ui.loginScreen) {
            if (offlineWaitReconnect) {
                codePollFrameAccum = 0;
            } else if (!ui.activationCode.empty()) {
                ++codePollFrameAccum;
                if (codePollFrameAccum >= 180) {
                    codePollFrameAccum = 0;
                    if (!ui.codeRequesting && !ui.syncing) {
                        startDeviceCodePoll(ui.serverUrl, ui.activationCode);
                    }
                }
            } else {
                codePollFrameAccum = 0;
                if (!ui.syncing && !autoCodeRequested && !ui.codeRequesting) {
                    autoCodeRequested = true;
                    ui.codeRequesting = true;
                    ui.activationError.clear();
                    startDeviceCodeRequest(ui.serverUrl);
                    ui.dirty = true;
                }
            }
        } else {
            autoCodeRequested = false;
            codePollFrameAccum = 0;
        }

        bool animSpin = ui.hasSpinnerActivity();
        bool animTick = animSpin && ((ui.tab == ui.downloadsTab() || ui.detail || ui.loginScreen) ? (frame & 1) == 0 : (frame % 3) == 0);
        if (ui.dirty || animTick) {
            ui.animFrame = frame;
            ui.draw();
        }
        display.flip(ui.c, frame);
        static bool splashHidden = false;
        if (!splashHidden) {
            splashHidden = true;
            int32_t splashRes = sceSystemServiceHideSplashScreen();
            installLog("Splash screen hidden result=0x%08X", (unsigned)splashRes);
        }
    }
}
