#include "service_server.h"
#include "net_compat.h"
#include "service_protocol.h"
#include "service_log.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <memory>

static volatile sig_atomic_t serviceQuit=0;
static void quitService(int sig){SERVICE_LOG("shutdown signal received: %d",sig);serviceQuit=1;}

int runPortalService(const std::string&root,DownloadFetch fetch,int port){
    using namespace PortalIPC;
    errno=0;int made=mkdir(root.c_str(),0777);int mkdirError=errno;
    SERVICE_LOG("server: mkdir root=%s => %d errno=%d",root.c_str(),made,mkdirError);

    errno=0;int ownership=open((root+"/queue-owner.lock").c_str(),O_RDWR|O_CREAT,0666);int openError=errno;
    SERVICE_LOG("server: open queue lock => fd=%d errno=%d",ownership,openError);
    if(ownership<0)return 2;
    errno=0;int shareResult=fchmod(ownership,0666);int shareError=errno;
    SERVICE_LOG("server: share queue lock => %d errno=%d",shareResult,shareError);
    if(shareResult){close(ownership);return 2;}
    errno=0;int lockResult=flock(ownership,LOCK_EX|LOCK_NB);int lockError=errno;
    SERVICE_LOG("server: acquire queue lock => %d errno=%d",lockResult,lockError);
    if(lockResult){close(ownership);return 2;}

    errno=0;int server=socket(AF_INET,SOCK_STREAM,0);int socketError=errno;
    SERVICE_LOG("server: socket(AF_INET,SOCK_STREAM) => fd=%d errno=%d",server,socketError);
    if(server<0){flock(ownership,LOCK_UN);close(ownership);return 3;}
    int yes=1;errno=0;int reuse=setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);int reuseError=errno;
    SERVICE_LOG("server: SO_REUSEADDR => %d errno=%d",reuse,reuseError);
    sockaddr_in address={};portalAddress4(&address,htonl(INADDR_LOOPBACK),port);
    errno=0;int bindResult=bind(server,(sockaddr*)&address,sizeof address);int bindError=errno;
    SERVICE_LOG("server: bind 127.0.0.1:%d => %d errno=%d",port,bindResult,bindError);
    if(bindResult){close(server);flock(ownership,LOCK_UN);close(ownership);return 3;}
    errno=0;int listenResult=listen(server,8);int listenError=errno;
    SERVICE_LOG("server: listen backlog=8 => %d errno=%d",listenResult,listenError);
    if(listenResult){close(server);flock(ownership,LOCK_UN);close(ownership);return 3;}

    signal(SIGTERM,quitService);signal(SIGINT,quitService);signal(SIGPIPE,SIG_IGN);serviceQuit=0;
    int result=0;{
        SERVICE_LOG("server: constructing download manager");
        DownloadManager manager(root,fetch);
        SERVICE_LOG("server: download manager constructed");
        errno=0;bool started=manager.start();int startError=errno;
        SERVICE_LOG("server: download manager start => %d errno=%d",started?1:0,startError);
        if(!started){SERVICE_LOG("server: saved queue unavailable: %s",manager.startupError().c_str());close(server);flock(ownership,LOCK_UN);close(ownership);return 4;}
        std::vector<TorrentLink>sourceLinks;std::string sourceError,sourceTitle;bool sourceReady=false;Game sourceGame;
#ifdef PORTAL_HOST
        printf("NULL_store service 1.0 listening on 127.0.0.1:%d (pid %d)\n",port,getpid());fflush(stdout);
#else
        SERVICE_LOG("server: listening on 127.0.0.1:%d pid=%d",port,getpid());
#endif
        time_t lastHeartbeat=time(0);
        while(!serviceQuit){
            fd_set set;FD_ZERO(&set);FD_SET(server,&set);timeval wait={0,250000};
            errno=0;int available=select(server+1,&set,0,0,&wait);int selectError=errno;
            if(available<0){if(selectError==EINTR)continue;SERVICE_LOG("server: select failed errno=%d",selectError);continue;}
            if(available==0){time_t now=time(0);if(now-lastHeartbeat>=30){SERVICE_LOG("server: heartbeat pid=%d interval=%llds",getpid(),(long long)(now-lastHeartbeat));lastHeartbeat=now;}continue;}
            errno=0;int fd=accept(server,0,0);int acceptError=errno;
            if(fd<0){if(acceptError!=EINTR)SERVICE_LOG("server: accept failed errno=%d",acceptError);continue;}
            timeval timeout={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
            std::string packet;Writer reply;
            if(receiveFrame(fd,packet)){
                try{
                    Reader r(packet);unsigned cmd=r.u32();Writer data;
                    switch(cmd){
                        case PING:r.end();data.u32(VERSION);data.string("1.0");data.u32(getpid());break;
                        case QUEUE:{r.end();uint64_t stamp=0;std::vector<DownloadRow>jobs;manager.snapshot(stamp,jobs);data.u32(jobs.size());for(auto&j:jobs)row(data,j);break;}
                        case DISCOVER:{sourceGame=game(r);r.end();sourceLinks.clear();sourceError.clear();sourceReady=false;SERVICE_LOG("server: game-page lookup started game=%d source=%d",sourceGame.id,sourceForURL(sourceGame.url));manager.discover(sourceGame);break;}
                        case SOURCES:{int id=r.u32();r.end();if(id!=sourceGame.id){SERVICE_LOG("server: ignored stale SOURCES request game=%d current=%d",id,sourceGame.id);data.u32(0);break;}if(!sourceReady)sourceReady=manager.takeSources(sourceLinks,sourceTitle,sourceError);data.u32(sourceReady?1:0);if(sourceReady){SERVICE_LOG("server: game-page lookup finished game=%d links=%zu result=%s",id,sourceLinks.size(),sourceError.empty()?"ok":sourceError.c_str());data.string(sourceError);data.u32(sourceLinks.size());for(auto&link:sourceLinks){data.string(link.title);data.string(link.url);}}break;}
                        case ENQUEUE:{Game g=game(r);TorrentLink link;link.title=r.string();link.url=r.string();auto metadata=r.string();r.end();SERVICE_LOG("server: enqueue game=%d cached_torrent_bytes=%zu",g.id,metadata.size());std::string why;if(!manager.enqueue(g,link,why,metadata))throw std::runtime_error(why);data.string(why);break;}
                        case PAUSE:case RESUME:case CANCEL:case REMOVE:case REMOVE_DELETE:{int id=r.u32();r.end();uint64_t stamp=0;std::vector<DownloadRow>jobs;manager.snapshot(stamp,jobs);bool exists=false;for(auto&j:jobs)if(j.id==id)exists=true;if(!exists)throw std::runtime_error("Download no longer exists");if(cmd==REMOVE||cmd==REMOVE_DELETE)manager.remove(id,cmd==REMOVE_DELETE);else if(cmd==CANCEL)manager.cancel(id);else manager.setPaused(id,cmd==PAUSE);break;}
                        case STOP:r.end();SERVICE_LOG("server: STOP command received");serviceQuit=1;break;
                        default:throw std::runtime_error("Unsupported service command");
                    }
                    reply.u32(0);reply.bytes+=data.bytes;
                }catch(const std::exception&e){SERVICE_LOG("server: rejected IPC request: %s",e.what());reply.bytes.clear();reply.u32(1);reply.string(e.what());}
                sendFrame(fd,reply.bytes);
            }
            close(fd);
        }
        SERVICE_LOG("server: stopping download manager");
        close(server);
    }
    flock(ownership,LOCK_UN);close(ownership);
    SERVICE_LOG("server: queue lock released; return=%d",result);
    return result;
}
