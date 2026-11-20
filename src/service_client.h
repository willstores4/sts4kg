#pragma once
#include "service_protocol.h"
#include <pthread.h>
class ServiceClient {
    std::string root;pthread_mutex_t mutex;pthread_cond_t changed;pthread_t thread;bool started=false,quitting=false,connected=false,queueReady=false,autoStart=true;uint64_t version=1;std::vector<DownloadRow>jobs;std::vector<std::string>commands;
    Game pendingGame;int sourceRequestSentGameId=0;bool discovering=false,sourcesReady=false;std::vector<TorrentLink>links;std::string sourceError,notice,status,lastSetupError;
    bool setupRequested=false,stopRequested=false,setupBusy=false,launchBusy=false;
    static void* entry(void*);void loop();bool call(const PortalIPC::Writer&,std::string&,std::string&);void push(const PortalIPC::Writer&);
public:
    ServiceClient(std::string);~ServiceClient();bool start();void discover(const Game&);bool takeSources(std::vector<TorrentLink>&,std::string&,std::string&);bool enqueue(const Game&,const TorrentLink&,const std::string&metadata,std::string&why);void toggle(int);void cancel(int);void remove(int,bool deleteFiles=false);bool snapshot(uint64_t&,std::vector<DownloadRow>&);bool takeNotice(std::string&);std::string connectionStatus();void setup();void stopService();bool isConnected();void allowLaunch(bool);bool setupInProgress();
};
