#include "../src/installer.h"
#include <atomic>
#include <cassert>
#include <ctime>
#include <iostream>
#include <unistd.h>

static std::atomic<int> registrations(0), starts(0), resumes(0);
static std::atomic<bool> resumed(false);

static bool regRemote(const InstallPackage&, int& id, std::string&) { ++registrations; id = 55; return true; }
static bool startFn(int id, std::string& err) { assert(id == 55); ++starts; err = "Injected start failure"; return false; }
static bool resumeFn(int id, std::string&) { assert(id == 55); ++resumes; resumed = true; return true; }
static bool pollFn(int id, uint64_t&, uint64_t&, int& pct, bool& done, std::string&) {
    assert(id == 55);
    done = resumed;
    pct = resumed ? 100 : 0;
    return true;
}
static void pauseMs() { timespec t = {0, 10000000}; nanosleep(&t, nullptr); }

static bool waitFor(PackageInstaller& i, InstallState state) {
    uint64_t stamp = 0;
    std::vector<InstallPackage> view;
    for (int n = 0; n < 300; n++) {
        i.snapshot(stamp, view);
        if (!view.empty() && view[0].state == state) return true;
        pauseMs();
    }
    return false;
}

int main() {
    char folder[] = "/tmp/hbstore-retry-XXXXXX";
    assert(mkdtemp(folder));
    std::string root = folder;

    InstallBackend api;
    api.registerRemoteTask = regRemote;
    api.startTask = startFn;
    api.resumeTask = resumeFn;
    api.pollTask = pollFn;

    int localId = 0;
    {
        PackageInstaller i(root, api);
        assert(i.start());
        Game g;
        g.id = 1;
        g.title = "Retry HB";
        g.url = "http://192.168.1.2:8765/pkgs/retry.pkg";
        g.contentId = "IV0000-TEST00001_00-TESTPACKAGE00001";
        g.sizeBytes = 4096;
        std::string msg;
        localId = i.enqueueHomebrew(g, InstallMode::RemoteBgft, msg);
        assert(waitFor(i, InstallState::Error));
        assert(registrations == 1 && starts == 1);
    }
    {
        PackageInstaller i(root, api);
        assert(i.start());
        i.retryTask(localId);
        assert(waitFor(i, InstallState::Complete));
        assert(registrations == 1 && starts == 1 && resumes == 1);
    }

    unlink((root + "/tasks.tsv").c_str());
    unlink((root + "/tasks.tsv.bak").c_str());
    rmdir((root + "/downloads").c_str());
    rmdir(root.c_str());
    std::cout << "Failed-start retry after relaunch preserves task ID without duplicates.\n";
    return 0;
}
