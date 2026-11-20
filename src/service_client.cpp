#include "service_client.h"
#include "catalog.h"
#include <unistd.h>
#include <ctime>
#ifndef PORTAL_HOST
#include "service_start_ps4.h"
#include "network_ps4.h"
#endif
using namespace PortalIPC;
ServiceClient::ServiceClient(std::string p):root(p){pthread_mutex_init(&mutex,0);pthread_cond_init(&changed,0);autoStart=readFile(root+"/service-stopped.txt").empty();status="Connecting to background service...";}
ServiceClient::~ServiceClient(){pthread_mutex_lock(&mutex);quitting=true;pthread_cond_signal(&changed);pthread_mutex_unlock(&mutex);if(started)pthread_join(thread,0);pthread_cond_destroy(&changed);pthread_mutex_destroy(&mutex);}
bool ServiceClient::start(){if(started)return true;started=pthread_create(&thread,0,entry,this)==0;return started;}
void* ServiceClient::entry(void*p){static_cast<ServiceClient*>(p)->loop();return 0;}
void ServiceClient::push(const Writer&w){commands.push_back(w.bytes);pthread_cond_signal(&changed);}
void ServiceClient::discover(const Game&g){pthread_mutex_lock(&mutex);pendingGame=g;sourceRequestSentGameId=0;links.clear();sourceError.clear();sourcesReady=false;discovering=true;Writer w;w.u32(DISCOVER);game(w,g);push(w);pthread_mutex_unlock(&mutex);}
bool ServiceClient::takeSources(std::vector<TorrentLink>&out,std::string&title,std::string&error){pthread_mutex_lock(&mutex);bool ready=sourcesReady;if(ready){out=links;title=pendingGame.title;error=sourceError;sourcesReady=false;}pthread_mutex_unlock(&mutex);return ready;}
bool ServiceClient::enqueue(const Game&g,const TorrentLink&link,const std::string&metadata,std::string&why){pthread_mutex_lock(&mutex);bool ok=connected&&queueReady;if(ok){Writer w;w.u32(ENQUEUE);game(w,g);w.string(link.title);w.string(link.url);w.string(metadata);push(w);why="Sending cached torrent to background service...";}else why="Service is not connected. Press L3 in Downloads to start it.";pthread_mutex_unlock(&mutex);return ok;}
void ServiceClient::toggle(int id){pthread_mutex_lock(&mutex);if(connected){for(auto&j:jobs)if(j.id==id){Writer w;w.u32(j.state==DownloadState::Active||j.state==DownloadState::Queued?PAUSE:RESUME);w.u32(id);push(w);break;}}if(!connected)notice="Service is not connected. Press L3 in Downloads.";pthread_mutex_unlock(&mutex);}
void ServiceClient::remove(int id,bool deleteFiles){pthread_mutex_lock(&mutex);Writer w;w.u32(deleteFiles?REMOVE_DELETE:REMOVE);w.u32(id);push(w);pthread_mutex_unlock(&mutex);}
void ServiceClient::cancel(int id){pthread_mutex_lock(&mutex);if(connected){Writer w;w.u32(CANCEL);w.u32(id);push(w);}else notice="Service is not connected. Press L3 in Downloads.";pthread_mutex_unlock(&mutex);}
bool ServiceClient::snapshot(uint64_t&stamp,std::vector<DownloadRow>&out){pthread_mutex_lock(&mutex);bool changed=queueReady&&stamp!=version;if(changed){stamp=version;out=jobs;}pthread_mutex_unlock(&mutex);return changed;}
bool ServiceClient::takeNotice(std::string&out){pthread_mutex_lock(&mutex);bool ready=!notice.empty();if(ready){out=notice;notice.clear();}pthread_mutex_unlock(&mutex);return ready;}
std::string ServiceClient::connectionStatus(){pthread_mutex_lock(&mutex);auto s=status;pthread_mutex_unlock(&mutex);return s;}
void ServiceClient::setup(){pthread_mutex_lock(&mutex);setupRequested=true;setupBusy=true;autoStart=true;unlink((root+"/service-stopped.txt").c_str());pthread_cond_signal(&changed);pthread_mutex_unlock(&mutex);}
void ServiceClient::stopService(){pthread_mutex_lock(&mutex);stopRequested=true;autoStart=false;writeFile(root+"/service-stopped.txt","Stopped by user\n");pthread_cond_signal(&changed);pthread_mutex_unlock(&mutex);}
bool ServiceClient::call(const Writer&w,std::string&reply,std::string&error){return request(w.bytes,reply,error);}
void ServiceClient::loop(){
#ifndef PORTAL_HOST
    pthread_mutex_lock(&mutex);launchBusy=true;status="Checking installed background service...";pthread_mutex_unlock(&mutex);
    portalNetworkInit();
#endif
    time_t lastLaunch=0;for(;;){pthread_mutex_lock(&mutex);if(quitting){pthread_mutex_unlock(&mutex);return;}bool setup=setupRequested,stop=stopRequested,launch=autoStart;setupRequested=stopRequested=false;std::vector<std::string>pending;pending.swap(commands);pthread_mutex_unlock(&mutex);std::string error,reply;
#ifndef PORTAL_HOST
    if(setup){pthread_mutex_lock(&mutex);status="Setting up background service...";lastSetupError.clear();pthread_mutex_unlock(&mutex);bool ok=portalServiceInstallAndLaunch(error);pthread_mutex_lock(&mutex);notice=ok?"Background service started. Downloads can continue after closing NULL_store.":error;lastSetupError=ok?std::string():error;pthread_mutex_unlock(&mutex);lastLaunch=time(0);pthread_mutex_lock(&mutex);setupBusy=false;pthread_mutex_unlock(&mutex);}
#endif
#ifdef PORTAL_HOST
    if(setup){pthread_mutex_lock(&mutex);setupBusy=false;notice="Native service installation is unavailable in the host test.";pthread_mutex_unlock(&mutex);}
#endif
    if(stop){Writer w;w.u32(STOP);bool stopped=call(w,reply,error);pthread_mutex_lock(&mutex);notice=stopped?"Stop requested. Saved downloads resume when you start the service again.":"Could not confirm service shutdown: "+error;pthread_mutex_unlock(&mutex);}
    Writer ping;ping.u32(PING);bool ok=call(ping,reply,error);bool compatible=false;std::string runningBuild;if(ok){try{Reader r(reply);compatible=r.u32()==VERSION;runningBuild=r.string();r.u32();r.end();if(!compatible)error="Background service version is incompatible. Stop it before updating.";}catch(const std::exception&e){ok=false;error=e.what();}}
#ifndef PORTAL_HOST
    if(!ok&&launch&&!stop&&!setup&&time(0)-lastLaunch>=5){std::string why;pthread_mutex_lock(&mutex);launchBusy=true;status="Starting installed background service...";pthread_mutex_unlock(&mutex);portalServiceLaunch(why);pthread_mutex_lock(&mutex);launchBusy=false;pthread_mutex_unlock(&mutex);lastLaunch=time(0);if(!why.empty())error=why;}
#endif
    pthread_mutex_lock(&mutex);launchBusy=false;connected=ok&&compatible;if(!connected)queueReady=false;status=connected?"Background service connected | "+runningBuild+(runningBuild=="1.0"?" | continues after closing":" | L3: update service to 1.0"):!lastSetupError.empty()?"Service setup failed: "+lastSetupError+" | L3 to retry":!launch?"Background service stopped | L3 to start":error+" | L3 Service";pthread_mutex_unlock(&mutex);
    if(ok&&compatible){
        for(auto&packet:pending){Writer w;w.bytes=packet;std::string why;Reader command(packet);auto op=command.u32();Game dispatchedGame;bool dispatchedDiscovery=op==DISCOVER;if(dispatchedDiscovery)dispatchedGame=game(command);bool success=call(w,reply,why);pthread_mutex_lock(&mutex);if(success&&dispatchedDiscovery&&pendingGame.id==dispatchedGame.id)sourceRequestSentGameId=dispatchedGame.id;if(!success){notice=why;if(op==DISCOVER){sourceError=why;sourcesReady=true;discovering=false;}}else if(op==ENQUEUE){try{Reader r(reply);notice=r.string();}catch(...){notice="Refresh Downloads to check whether the request was accepted.";}}pthread_mutex_unlock(&mutex);}
        Writer queue;queue.u32(QUEUE);if(call(queue,reply,error)){try{Reader r(reply);auto count=r.u32();if(count>128)throw std::runtime_error("Service queue is too large");std::vector<DownloadRow>next;for(unsigned i=0;i<count;i++)next.push_back(row(r));r.end();pthread_mutex_lock(&mutex);jobs=next;queueReady=true;++version;pthread_mutex_unlock(&mutex);}catch(const std::exception&e){pthread_mutex_lock(&mutex);notice=e.what();pthread_mutex_unlock(&mutex);}}
        pthread_mutex_lock(&mutex);bool lookup=discovering&&sourceRequestSentGameId==pendingGame.id;int gameId=pendingGame.id;pthread_mutex_unlock(&mutex);if(lookup){Writer w;w.u32(SOURCES);w.u32(gameId);if(call(w,reply,error)){try{Reader r(reply);if(r.u32()){auto why=r.string();unsigned count=r.u32();if(count>128)throw std::runtime_error("Too many torrent links");std::vector<TorrentLink>found;for(unsigned i=0;i<count;i++){TorrentLink l;l.title=r.string();l.url=r.string();found.push_back(l);}r.end();pthread_mutex_lock(&mutex);if(pendingGame.id==gameId){links=found;sourceError=why;sourcesReady=true;discovering=false;}pthread_mutex_unlock(&mutex);}}catch(const std::exception&e){pthread_mutex_lock(&mutex);sourceError=e.what();sourcesReady=true;discovering=false;pthread_mutex_unlock(&mutex);}}else{pthread_mutex_lock(&mutex);sourceError=error;sourcesReady=true;discovering=false;pthread_mutex_unlock(&mutex);}}
    }else if(!pending.empty()){pthread_mutex_lock(&mutex);notice=status;if(discovering){sourceError=status;sourcesReady=true;discovering=false;}pthread_mutex_unlock(&mutex);}
    pthread_mutex_lock(&mutex);if(!quitting&&commands.empty()&&!setupRequested&&!stopRequested){timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=500000000;if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}pthread_cond_timedwait(&changed,&mutex,&until);}pthread_mutex_unlock(&mutex);
}}

bool ServiceClient::isConnected(){pthread_mutex_lock(&mutex);bool value=connected&&queueReady;pthread_mutex_unlock(&mutex);return value;}
void ServiceClient::allowLaunch(bool value){pthread_mutex_lock(&mutex);autoStart=value;pthread_mutex_unlock(&mutex);}
bool ServiceClient::setupInProgress(){pthread_mutex_lock(&mutex);bool value=setupBusy||setupRequested||launchBusy;pthread_mutex_unlock(&mutex);return value;}
