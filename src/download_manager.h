#pragma once
#include "download_model.h"
#include "torrent.h"
#include <pthread.h>
/* All network calls and torrent transfers happen off the UI thread. */
typedef bool (*DownloadFetch)(const std::string&,std::string&,size_t,const std::string&);
class DownloadManager {
    struct WorkerContext {DownloadManager*manager=0;int jobId=0,deleteJobId=0;uint64_t rateBytes=0;double rateTime=0,rateValue=0;};
    static const int MAX_PARALLEL_DOWNLOADS=2;
    std::string root;DownloadFetch fetch;pthread_mutex_t mutex;pthread_cond_t changed;pthread_t workers[MAX_PARALLEL_DOWNLOADS],sourceWorker,saveWorker;WorkerContext workerContexts[MAX_PARALLEL_DOWNLOADS];int workerCount=0;
    std::vector<DownloadRow>jobs;bool quitting=false,started=false,saveStarted=false,saveStopping=false,saveDirty=false;std::string pendingSave,loadError;uint64_t version=1;double lastCheckpoint=0;
    bool discovery=false,sourceReady=false;Game sourceGame;std::vector<TorrentLink>links;std::string sourceError;int nextId=1;
    static void* entry(void*);static void* sourceEntry(void*);static void* saveEntry(void*);void loop(WorkerContext*);void sourceLoop();void saveLoop();void saveLocked();DownloadRow* findLocked(int);
    static unsigned char* http(void*,const char*,size_t*);static int stopped(void*);static void progress(void*,const char*,uint64_t,uint64_t,unsigned);static void error(void*,const char*);
    static void traffic(void*,uint64_t);
public:
    DownloadManager(std::string,DownloadFetch);~DownloadManager();
    const std::string& startupError()const{return loadError;}
    bool start();void discover(const Game&);bool takeSources(std::vector<TorrentLink>&,std::string&,std::string&);
    bool enqueue(const Game&,const TorrentLink&,std::string&,const std::string& metadata="");void toggle(int);void setPaused(int,bool);void cancel(int);void remove(int,bool deleteFiles=false);bool snapshot(uint64_t&,std::vector<DownloadRow>&);
};
