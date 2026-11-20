#pragma once
#include "catalog.h"
#include "path_info.h"
#include <unistd.h>
#include <stdint.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
inline uint32_t queueChecksum(const std::string&s){uint32_t n=2166136261u;for(unsigned char c:s){n^=c;n*=16777619u;}return n;}
inline bool decodeQueue(const std::string&raw,std::string&payload){if(raw.empty())return false;if(raw.compare(0,16,"PORTAL_QUEUE_V2\t")==0){auto e=raw.find('\n');if(e==std::string::npos)return false;std::string hash=raw.substr(16,e-16);char*end=0;unsigned long expected=strtoul(hash.c_str(),&end,16);if(end==hash.c_str()||*end||expected>UINT32_MAX)return false;payload=raw.substr(e+1);return queueChecksum(payload)==expected;}if(!isdigit((unsigned char)raw[0])||raw.find('\t')==std::string::npos||raw.back()!='\n')return false;payload=raw;return true;}
// A missing queue is new storage. An unreadable queue must never become a new,
// empty queue: the daemon and application run under different user IDs.
inline int readQueueFile(const std::string&path,std::string&raw){
    raw.clear();int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;int saved=0;
    if(portalDescriptorInfo(fd,&st))saved=errno;
    else if(!S_ISREG(st.st_mode))saved=EINVAL;
    else if(st.st_size>16*1024*1024)saved=EFBIG;
    char buffer[8192];while(!saved){ssize_t n=read(fd,buffer,sizeof buffer);if(n<0&&errno==EINTR)continue;if(n<0){saved=errno;break;}if(!n)break;if(raw.size()+(size_t)n>16*1024*1024){saved=EFBIG;break;}raw.append(buffer,(size_t)n);}
    close(fd);if(saved){raw.clear();errno=saved;return -1;}return 1;
}
inline bool durableReplace(const std::string&path,const std::string&text,mode_t mode){
    // A fixed .tmp file can be left owned by root after a daemon crash. Each
    // writer uses a fresh, exclusive file and publishes only after syncing it.
    std::string pattern=path+".tmp-XXXXXX";std::vector<char>name(pattern.begin(),pattern.end());name.push_back(0);
    int fd=mkstemp(name.data());if(fd<0)return false;std::string temp=name.data();size_t n=0;int saved=0;
    while(n<text.size()){ssize_t k=write(fd,text.data()+n,text.size()-n);if(k<0&&errno==EINTR)continue;if(k<=0){saved=k<0?errno:EIO;break;}n+=(size_t)k;}
    if(!saved&&fchmod(fd,mode))saved=errno;if(!saved&&fsync(fd))saved=errno;if(close(fd)&&!saved)saved=errno;
    if(!saved&&rename(temp.c_str(),path.c_str()))saved=errno;if(saved){unlink(temp.c_str());errno=saved;return false;}
    auto slash=path.find_last_of('/');std::string dir=slash==std::string::npos?".":path.substr(0,slash);int parent=open(dir.c_str(),O_RDONLY);if(parent>=0){fsync(parent);close(parent);}return true;
}
inline bool durableReplace(const std::string&path,const std::string&text){return durableReplace(path,text,0600);}
inline bool saveQueue(const std::string&path,const std::string&payload,mode_t mode=0600){std::string old,checked;if(readQueueFile(path,old)<0)return false;if(decodeQueue(old,checked)&&!durableReplace(path+".bak",old,mode))return false;char hash[16];snprintf(hash,sizeof hash,"%08x",queueChecksum(payload));return durableReplace(path,std::string("PORTAL_QUEUE_V2\t")+hash+"\n"+payload,mode);}
inline std::string loadQueue(const std::string&path,bool*recovered=0,std::string*error=0){
    if(recovered)*recovered=false;if(error)error->clear();std::string raw,payload;
    int primary=readQueueFile(path,raw);if(primary<0){if(error)*error="Cannot read saved queue (errno "+std::to_string(errno)+"). Update the background service and retry.";return "";}
    if(primary&&decodeQueue(raw,payload))return payload;
    int backup=readQueueFile(path+".bak",raw);if(backup<0){if(error)*error="Cannot read queue backup (errno "+std::to_string(errno)+").";return "";}
    if(backup&&decodeQueue(raw,payload)){if(recovered)*recovered=true;return payload;}
    if((primary||backup)&&error)*error="Saved download queue is damaged. Its files have been preserved.";return "";
}
inline int repairSharedDownloadQueue(const std::string&root){
    int failure=0;for(const char*suffix:{"",".bak"}){
        std::string path=root+"/downloads.tsv"+suffix;int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0){if(errno!=ENOENT&&!failure)failure=errno;continue;}
        struct stat st;if(portalDescriptorInfo(fd,&st)){if(!failure)failure=errno;}
        else if(!S_ISREG(st.st_mode)){if(!failure)failure=EINVAL;}
        else if((st.st_mode&0777)!=0644&&fchmod(fd,0644)&&!failure)failure=errno;
        close(fd);
    }return failure;
}
