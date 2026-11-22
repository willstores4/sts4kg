#include "network_ps4.h"
#include "data_root.h"
#include "catalog.h"
#include <pthread.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "../build/trusted_roots.h"

extern "C" int recvTimeout(int, unsigned) __asm__("sceHttpSetRecvTimeOut");
extern "C" int sslTerminate(int) __asm__("sceSslTerm");
extern "C" int autoRedirect(int, int) __asm__("sceHttpSetAutoRedirect");
extern "C" int httpsError(int, int*, uint32_t*) __asm__("sceHttpsGetSslError");

static int pool = -1, ssl = -1, http = -1, caLoadResult = -1;
static pthread_mutex_t initMutex = PTHREAD_MUTEX_INITIALIZER;

void portalNetworkTrace(const char* format, ...) {
    (void)format;
}

bool portalNetworkInit() {
    int lock = pthread_mutex_lock(&initMutex);
    if (lock) {
        portalNetworkTrace("initialization mutex failed=%d", lock);
        return false;
    }
    if (http >= 0) {
        pthread_mutex_unlock(&initMutex);
        return true;
    }
    uint32_t netModule = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    uint32_t sslModule = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    uint32_t httpModule = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    int netResult = sceNetInit(), ctlResult = sceNetCtlInit();
    portalNetworkTrace("init modules net=0x%08X ssl=0x%08X http=0x%08X; netInit=0x%08X ctlInit=0x%08X",
                       netModule, sslModule, httpModule, (unsigned)netResult, (unsigned)ctlResult);
    errno = 0;
    pool = sceNetPoolCreate("HBStoreHTTP", 1024 * 1024, 0);
    int poolError = errno;
    if (pool >= 0) ssl = sceSslInit(2 * 1024 * 1024);
    if (ssl >= 0) http = sceHttpInit(pool, ssl, 2 * 1024 * 1024);
    bool ok = false;
    if (http >= 0) {
        caLoadResult = sceHttpsLoadCert(http, (int32_t)portalTrustCount, portalTrustRoots, 0, 0);
        portalNetworkTrace("HTTPS trust roots=%zu load=0x%08X sha256=%s", portalTrustCount, (unsigned)caLoadResult, portalTrustSHA256);
        ok = caLoadResult >= 0;
    }
    portalNetworkTrace("init complete=%d pool=%d pool_errno=%d ssl=%d http=%d", ok ? 1 : 0, pool, poolError, ssl, http);
    if (!ok) {
        if (http >= 0) sceHttpTerm(http);
        if (ssl >= 0) sslTerminate(ssl);
        if (pool >= 0) sceNetPoolDestroy(pool);
        http = ssl = pool = -1;
    }
    pthread_mutex_unlock(&initMutex);
    return ok;
}

std::string portalNetworkDeviceId() {
    static std::string cachedId;
    if (!cachedId.empty()) return cachedId;
    portalNetworkInit();
    unsigned char mac[6] = {};
    OrbisNetEtherAddr addr = {};
    if (sceNetGetMacAddress(&addr, 0) >= 0) {
        memcpy(mac, addr.data, 6);
    }
    bool nonZero = false;
    for (int i = 0; i < 6; ++i) {
        if (mac[i] != 0) nonZero = true;
    }
    if (!nonZero) {
        OrbisNetCtlInfo ctlInfo = {};
        if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_ETHER_ADDR, &ctlInfo) >= 0) {
            memcpy(mac, ctlInfo.ether_addr, 6);
            for (int i = 0; i < 6; ++i) {
                if (mac[i] != 0) nonZero = true;
            }
        }
    }
    if (!nonZero) return "";
    char buf[32];
    snprintf(buf, sizeof buf, "PS4-%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cachedId = buf;
    return cachedId;
}

static std::string networkHost(const std::string& url) {
    size_t p = url.compare(0, 8, "https://") == 0 ? 8 : url.compare(0, 7, "http://") == 0 ? 7 : 0;
    if (!p) return "invalid";
    auto end = url.find_first_of("/?#", p);
    return url.substr(p, end - p);
}

static std::string redirectURL(const std::string& base, std::string path) {
    while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
    if (path.compare(0, 2, "//") == 0) return base.substr(0, base.find(':')) + ":" + path;
    if (path.find("://") != std::string::npos) return path;
    auto scheme = base.find("://");
    auto end = base.find('/', scheme + 3);
    auto origin = base.substr(0, end);
    if (!path.empty() && path[0] == '/') return origin + path;
    return base.substr(0, base.find_last_of('/') + 1) + path;
}

static std::string findResponseHeader(const std::string& allHeaders, const std::string& headerName) {
    std::string normalized = lower(allHeaders);
    std::string needle = "\n" + lower(headerName) + ":";
    size_t p = normalized.find(needle);
    if (p == std::string::npos && normalized.compare(0, needle.size() - 1, needle.substr(1)) == 0) {
        p = 0;
    } else if (p != std::string::npos) {
        ++p;
    }
    if (p == std::string::npos) return "";
    p += needle.size() - 1;
    while (p < allHeaders.size() && (allHeaders[p] == ' ' || allHeaders[p] == '\t')) ++p;
    size_t end = allHeaders.find_first_of("\r\n", p);
    return allHeaders.substr(p, end - p);
}

static bool performHttp(bool isPost, const std::string& initial, const std::string& reqBody,
                        const std::string& contentType, const std::string& authToken, const std::string& deviceId,
                        std::string* outMemory, size_t cap, bool captureErrorBody,
                        const std::string& referer, const std::string& rangeHeader, bool capPartialOk,
                        const std::string& destFile, bool appendMode, uint64_t expectedBytes,
                        PortalDownloadProgressFn progressCb, PortalDownloadStopFn stopCb, void* user,
                        int* httpCodeOut, std::string& errorOut) {
    errorOut.clear();
    if (httpCodeOut) *httpCodeOut = 0;
    if (outMemory) outMemory->clear();
    if (!portalNetworkInit()) {
        errorOut = "Falha ao inicializar rede PS4";
        portalNetworkTrace("%s failed host=%s reason=network_init", isPost ? "POST" : "GET", networkHost(initial).c_str());
        return false;
    }
    std::string url = initial;
    for (int hop = 0; hop < 6; hop++) {
        if (!isHttpUrl(url)) {
            errorOut = "URL inválida";
            portalNetworkTrace("%s failed host=%s hop=%d reason=invalid_url", isPost ? "POST" : "GET", networkHost(url).c_str(), hop);
            return false;
        }
        int t = sceHttpCreateTemplate(http, "HBStore/1.0", ORBIS_HTTP_VERSION_1_1, 1);
        if (t < 0) {
            errorOut = "Erro interno HTTP (template)";
            portalNetworkTrace("%s failed host=%s hop=%d reason=create_template result=%d", isPost ? "POST" : "GET", networkHost(url).c_str(), hop, t);
            return false;
        }
        int co = -1, r = -1, code = 0, sendResult = -1, readFailure = 0;
        bool ok = false;
        std::string location, failure = "create_connection";
        autoRedirect(t, 0);
        sceHttpSetResolveTimeOut(t, 8000000);
        sceHttpSetConnectTimeOut(t, 8000000);
        sceHttpSetSendTimeOut(t, 8000000);
        recvTimeout(t, 15000000);

        co = sceHttpCreateConnectionWithURL(t, url.c_str(), 1);
        if (co >= 0) {
            failure = "create_request";
            r = sceHttpCreateRequestWithURL(co, isPost ? ORBIS_METHOD_POST : ORBIS_METHOD_GET,
                                            url.c_str(), isPost ? (uint64_t)reqBody.size() : 0);
            if (r >= 0) {
                failure = "send_request";
                sceHttpAddRequestHeader(r, "Accept-Encoding", "identity", 0);
                if (!referer.empty()) sceHttpAddRequestHeader(r, "Referer", referer.c_str(), 0);
                if (!authToken.empty()) sceHttpAddRequestHeader(r, "X-App-Token", authToken.c_str(), 0);
                if (!deviceId.empty()) sceHttpAddRequestHeader(r, "X-Device-Id", deviceId.c_str(), 0);
                if (!contentType.empty()) sceHttpAddRequestHeader(r, "Content-Type", contentType.c_str(), 0);
                if (!rangeHeader.empty()) sceHttpAddRequestHeader(r, "Range", rangeHeader.c_str(), 0);
                sendResult = sceHttpSendRequest(
                    r, (isPost && !reqBody.empty()) ? reqBody.data() : nullptr, isPost ? reqBody.size() : 0);
                if (sendResult < 0) {
                    int nativeErrno = 0, sslCode = 0;
                    uint32_t verify = 0;
                    int sslQuery = -1;
                    int errnoQuery = sceHttpGetLastErrno(r, &nativeErrno);
                    if (((uint32_t)sendResult & 0xffff0000u) == 0x80950000u) {
                        sslQuery = httpsError(r, &sslCode, &verify);
                    }
                    portalNetworkTrace("request failure host=%s send=0x%08X sslQuery=0x%08X ssl=0x%08X verify=0x%08X errnoQuery=0x%08X errno=%d",
                                       networkHost(url).c_str(), (unsigned)sendResult, (unsigned)sslQuery, (unsigned)sslCode, verify, (unsigned)errnoQuery, nativeErrno);
                    if (((uint32_t)sendResult & 0xffff0000u) == 0x80950000u) {
                        errorOut = "Falha na conexão segura (HTTPS) com " + networkHost(url) + " — ative o Proxy no painel";
                    } else {
                        errorOut = "Não foi possível conectar ao servidor (" + networkHost(url) + ")";
                    }
                } else {
                    failure = "get_status";
                    int statusResult = sceHttpGetStatusCode(r, &code);
                    if (statusResult >= 0 && httpCodeOut) *httpCodeOut = code;
                    char* headers = 0;
                    size_t headerSize = 0;
                    std::string allHeaders;
                    if (sceHttpGetAllResponseHeaders(r, &headers, &headerSize) >= 0 && headers && headerSize > 0) {
                        allHeaders.assign(headers, headerSize);
                    }
                    if (statusResult >= 0 && code >= 200 && code < 300 && (code != 206 || !rangeHeader.empty())) {
                        failure = "read_response";
                        uint64_t totalLen = expectedBytes;
                        std::string cl = findResponseHeader(allHeaders, "Content-Length");
                        if (!cl.empty()) totalLen = strtoull(cl.c_str(), nullptr, 10);

                        int fd = -1;
                        std::string tmpPath;
                        if (!destFile.empty()) {
                            if (appendMode) {
                                fd = open(destFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                            } else {
                                tmpPath = destFile + ".part";
                                fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                            }
                            if (fd < 0) {
                                failure = "open_dest";
                                errorOut = "Erro ao criar arquivo de destino";
                            }
                        }

                        if (destFile.empty() || fd >= 0) {
                            char buffer[65536];
                            uint64_t received = 0;
                            for (;;) {
                                if (stopCb && stopCb(user)) {
                                    failure = "cancelled";
                                    errorOut = "Download cancelado";
                                    break;
                                }
                                int n = sceHttpReadData(r, buffer, sizeof buffer);
                                if (n == 0) {
                                    ok = true;
                                    failure = "";
                                    break;
                                }
                                if (n < 0) {
                                    readFailure = n;
                                    errorOut = "Conexão interrompida durante download";
                                    break;
                                }
                                if (outMemory) {
                                    size_t take = (size_t)n;
                                    if (outMemory->size() + take > cap) {
                                        if (capPartialOk) {
                                            take = cap - outMemory->size();
                                            outMemory->append(buffer, take);
                                            ok = true;
                                            failure = "";
                                            break;
                                        }
                                        readFailure = -2;
                                        failure = "response_too_large";
                                        errorOut = "Resposta excedeu o limite de memória";
                                        break;
                                    }
                                    outMemory->append(buffer, take);
                                }
                                if (fd >= 0) {
                                    size_t off = 0;
                                    bool writeErr = false;
                                    while (off < (size_t)n) {
                                        ssize_t w = write(fd, buffer + off, (size_t)n - off);
                                        if (w < 0 && errno == EINTR) continue;
                                        if (w <= 0) { writeErr = true; break; }
                                        off += (size_t)w;
                                    }
                                    if (writeErr) {
                                        failure = "disk_write";
                                        errorOut = "Erro ao gravar PKG no armazenamento";
                                        break;
                                    }
                                }
                                received += (uint64_t)n;
                                if (progressCb) progressCb(user, received, totalLen);
                            }
                            if (fd >= 0) {
                                fsync(fd);
                                close(fd);
                                if (!appendMode) {
                                    if (ok) {
                                        if (rename(tmpPath.c_str(), destFile.c_str()) != 0) {
                                            ok = false;
                                            errorOut = "Erro ao finalizar arquivo PKG";
                                            unlink(tmpPath.c_str());
                                        }
                                    } else {
                                        unlink(tmpPath.c_str());
                                    }
                                }
                            }
                        }
                    } else if (statusResult >= 0 && (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)) {
                        failure = "redirect_without_location";
                        location = findResponseHeader(allHeaders, "location");
                        if (!location.empty()) failure = "redirect";
                    } else if (statusResult >= 0) {
                        failure = "http_status";
                        if (captureErrorBody && outMemory && code >= 400) {
                            char errBuf[1024];
                            size_t errCap = std::min<size_t>(cap, 16384);
                            while (outMemory->size() < errCap) {
                                int n = sceHttpReadData(r, errBuf, sizeof errBuf);
                                if (n <= 0) break;
                                size_t take = std::min<size_t>((size_t)n, errCap - outMemory->size());
                                outMemory->append(errBuf, take);
                            }
                        }
                        if (code == 401 || code == 403) {
                            errorOut = "Acesso negado ou login necessário (HTTP " + std::to_string(code) + ")";
                        } else if (code == 404 || code == 410) {
                            errorOut = "Arquivo PKG não encontrado no servidor";
                        } else if (code == 416) {
                            errorOut = "Servidor não suportou o trecho solicitado do arquivo";
                        } else if (code == 429) {
                            errorOut = "Servidor sobrecarregado com muitas requisições — tente novamente";
                        } else if (code >= 500) {
                            errorOut = "Servidor do arquivo indisponível no momento";
                        } else {
                            errorOut = "Servidor recusou a solicitação (HTTP " + std::to_string(code) + ")";
                        }
                    }
                }
            }
        }
        if (r >= 0) sceHttpDeleteRequest(r);
        if (co >= 0) sceHttpDeleteConnection(co);
        sceHttpDeleteTemplate(t);

        if (ok) {
            portalNetworkTrace("%s ok host=%s hop=%d status=%d", isPost ? "POST" : "GET", networkHost(url).c_str(), hop, code);
            return true;
        }
        if (outMemory && !(captureErrorBody && failure == "http_status")) outMemory->clear();
        if (!location.empty()) {
            portalNetworkTrace("%s redirect host=%s hop=%d status=%d", isPost ? "POST" : "GET", networkHost(url).c_str(), hop, code);
            url = redirectURL(url, location);
            continue;
        }
        portalNetworkTrace("%s failed host=%s hop=%d reason=%s status=%d send=%d read=%d",
                           isPost ? "POST" : "GET", networkHost(url).c_str(), hop, failure.c_str(), code, sendResult, readFailure);
        if (errorOut.empty()) {
            if (failure == "redirect_without_location") {
                errorOut = "Servidor enviou redirecionamento inválido";
            } else {
                errorOut = "Não foi possível conectar ao servidor (" + networkHost(url) + ")";
            }
        }
        return false;
    }
    errorOut = "Limite de redirecionamentos excedido";
    portalNetworkTrace("%s failed host=%s reason=redirect_limit", isPost ? "POST" : "GET", networkHost(url).c_str());
    return false;
}

bool portalNetworkFetch(const std::string& initial, std::string& out, size_t cap, const std::string& referer) {
    std::string err;
    return performHttp(false, initial, "", "", "", "", &out, cap, false, referer, "", false, "", false, 0, nullptr, nullptr, nullptr, nullptr, err);
}

bool portalNetworkFetchAuth(const std::string& url, const std::string& authToken, std::string& out, size_t cap,
                            int& httpCodeOut, std::string& error) {
    return performHttp(false, url, "", "", authToken, portalNetworkDeviceId(), &out, cap, true, "", "", false, "", false, 0, nullptr, nullptr, nullptr, &httpCodeOut, error);
}

bool portalNetworkPostJson(const std::string& url, const std::string& jsonBody, const std::string& authToken,
                           std::string& out, size_t cap, int& httpCodeOut, std::string& error) {
    return performHttp(true, url, jsonBody, "application/json; charset=utf-8", authToken, portalNetworkDeviceId(), &out, cap, true, "", "", false, "", false, 0, nullptr, nullptr, nullptr, &httpCodeOut, error);
}

bool portalNetworkFetchRange(const std::string& url, std::string& out, uint64_t offset, size_t length, std::string& error) {
    std::string range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
    return performHttp(false, url, "", "", "", "", &out, length, false, "", range, true, "", false, 0, nullptr, nullptr, nullptr, nullptr, error);
}

bool portalNetworkDownloadFile(const std::string& url, const std::string& destPath, uint64_t expectedBytes,
                               PortalDownloadProgressFn progressCb, PortalDownloadStopFn stopCb, void* user,
                               std::string& error) {
    return performHttp(false, url, "", "", "", "", nullptr, 0, false, "", "", false, destPath, false, expectedBytes, progressCb, stopCb, user, nullptr, error);
}

bool portalNetworkAppendFile(const std::string& url, const std::string& destPath, uint64_t expectedBytes,
                             PortalDownloadProgressFn progressCb, PortalDownloadStopFn stopCb, void* user,
                             std::string& error) {
    return performHttp(false, url, "", "", "", "", nullptr, 0, false, "", "", false, destPath, true, expectedBytes, progressCb, stopCb, user, nullptr, error);
}

extern "C" int nbNetSocket(const char*, int, int, int) __asm__("sceNetSocket");
extern "C" int nbNetConnect(int, const void*, uint32_t) __asm__("sceNetConnect");
extern "C" int nbNetSend(int, const void*, size_t, int) __asm__("sceNetSend");
extern "C" int nbNetClose(int) __asm__("sceNetSocketClose");

struct OrbisRawSockaddrIn {
    uint8_t len;
    uint8_t family;
    uint16_t port;
    uint32_t addr;
    unsigned char zero[8];
};
static_assert(sizeof(OrbisRawSockaddrIn) == 16, "Orbis IPv4 sockaddr_in ABI");

static bool parseIpv4NetworkOrder(const std::string& ip, uint32_t& outAddr) {
    unsigned parts[4] = {};
    int idx = 0;
    unsigned val = 0;
    bool hasDigit = false;
    for (size_t i = 0; i <= ip.size(); ++i) {
        if (i == ip.size() || ip[i] == '.') {
            if (!hasDigit || val > 255 || idx >= 4) return false;
            parts[idx++] = val;
            val = 0;
            hasDigit = false;
        } else if (ip[i] >= '0' && ip[i] <= '9') {
            hasDigit = true;
            val = val * 10 + unsigned(ip[i] - '0');
            if (val > 255) return false;
        } else {
            return false;
        }
    }
    if (idx != 4) return false;
    outAddr = (parts[0]) | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return true;
}

bool portalNetworkSendTcp(const std::string& hostIp, uint16_t port, const std::string& data, std::string& error) {
    error.clear();
    if (data.empty()) {
        error = "Payload ELF vazio";
        return false;
    }
    uint32_t netIp = 0;
    if (!parseIpv4NetworkOrder(hostIp, netIp)) {
        error = "IP do BinLoader inválido: " + hostIp;
        return false;
    }
    if (!portalNetworkInit()) {
        error = "Falha ao inicializar rede PS4";
        return false;
    }
    int sock = nbNetSocket("WILLBinLoader", 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (sock < 0) {
        portalNetworkTrace("BinLoader socket failed sock=0x%08X", (unsigned)sock);
        error = "Falha ao abrir conexão interna com o BinLoader";
        return false;
    }

    OrbisRawSockaddrIn addr = {};
    addr.len = sizeof(addr);
    addr.family = 2; // AF_INET
    addr.port = uint16_t(((port & 0xffu) << 8) | ((port >> 8) & 0xffu));
    addr.addr = netIp;

    int rc = nbNetConnect(sock, &addr, sizeof(addr));
    if (rc < 0) {
        nbNetClose(sock);
        error = "BinLoader inacessível em " + hostIp + ":" + std::to_string(port) + " — ative o BinLoader no GoldHEN";
        portalNetworkTrace("BinLoader connect failed %s:%u rc=0x%08X", hostIp.c_str(), (unsigned)port, (unsigned)rc);
        return false;
    }

    size_t sent = 0;
    while (sent < data.size()) {
        int n = nbNetSend(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            nbNetClose(sock);
            error = "Conexão interrompida ao enviar payload para o BinLoader";
            portalNetworkTrace("BinLoader send failed %s:%u n=%d sent=%zu/%zu",
                               hostIp.c_str(), (unsigned)port, n, sent, data.size());
            return false;
        }
        sent += size_t(n);
    }
    nbNetClose(sock);
    portalNetworkTrace("BinLoader sent %zu bytes to %s:%u", sent, hostIp.c_str(), (unsigned)port);
    return true;
}
