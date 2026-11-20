#pragma once
#include "service_client.h"
#include "download_manager.h"
#include <memory>
class DownloadController {
    struct PendingEnqueue {Game game;TorrentLink link;};
    std::string root;DownloadFetch fetch;ServiceClient remote;std::unique_ptr<DownloadManager>local;pthread_mutex_t mutex;pthread_t monitor;bool started=false,quitting=false,setupPending=false,switching=false,sourcePending=false,sourceReady=false,sourceBusy=false;int owner=-1;uint64_t version=1,localVersion=0,remoteVersion=0;Game sourceGame;std::vector<TorrentLink>sources;std::string sourceTitle,sourceError,notice;std::vector<PendingEnqueue>pendingEnqueues;
    static void* entry(void*);void loop();bool ensureLocalLocked(bool report);
    std::vector<DownloadRow>visibleJobs;std::string localError;
public:
    DownloadController(std::string,DownloadFetch);~DownloadController();bool start();void discover(const Game&);bool takeSources(std::vector<TorrentLink>&,std::string&,std::string&);bool enqueue(const Game&,const TorrentLink&,std::string&);void toggle(int);void cancel(int);void remove(int,bool deleteFiles=false);bool snapshot(uint64_t&,std::vector<DownloadRow>&);bool takeNotice(std::string&);std::string connectionStatus();void setup();void stopService();
};
