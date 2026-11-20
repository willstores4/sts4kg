#pragma once
#include "data_root.h"
#include <cstdio>
#include <cstdarg>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
inline void installLog(const char*format,...){
#ifndef PORTAL_HOST
 char text[1400];va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
 const char*root=nullStoreDataRoot();char path[128];snprintf(path,sizeof path,"%s/installer.log",root);
 int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0644);
 if(fd<0){snprintf(path,sizeof path,"%s/installer-%u.log",root,(unsigned)getuid());fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0644);}
 if(fd<0)return;fchmod(fd,0644);
 char line[1500];int n=snprintf(line,sizeof line,"[%lld][pid %d] %s\n",(long long)time(0),getpid(),text);if(n>0){size_t size=(size_t)n<sizeof line?(size_t)n:sizeof line-1;size_t done=0;while(done<size){ssize_t k=write(fd,line+done,size-done);if(k<0&&errno==EINTR)continue;if(k<=0)break;done+=(size_t)k;}}close(fd);
#else
 (void)format;
#endif
}
