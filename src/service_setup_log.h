#pragma once
#include "data_root.h"
#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
inline void serviceSetupLog(bool reset,const char*format,...){
    // Keep the descriptor opened under the original app credentials. Reopening
    // the UI-owned 0644 file while temporarily elevated loses the rooted steps
    // on this HEN; a granted descriptor remains writable across the transition.
    static int fd=-1;static std::string pending;
    char text[1200];va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
    if(reset&&fd>=0){close(fd);fd=-1;}
    int flags=O_WRONLY|O_CREAT|O_APPEND;
    if(fd<0){const std::string logPath=std::string(nullStoreDataRoot())+"/service-setup.log";const std::string paths[]={logPath,"/user"+logPath};for(const auto& path:paths){if(reset){std::string previous=path+".previous";rename(path.c_str(),previous.c_str());}fd=open(path.c_str(),flags|O_NOFOLLOW,0644);if(fd>=0)break;}}
    if(fd<0)return;
    fchmod(fd,0644);char line[1400];int n=snprintf(line,sizeof line,"[%lld][pid %d] %s\n",(long long)time(0),getpid(),text);if(n>0)pending.append(line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
    size_t done=0;while(done<pending.size()){ssize_t k=write(fd,pending.data()+done,pending.size()-done);if(k<0&&errno==EINTR)continue;if(k<=0)break;done+=(size_t)k;}if(done)pending.erase(0,done);fsync(fd);
}
inline void serviceSetupFileLog(const char*action,const std::string&path,long result){serviceSetupLog(false,"%s: %s => %ld%s%s",action,path.c_str(),result,result<0?" / ":"",result<0?strerror((int)-result):"");}
