#include "service_log.h"
#ifndef PORTAL_HOST
#include "data_root.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

static int serviceLogFd=-1;

static void writeAll(int fd,const char *data,size_t size){
    size_t sent=0;
    while(sent<size){ssize_t n=write(fd,data+sent,size-sent);if(n<0&&errno==EINTR)continue;if(n<=0)break;sent+=(size_t)n;}
}

void serviceLogReopen(){
    if(serviceLogFd>=0)return;
    int firstError=0;
    const char*root=nullStoreDataRoot();char path[128];snprintf(path,sizeof path,"%s/service.log",root);
    serviceLogFd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0644);
    if(serviceLogFd<0){firstError=errno;snprintf(path,sizeof path,"/user%s/service.log",root);serviceLogFd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0644);}
    if(serviceLogFd<0)return;
    int chmodResult=fchmod(serviceLogFd,0644),chmodError=errno;
    char line[256];int n=snprintf(line,sizeof line,"[SERVICE 1.0][pid %d uid %d][%lld] runtime log opened; primary_error=%d; fchmod=%d errno=%d\n",getpid(),getuid(),(long long)time(0),firstError,chmodResult,chmodError);
    if(n>0)writeAll(serviceLogFd,line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
    fsync(serviceLogFd);
}

void serviceLog(const char *format,...){
    int saved=errno;
    if(serviceLogFd<0)serviceLogReopen();
    if(serviceLogFd<0){errno=saved;return;}
    char line[1200];
    int prefix=snprintf(line,sizeof line,"[SERVICE 1.0][pid %d uid %d][%lld] ",getpid(),getuid(),(long long)time(0));
    if(prefix<0||(size_t)prefix>=sizeof line){errno=saved;return;}
    va_list args;va_start(args,format);int body=vsnprintf(line+prefix,sizeof line-(size_t)prefix,format,args);va_end(args);
    if(body<0){errno=saved;return;}
    size_t used=(size_t)prefix+(size_t)body;
    if(used>=sizeof line)used=sizeof line-1;
    if(!used||line[used-1]!='\n'){if(used==sizeof line-1)line[used-1]='\n';else line[used++]='\n';}
    writeAll(serviceLogFd,line,used);
    fsync(serviceLogFd);
    errno=saved;
}

static void fatalSignalLog(int sig){
    if(serviceLogFd<0)return;
    const char *line="[SERVICE 1.0] fatal signal: unknown\n";size_t size=sizeof("[SERVICE 1.0] fatal signal: unknown\n")-1;
    switch(sig){
        case SIGSEGV:line="[SERVICE 1.0] fatal signal: SIGSEGV\n";size=sizeof("[SERVICE 1.0] fatal signal: SIGSEGV\n")-1;break;
        case SIGBUS:line="[SERVICE 1.0] fatal signal: SIGBUS\n";size=sizeof("[SERVICE 1.0] fatal signal: SIGBUS\n")-1;break;
        case SIGILL:line="[SERVICE 1.0] fatal signal: SIGILL\n";size=sizeof("[SERVICE 1.0] fatal signal: SIGILL\n")-1;break;
        case SIGFPE:line="[SERVICE 1.0] fatal signal: SIGFPE\n";size=sizeof("[SERVICE 1.0] fatal signal: SIGFPE\n")-1;break;
        case SIGABRT:line="[SERVICE 1.0] fatal signal: SIGABRT\n";size=sizeof("[SERVICE 1.0] fatal signal: SIGABRT\n")-1;break;
    }
    writeAll(serviceLogFd,line,size);
}

void serviceLogInstallCrashHandlers(){
    struct sigaction action={};action.sa_handler=fatalSignalLog;action.sa_flags=SA_RESETHAND;sigemptyset(&action.sa_mask);
    const int signals[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT};int failures=0;
    for(size_t i=0;i<sizeof signals/sizeof signals[0];++i)if(sigaction(signals[i],&action,0)<0)++failures;
    serviceLog("fatal signal handlers installed; failures=%d",failures);
}
#endif
