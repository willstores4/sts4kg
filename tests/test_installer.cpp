#include "../src/installer.h"
#include "../src/queue_store.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

static std::string root;
static std::atomic<int> remoteReg(0), storageReg(0), started(0), polled(0), paused(0), resumed(0), stopped(0), headerFetched(0), downloaded(0), notified(0);

static void pauseMs(int n) {
    struct timespec ts = {n / 1000, (n % 1000) * 1000000};
    nanosleep(&ts, nullptr);
}

static void put64(std::string& s, int pos, uint64_t n) {
    for (int i = 7; i >= 0; i--) { s[pos + i] = char(n & 255); n >>= 8; }
}
static void put32(std::string& s, int pos, uint32_t n) {
    for (int i = 3; i >= 0; i--) { s[pos + i] = char(n & 255); n >>= 8; }
}
static std::string makePkgBytes(uint64_t sizeVal = 4096) {
    std::string s(0x1000, 0);
    s[0] = 0x7f; s[1] = 'C'; s[2] = 'N'; s[3] = 'T';
    s[0x77] = 0x1a;
    s.replace(0x40, 36, "IV0000-TEST00001_00-TESTPACKAGE00001");
    put64(s, 0x430, sizeVal);
    return s;
}

static bool regRemote(const InstallPackage& p, int& id, std::string&) {
    assert(p.contentId == "IV0000-TEST00001_00-TESTPACKAGE00001");
    assert(p.bytes == 4096);
    id = 42;
    ++remoteReg;
    return true;
}
static bool regStorage(const InstallPackage& p, int& id, std::string&) {
    assert(p.contentId == "IV0000-TEST00001_00-TESTPACKAGE00001");
    id = 43;
    ++storageReg;
    return true;
}
static bool start(int, std::string&) { ++started; return true; }
static bool pauseFn(int, std::string&) { ++paused; return true; }
static bool resumeFn(int, std::string&) { ++resumed; return true; }
static bool stopFn(int, std::string&) { ++stopped; return true; }
static bool pollFn(int, uint64_t& done, uint64_t& total, int& percent, bool& complete, std::string&) {
    ++polled;
    done = 4096;
    total = 4096;
    percent = 100;
    complete = true;
    return true;
}
static bool fetchHeader(const std::string&, std::string& out, std::string&) {
    ++headerFetched;
    out = makePkgBytes(4096);
    return true;
}
static bool dlFile(const std::string&, const std::string& dest, uint64_t,
                   PortalDownloadProgressFn cb, PortalDownloadStopFn, void* user, std::string&) {
    ++downloaded;
    if (cb) cb(user, 2048, 4096);
    return durableReplace(dest, makePkgBytes(4096));
}
static void notify(const std::string&) { ++notified; }

int main() {
    char dir[] = "/tmp/hbstore-installer-XXXXXX";
    assert(mkdtemp(dir));
    root = dir;

    auto pkgPath = root + "/test.pkg";
    assert(durableReplace(pkgPath, makePkgBytes()));
    InstallPackage pkg;
    std::string error;
    assert(inspectPackage(pkgPath, pkg, error) && pkg.bytes == 4096 && pkg.titleId == "TEST00001");

    auto bad = makePkgBytes();
    put64(bad, 0x430, 9000);
    assert(durableReplace(root + "/bad.pkg", bad));
    assert(!inspectPackage(root + "/bad.pkg", pkg, error));

    assert(symlink(pkgPath.c_str(), (root + "/link.pkg").c_str()) == 0);
    assert(!inspectPackage(root + "/link.pkg", pkg, error));

    for (uint32_t type : {0x1au, 0x1bu, 0x1cu, 0x1eu}) {
        auto h = makePkgBytes();
        put32(h, 0x74, type);
        assert(durableReplace(root + "/cls.pkg", h));
        assert(inspectPackage(root + "/cls.pkg", pkg, error) && pkg.contentType == type && pkg.patch == (type == 0x1e));
    }
    unlink((root + "/cls.pkg").c_str());

    InstallBackend api;
    api.registerRemoteTask = regRemote;
    api.registerStorageTask = regStorage;
    api.startTask = start;
    api.pauseTask = pauseFn;
    api.resumeTask = resumeFn;
    api.stopTask = stopFn;
    api.pollTask = pollFn;
    api.fetchPkgHeader = fetchHeader;
    api.downloadPkgFile = dlFile;
    api.notifySystem = notify;

    {
        PackageInstaller installer(root, api);
        assert(installer.start());

        Game g;
        g.id = 1;
        g.title = "Homebrew A";
        g.url = "http://192.168.1.2:8765/pkgs/a.pkg";
        // Sem contentId nem sizeBytes: deve buscar header automaticamente!
        std::string msg;
        int id1 = installer.enqueueHomebrew(g, InstallMode::RemoteBgft, msg);
        assert(id1 > 0);
        // Duplicata enquanto ativo não cria nova tarefa
        int idDup = installer.enqueueHomebrew(g, InstallMode::RemoteBgft, msg);
        assert(idDup == id1);

        uint64_t stamp = 0;
        std::vector<InstallPackage> view;
        bool done = false;
        for (int i = 0; i < 300; i++) {
            installer.snapshot(stamp, view);
            if (!view.empty() && view[0].state == InstallState::Complete) { done = true; break; }
            pauseMs(10);
        }
        assert(done && headerFetched == 1 && remoteReg == 1 && started == 1 && notified == 1 && polled >= 1);

        // Testar LocalStorageEx
        Game g2 = g;
        g2.id = 2;
        g2.title = "Homebrew B";
        g2.url = "http://192.168.1.2:8765/pkgs/b.pkg";
        int id2 = installer.enqueueHomebrew(g2, InstallMode::LocalStorageEx, msg);
        assert(id2 > id1);
        done = false;
        for (int i = 0; i < 300; i++) {
            installer.snapshot(stamp, view);
            for (auto& item : view) if (item.id == id2 && item.state == InstallState::Complete) done = true;
            if (done) break;
            pauseMs(10);
        }
        assert(done && downloaded == 1 && storageReg == 1 && started == 2);
        installer.removeTask(id2);
        installer.removeTask(id1);
    }

    unlink(pkgPath.c_str());
    unlink((root + "/bad.pkg").c_str());
    unlink((root + "/link.pkg").c_str());
    unlink((root + "/tasks.tsv").c_str());
    unlink((root + "/tasks.tsv.bak").c_str());
    rmdir((root + "/downloads").c_str());
    rmdir(root.c_str());
    std::cout << "PKG validation, remote BGFT notifications, header auto-inspect and local StorageEx passed.\n";
    return 0;
}
