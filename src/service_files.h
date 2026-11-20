#pragma once
#include "queue_store.h"
#include "path_info.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

// Only called for fixed filenames beneath the validated, owned service folder.
// The recovery copy lives on /data: it must not depend on /system accepting
// rename/unlink. Itemzflow's daemon installer also uses direct kernel file copies
// after remounting /system (itemz-loader/source/daemon.cpp).
typedef void (*ServiceFileLog)(const char*,const std::string&,long);
struct ServiceFileIO {
    bool (*replace)(const std::string&,const std::string&);
    int (*flush)(int);
    ssize_t (*writeBytes)(int,const void*,size_t);
};
inline ServiceFileIO serviceFileIO(){ServiceFileIO io={durableReplace,fsync,write};return io;}
inline int serviceReadRegular(const std::string&path,std::string&out){
    out.clear();int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;int code=0;if(portalDescriptorInfo(fd,&st))code=errno;
    else if(!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>16*1024*1024)code=EINVAL;
    if(!code){out.resize((size_t)st.st_size);size_t n=0;while(n<out.size()){ssize_t k=read(fd,&out[n],out.size()-n);if(k<0&&errno==EINTR)continue;if(k<=0){code=k<0?errno:EIO;break;}n+=(size_t)k;}}
    if(close(fd)&&!code)code=errno;if(code){out.clear();errno=code;return -1;}return 1;
}
inline bool serviceDirectWrite(const std::string&path,const std::string&bytes,ServiceFileLog log,const ServiceFileIO&io,bool&opened){
    opened=false;int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0755);
    if(fd<0){int code=errno;if(log)log("direct-open",path,-code);errno=code;return false;}opened=true;
    size_t n=0;int code=0;
    while(n<bytes.size()){ssize_t k=io.writeBytes(fd,bytes.data()+n,bytes.size()-n);if(k<0&&errno==EINTR)continue;if(k<=0){code=k<0?errno:EIO;break;}n+=(size_t)k;}
    if(log)log("direct-write",path,code?-code:(long)n);
    if(!code){int r;do{r=io.flush(fd);}while(r&&errno==EINTR);if(r){int syncCode=errno;if(log)log("direct-fsync",path,-syncCode);
        // Some PS4 system exFAT operations report EROFS after accepting writes.
        // Keep the /data recovery copy, require a complete read-back match, and
        // let the installer's read-only remount flush the system volume.
        if(syncCode!=EROFS)code=syncCode;
    }else if(log)log("direct-fsync",path,0);}
    if(close(fd)&&!code)code=errno;
    std::string actual;if(!code&&(serviceReadRegular(path,actual)!=1||actual!=bytes))code=EIO;
    if(log)log("direct-readback",path,code?-code:(long)actual.size());
    if(code){errno=code;return false;}return true;
}
inline bool serviceReplaceOwnedFile(const std::string&path,const std::string&bytes,const std::string&backup,ServiceFileLog log,const ServiceFileIO&io=serviceFileIO()){
    std::string previous;int exists=serviceReadRegular(path,previous);
    if(exists<0){int code=errno;if(log)log("inspect-target",path,-code);errno=code;return false;}
    if(exists&&previous==bytes){if(log)log("unchanged",path,(long)bytes.size());return true;}
    // Never truncate an existing file unless a durable, byte-checked recovery
    // copy has already been made on the writable data volume.
    if(exists){std::string checked;if(!durableReplace(backup,previous)||serviceReadRegular(backup,checked)!=1||checked!=previous){int code=errno?errno:EIO;if(log)log("recovery-backup",backup,-code);errno=code;return false;}if(log)log("recovery-backup",backup,(long)previous.size());}
    if(io.replace(path,bytes)){std::string actual;if(serviceReadRegular(path,actual)==1&&actual==bytes){if(log)log("atomic-readback",path,(long)actual.size());return true;}
        if(log)log("atomic-readback",path,-EIO);
        if(exists){bool opened=false;bool restored=serviceDirectWrite(path,previous,log,io,opened);if(log)log(restored?"rollback-restored":"rollback-failed",path,restored?0:-errno);}errno=EIO;return false;}
    int code=errno;if(log)log("atomic-replace",path,-code);
    if(code!=EROFS&&code!=EEXIST&&code!=ENOTEMPTY){errno=code;return false;}
    bool opened=false;if(serviceDirectWrite(path,bytes,log,io,opened))return true;
    code=errno;
    if(exists&&opened){bool restoredOpen=false;bool restored=serviceDirectWrite(path,previous,log,io,restoredOpen);if(log)log(restored?"rollback-restored":"rollback-failed",path,restored?0:-errno);}
    errno=code;return false;
}
