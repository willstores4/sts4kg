#include "../src/installer.h"
#include "../src/queue_store.h"
#include <atomic>
#include <cassert>
#include <ctime>
#include <iostream>
#include <unistd.h>

static std::atomic<int> registrations(0), starts(0), progress(-1);
static std::atomic<bool> complete(false), queryFailure(false);

static bool regRemote(const InstallPackage&, int& id, std::string&) {
    ++registrations;
    id = 77;
    return true;
}
static bool startFn(int id, std::string&) { assert(id == 77); ++starts; return true; }
static bool pollFn(int id, uint64_t& done, uint64_t& total, int& pct, bool& isDone, std::string& err) {
    assert(id == 77);
    if (queryFailure) { err = "Temporary query failure"; return false; }
    done = progress > 0 ? (uint64_t)progress : 0;
    total = 100;
    pct = progress;
    isDone = complete;
    return true;
}
static void pauseMs() { timespec delay = {0, 10000000}; nanosleep(&delay, nullptr); }

static void waitFor(PackageInstaller& installer, InstallState state, int pct) {
    uint64_t v = 0;
    std::vector<InstallPackage> view;
    for (int attempt = 0; attempt < 400; ++attempt) {
        installer.snapshot(v, view);
        if (!view.empty() && view[0].state == state && view[0].percent == pct) return;
        pauseMs();
    }
    assert(false && "Installer failed to reach expected state");
}

int main() {
    char folder[] = "/tmp/hbstore-progress-XXXXXX";
    assert(mkdtemp(folder));
    std::string root = folder;

    InstallBackend api;
    api.registerRemoteTask = regRemote;
    api.startTask = startFn;
    api.pollTask = pollFn;

    {
        PackageInstaller installer(root, api);
        assert(installer.start());
        Game g;
        g.id = 1;
        g.title = "Sample HB";
        g.url = "http://192.168.1.2:8765/pkgs/sample.pkg";
        g.contentId = "IV0000-TEST00001_00-TESTPACKAGE00001";
        g.sizeBytes = 4096;
        std::string msg;
        installer.enqueueHomebrew(g, InstallMode::RemoteBgft, msg);
        waitFor(installer, InstallState::Installing, -1);
        progress = 35;
        waitFor(installer, InstallState::Installing, 35);
        queryFailure = true;
        waitFor(installer, InstallState::Unavailable, 35);
        queryFailure = false;
        complete = true;
        waitFor(installer, InstallState::Complete, 100);
    }

    unlink((root + "/tasks.tsv").c_str());
    unlink((root + "/tasks.tsv.bak").c_str());
    rmdir((root + "/downloads").c_str());
    rmdir(root.c_str());
    std::cout << "BGFT notification progress transitions passed.\n";
    return 0;
}
