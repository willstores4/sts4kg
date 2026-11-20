#pragma once
#include "bgft_abi.h"
#include "catalog.h"
#include "installer.h"
#include <cstdint>
#include <cstring>
#include <string>

#ifdef PORTAL_HOST
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

static const char WILL_BINLOADER_MAGIC[16] = "WILL_BGFT_CFG_1";
static const size_t WILL_BINLOADER_CFG_SIZE = 16 + 8 + 1024 + 256 + 64 + 512 + 16;

inline std::string loadBinLoaderTemplateElf(const std::string& customPath = "") {
    if (!customPath.empty()) {
        std::string s = readFile(customPath);
        if (!s.empty()) return s;
    }
    const char* candidates[] = {
        "/app0/assets/bgft_template.elf",
        "assets/bgft_template.elf",
        "../assets/bgft_template.elf"
    };
    for (const char* p : candidates) {
        std::string s = readFile(p);
        if (!s.empty()) return s;
    }
    return "";
}

inline std::string binLoaderPackageType(uint32_t contentType, bool patch) {
    if (contentType == 0x1e) return "PS4DP";
    if (contentType == 0x1b) return "PS4AC";
    if (contentType == 0x1c) return "PS4AL";
    if (patch && contentType == 0x1a) return "PS4GP";
    const char* t = NullBGFT::packageType(contentType);
    return t ? std::string(t) : std::string("PS4GD");
}

inline bool buildBinLoaderPayloadElf(const std::string& templateElf, const InstallPackage& pkg,
                                     std::string& outElf, std::string& error) {
    outElf.clear();
    if (templateElf.size() < 64 || templateElf.compare(0, 4, "\x7f" "ELF") != 0) {
        error = "Template ELF do BinLoader ausente ou inválido";
        return false;
    }
    if (!isHttpUrl(pkg.url) || pkg.url.size() >= 1024) {
        error = pkg.url.size() >= 1024 ? "URL muito longa para payload BinLoader" : "URL do PKG inválida";
        return false;
    }
    if (!validPkgContentId(pkg.contentId)) {
        error = "Content ID inválido para payload BinLoader";
        return false;
    }

    const std::string needle(WILL_BINLOADER_MAGIC, sizeof(WILL_BINLOADER_MAGIC));
    size_t pos = templateElf.find(needle);
    if (pos == std::string::npos || pos + WILL_BINLOADER_CFG_SIZE > templateElf.size()) {
        error = "Estrutura de configuração não encontrada no template ELF";
        return false;
    }

    outElf = templateElf;
    // Zera os campos após a assinatura magic (16 bytes)
    std::memset(&outElf[pos + 16], 0, WILL_BINLOADER_CFG_SIZE - 16);

    // Tamanho em little-endian (uint64_t)
    uint64_t sz = pkg.bytes;
    for (int i = 0; i < 8; ++i) {
        outElf[pos + 16 + i] = static_cast<char>((sz >> (i * 8)) & 0xffu);
    }

    auto copyField = [&](size_t offset, size_t cap, const std::string& val) {
        size_t n = std::min(val.size(), cap - 1);
        if (n > 0) std::memcpy(&outElf[pos + offset], val.data(), n);
    };

    std::string displayName = pkg.name.empty() ? pkg.contentId : pkg.name;
    std::string iconUrl = pkg.iconUrl.size() < 512 ? pkg.iconUrl : "";
    std::string pkgType = binLoaderPackageType(pkg.contentType, pkg.patch);

    copyField(24, 1024, pkg.url);
    copyField(24 + 1024, 256, displayName);
    copyField(24 + 1024 + 256, 64, pkg.contentId);
    copyField(24 + 1024 + 256 + 64, 512, iconUrl);
    copyField(24 + 1024 + 256 + 64 + 512, 16, pkgType);
    return true;
}

#ifdef PORTAL_HOST
inline bool sendBinLoaderTcpPosix(const std::string& hostIp, uint16_t port, const std::string& data, std::string& error) {
    if (data.empty()) {
        error = "Payload ELF vazio";
        return false;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "Falha ao abrir socket para BinLoader";
        return false;
    }
    struct timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, hostIp.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        error = "IP do BinLoader inválido";
        return false;
    }
    if (connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        error = "Não foi possível conectar ao BinLoader em " + hostIp + ":" + std::to_string(port) +
                " — ative o BinLoader no GoldHEN";
        return false;
    }
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            error = "Conexão interrompida ao enviar ELF para BinLoader";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}
#endif
