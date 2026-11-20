#define PORTAL_HOST
#include "../src/binloader_elf.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <pthread.h>
#include <string>

static std::string makeSyntheticTemplateElf() {
    std::string elf(256, '\0');
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; // 64-bit
    elf[5] = 1; // little-endian
    elf[16] = 2; // ET_EXEC
    std::string cfg(WILL_BINLOADER_CFG_SIZE, '\0');
    std::memcpy(&cfg[0], WILL_BINLOADER_MAGIC, sizeof(WILL_BINLOADER_MAGIC));
    elf += cfg;
    elf.append(64, '\0');
    return elf;
}

struct ServerCtx {
    int listenFd = -1;
    uint16_t port = 0;
    std::string received;
    std::atomic<bool> ready{false};
};

static void* tcpServerThread(void* arg) {
    auto* ctx = static_cast<ServerCtx*>(arg);
    ctx->ready = true;
    int client = accept(ctx->listenFd, nullptr, nullptr);
    if (client >= 0) {
        char buf[4096];
        for (;;) {
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            ctx->received.append(buf, static_cast<size_t>(n));
        }
        close(client);
    }
    close(ctx->listenFd);
    return nullptr;
}

int main() {
    std::string tpl = makeSyntheticTemplateElf();
    InstallPackage pkg;
    pkg.name = "PS4-Xplorer";
    pkg.url = "http://192.168.1.8:8765/api/pkg/3.pkg";
    pkg.contentId = "UP9000-PSHD00868_00-PSHD123450000001";
    pkg.iconUrl = "http://192.168.1.8:8765/covers/3.jpg";
    pkg.contentType = 0x1a;
    pkg.patch = true;
    pkg.bytes = 656736256ULL;

    std::string elf, err;
    assert(buildBinLoaderPayloadElf(tpl, pkg, elf, err));
    assert(elf.size() == tpl.size());

    size_t pos = elf.find(std::string(WILL_BINLOADER_MAGIC, sizeof(WILL_BINLOADER_MAGIC)));
    assert(pos != std::string::npos);

    uint64_t decodedSize = 0;
    for (int i = 0; i < 8; ++i) {
        decodedSize |= uint64_t(static_cast<unsigned char>(elf[pos + 16 + i])) << (i * 8);
    }
    assert(decodedSize == pkg.bytes);
    assert(std::string(&elf[pos + 24]) == pkg.url);
    assert(std::string(&elf[pos + 24 + 1024]) == pkg.name);
    assert(std::string(&elf[pos + 24 + 1024 + 256]) == pkg.contentId);
    assert(std::string(&elf[pos + 24 + 1024 + 256 + 64]) == pkg.iconUrl);
    assert(std::string(&elf[pos + 24 + 1024 + 256 + 64 + 512]) == "PS4GP");

    // Rejeita URL vazia ou Content ID inválido
    InstallPackage bad = pkg;
    bad.url = "ftp://invalido/test.pkg";
    assert(!buildBinLoaderPayloadElf(tpl, bad, elf, err));
    bad = pkg;
    bad.contentId = "CURTO";
    assert(!buildBinLoaderPayloadElf(tpl, bad, elf, err));

    // Testa envio TCP real para 127.0.0.1 em porta loopback efêmera
    assert(buildBinLoaderPayloadElf(tpl, pkg, elf, err));
    ServerCtx ctx;
    ctx.listenFd = socket(AF_INET, SOCK_STREAM, 0);
    assert(ctx.listenFd >= 0);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(bind(ctx.listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(ctx.listenFd, reinterpret_cast<struct sockaddr*>(&addr), &alen) == 0);
    ctx.port = ntohs(addr.sin_port);
    assert(listen(ctx.listenFd, 1) == 0);

    pthread_t th;
    assert(pthread_create(&th, nullptr, tcpServerThread, &ctx) == 0);
    assert(sendBinLoaderTcpPosix("127.0.0.1", ctx.port, elf, err));
    pthread_join(th, nullptr);
    assert(ctx.received == elf);

    std::cout << "BinLoader ELF template patching and 127.0.0.1 TCP delivery passed.\n";
    return 0;
}
