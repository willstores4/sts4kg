#include "download_controller.h"
#include "catalog.h"
#ifndef PORTAL_HOST
#include "network_ps4.h"
#endif
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>

DownloadController::DownloadController(std::string r,DownloadFetch f):root(r),fetch(f),remote(r){pthread_mutex_init(&mutex,0);}
DownloadController::~DownloadController(){pthread_mutex_lock(&mutex);quitting=true;pthread_mutex_unlock(&mutex);if(started)pthread_join(monitor,0);local.reset();if(owner>=0){flock(owner,LOCK_UN);close(owner);}pthread_mutex_destroy(&mutex);}
bool DownloadController::start(){if(!remote.start())return false;started=pthread_create(&monitor,0,entry,this)==0;return started;}
void* DownloadController::entry(void*p){static_cast<DownloadController*>(p)->loop();return 0;}
bool DownloadController::ensureLocalLocked(bool report){if(local)return true;if(switching||remote.setupInProgress()){if(report)notice="Background service is starting. Retry when startup finishes.";return false;}remote.allowLaunch(false);int fd=open((root+"/queue-owner.lock").c_str(),O_RDWR|O_CREAT,0666);if(fd<0){if(report)notice="Could not open the download queue lock.";return false;}fchmod(fd,0666);if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);if(report)notice="Connecting to the current queue owner. Retry in a moment.";return false;}std::unique_ptr<DownloadManager> manager(new DownloadManager(root,fetch));if(!manager->start()){flock(fd,LOCK_UN);close(fd);localError=manager->startupError().empty()?"Could not start downloads in NULL_store.":manager->startupError();if(report)notice=localError;return false;}localError.clear();owner=fd;local=std::move(manager);localVersion=0;++version;return true;}

// Catalog pages are fetched in the title process. The privileged daemon's
// sceSsl context rejects public CA chains even when sceHttpsLoadCert succeeds.
void DownloadController::discover(const Game&g){pthread_mutex_lock(&mutex);sourceGame=g;sourcePending=true;sourceReady=false;sources.clear();sourceTitle=g.title;sourceError.clear();pthread_mutex_unlock(&mutex);}
bool DownloadController::takeSources(std::vector<TorrentLink>&v,std::string&t,std::string&e){pthread_mutex_lock(&mutex);bool ready=sourceReady;if(ready){v=sources;t=sourceTitle;e=sourceError;sourceReady=false;}pthread_mutex_unlock(&mutex);return ready;}

bool DownloadController::enqueue(const Game&g,const TorrentLink&l,std::string&why){
    pthread_mutex_lock(&mutex);
    if(local){bool ok=local->enqueue(g,l,why);pthread_mutex_unlock(&mutex);return ok;}
    if(remote.isConnected()){
        for(const auto&pending:pendingEnqueues)if(pending.link.url==l.url){why="Already preparing this torrent for the background service.";pthread_mutex_unlock(&mutex);return false;}
        pendingEnqueues.push_back({g,l});why="Fetching the torrent file in NULL_store for background download…";pthread_mutex_unlock(&mutex);return true;
    }
    bool ok=ensureLocalLocked(true)&&local->enqueue(g,l,why);pthread_mutex_unlock(&mutex);return ok;
}
void DownloadController::toggle(int id){pthread_mutex_lock(&mutex);if(local)local->toggle(id);else remote.toggle(id);pthread_mutex_unlock(&mutex);}
void DownloadController::cancel(int id){pthread_mutex_lock(&mutex);if(local)local->cancel(id);else remote.cancel(id);pthread_mutex_unlock(&mutex);}
void DownloadController::remove(int id,bool deleteFiles){pthread_mutex_lock(&mutex);if(local)local->remove(id,deleteFiles);else remote.remove(id,deleteFiles);pthread_mutex_unlock(&mutex);}
bool DownloadController::snapshot(uint64_t&stamp,std::vector<DownloadRow>&out){pthread_mutex_lock(&mutex);std::vector<DownloadRow>next;bool changed=local?local->snapshot(localVersion,next):remote.snapshot(remoteVersion,next);if(changed){visibleJobs.swap(next);++version;}bool result=changed||stamp!=version;if(result)out=visibleJobs;stamp=version;pthread_mutex_unlock(&mutex);return result;}
bool DownloadController::takeNotice(std::string&out){pthread_mutex_lock(&mutex);bool found=!notice.empty();if(found){out=notice;notice.clear();}pthread_mutex_unlock(&mutex);return found||remote.takeNotice(out);}
std::string DownloadController::connectionStatus(){pthread_mutex_lock(&mutex);auto status=switching?"Setting up optional background service; download progress is saved.":local?"In-app downloads | Keep NULL_store open. L3: optional background service.":!localError.empty()?localError:remote.connectionStatus();pthread_mutex_unlock(&mutex);return status;}
void DownloadController::setup(){pthread_mutex_lock(&mutex);setupPending=true;switching=true;notice="Starting background service setup…";pthread_mutex_unlock(&mutex);}
void DownloadController::stopService(){remote.stopService();}

void DownloadController::loop(){
    time_t start=time(0),setupAt=0;
    for(;;){
        pthread_mutex_lock(&mutex);
        if(quitting){pthread_mutex_unlock(&mutex);return;}
        bool doSetup=setupPending;setupPending=false;
        Game gameToRead;bool doDiscover=sourcePending;if(doDiscover){gameToRead=sourceGame;sourcePending=false;sourceBusy=true;}
        PendingEnqueue enqueueItem;bool doEnqueue=!doDiscover&&!pendingEnqueues.empty();if(doEnqueue){enqueueItem=pendingEnqueues.front();pendingEnqueues.erase(pendingEnqueues.begin());}
        std::unique_ptr<DownloadManager> saved;
        int savedOwner=-1;
        if(doSetup){saved=std::move(local);savedOwner=owner;owner=-1;}
        pthread_mutex_unlock(&mutex);

        if(doSetup){saved.reset();if(savedOwner>=0){flock(savedOwner,LOCK_UN);close(savedOwner);}remote.setup();setupAt=time(0);pthread_mutex_lock(&mutex);++version;pthread_mutex_unlock(&mutex);}

        if(doDiscover){
            const int source=sourceForURL(gameToRead.url);std::string html;
#ifndef PORTAL_HOST
            portalNetworkTrace("app game-page start game=%d source=%d",gameToRead.id,source);
#endif
            bool ok=sourceURL(gameToRead.url)&&fetch(gameToRead.url,html,4*1024*1024,sourceOrigin(source)+"/");
            auto found=ok?torrentLinks(html,source):std::vector<TorrentLink>{};
            std::string error=!ok?"Could not read the game page. Check internet and retry.":found.empty()?"No public torrent link found on this game page.":"";
#ifndef PORTAL_HOST
            portalNetworkTrace("app game-page result game=%d ok=%d bytes=%zu links=%zu result=%s",gameToRead.id,ok?1:0,html.size(),found.size(),error.empty()?"ok":error.c_str());
#endif
            pthread_mutex_lock(&mutex);if(sourceGame.id==gameToRead.id){sources=found;sourceError=error;sourceReady=true;}sourceBusy=false;pthread_mutex_unlock(&mutex);
        }else if(doEnqueue){
            std::string metadata,reason;
            bool fetched=fetch(enqueueItem.link.url,metadata,8*1024*1024,enqueueItem.game.url)&&!metadata.empty()&&metadata[0]=='d';
#ifndef PORTAL_HOST
            portalNetworkTrace("app torrent-file result game=%d ok=%d bytes=%zu",enqueueItem.game.id,fetched?1:0,metadata.size());
#endif
            bool queued=false;
            if(fetched){
                pthread_mutex_lock(&mutex);
                if(remote.isConnected())queued=remote.enqueue(enqueueItem.game,enqueueItem.link,metadata,reason);
                if(!queued&&!remote.isConnected()&&ensureLocalLocked(false))queued=local->enqueue(enqueueItem.game,enqueueItem.link,reason,metadata);
                if(queued)notice="Torrent file cached in the app; background download queued.";
                else notice=reason.empty()?"Could not hand the cached torrent to the download worker. Retry.":reason;
                pthread_mutex_unlock(&mutex);
            }else{
                pthread_mutex_lock(&mutex);notice="Could not fetch a valid torrent file in NULL_store. Check the network log and retry.";pthread_mutex_unlock(&mutex);
            }
        }

        if(!doSetup&&!doDiscover&&!doEnqueue){
            pthread_mutex_lock(&mutex);
            if(switching&&setupAt&&!remote.setupInProgress()&&(remote.isConnected()||time(0)-setupAt>=2)){switching=false;setupAt=0;++version;}
            if(!switching&&!local&&!remote.isConnected()&&!remote.setupInProgress()&&time(0)-start>=1)ensureLocalLocked(false);
            pthread_mutex_unlock(&mutex);
        }
        timespec wait={0,200000000};nanosleep(&wait,0);
    }
}
