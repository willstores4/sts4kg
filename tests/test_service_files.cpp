#define PORTAL_HOST
#include "../src/service_files.h"
#include <cassert>
#include <iostream>
#include <vector>
static int atomicCalls=0,writeMode=0,flushError=0,writeCount=0;
static std::vector<std::string>events;
static void logFile(const char*action,const std::string&,long result){events.push_back(std::string(action)+":"+std::to_string(result));}
static bool blockedRename(const std::string&,const std::string&){++atomicCalls;errno=EROFS;return false;}
static bool blockedPermission(const std::string&,const std::string&){++atomicCalls;errno=EACCES;return false;}
static bool brokenPublish(const std::string&p,const std::string&){return durableReplace(p,"bad published bytes");}
static int flushFile(int fd){if(flushError){errno=flushError;return -1;}return fsync(fd);}
static ssize_t writeFileBytes(int fd,const void*p,size_t n){++writeCount;if(writeMode==1){if(writeCount==1)return write(fd,p,std::min(n,size_t(3)));writeMode=0;errno=ENOSPC;return -1;}if(writeMode==2){writeMode=0;errno=EINTR;return -1;}return write(fd,p,std::min(n,size_t(7)));}
int main(){
 char temp[]="/tmp/nullstore-service-files-XXXXXX";assert(mkdtemp(temp));std::string root=temp;
 const std::string target=root+"/eboot.bin",backup=root+"/recovery.bin",previous="old executable contents",next="new verified executable contents";
 ServiceFileIO io={blockedRename,flushFile,writeFileBytes};
 assert(durableReplace(target,previous));assert(serviceReplaceOwnedFile(target,next,backup,logFile,io));assert(readFile(target)==next&&readFile(backup)==previous);
 assert(durableReplace(target,previous));flushError=EROFS;assert(serviceReplaceOwnedFile(target,next,backup,logFile,io));assert(readFile(target)==next&&readFile(backup)==previous);flushError=0;
 assert(durableReplace(target,previous));writeMode=2;assert(serviceReplaceOwnedFile(target,next,backup,logFile,io));assert(readFile(target)==next);
 assert(durableReplace(target,previous));writeMode=1;writeCount=0;assert(!serviceReplaceOwnedFile(target,next,backup,logFile,io)&&errno==ENOSPC);assert(readFile(target)==previous&&readFile(backup)==previous);
 atomicCalls=0;assert(!serviceReplaceOwnedFile(target,next,root+"/missing/recovery",logFile,io));assert(atomicCalls==0&&readFile(target)==previous);
 io.replace=blockedPermission;assert(!serviceReplaceOwnedFile(target,next,backup,logFile,io)&&errno==EACCES);assert(readFile(target)==previous);
 io.replace=brokenPublish;assert(!serviceReplaceOwnedFile(target,next,backup,logFile,io)&&errno==EIO);assert(readFile(target)==previous);
 io.replace=blockedRename;atomicCalls=0;assert(serviceReplaceOwnedFile(target,previous,backup,logFile,io));assert(atomicCalls==0);
 auto link=root+"/linked.bin";assert(!symlink(target.c_str(),link.c_str()));assert(!serviceReplaceOwnedFile(link,next,backup,logFile,io));assert(readFile(target)==previous);
 assert(std::find(events.begin(),events.end(),"atomic-replace:"+std::to_string(-EROFS))!=events.end());assert(std::find(events.begin(),events.end(),"rollback-restored:0")!=events.end());
 unlink(link.c_str());unlink(target.c_str());unlink(backup.c_str());rmdir(root.c_str());
 std::cout<<"Service exFAT fallback, verified recovery, short writes, sync errors, rollback and symlink rejection passed.\n";
}
