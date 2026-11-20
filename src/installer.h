#pragma once
#include "catalog.h"
#include <stdint.h>
#include <pthread.h>
enum class InstallState {Ready,Registering,Registered,Installing,Complete,Error,Unavailable};
struct InstallPackage {int jobId=0,taskId=-1;std::string path,name,contentId,titleId,note;uint64_t bytes=0;uint32_t contentType=0,contentFlags=0;bool patch=false;InstallState state=InstallState::Ready;int percent=-1;};
inline std::string installStatusText(const InstallPackage& package){
    if(package.state==InstallState::Complete)return "Installed";
    if(package.state!=InstallState::Installing)return package.note;
    if(package.percent>0&&package.percent<100)return "Installing  "+std::to_string(package.percent)+"%";
    return "Installing";
}
struct InstallBackend {
    bool (*registerTask)(const InstallPackage&,int&,std::string&);
    bool (*startTask)(int,std::string&);
    bool (*pollTask)(int,int&,bool&,std::string&);
    bool (*resumeTask)(int,std::string&);
};
bool inspectPackage(const std::string&,InstallPackage&,std::string&);
std::vector<InstallPackage> scanPackages(const std::string&,int,std::string&);
std::vector<InstallPackage> scanPackages(const std::string&,int,const std::string&,std::string&);
inline std::string storageInstallPath(const std::string&p){return p.compare(0,6,"/data/")==0?"/user"+p:p;}
class PackageInstaller {
    std::string root;InstallBackend backend;pthread_mutex_t mutex;pthread_cond_t changed;pthread_t worker;bool started=false,quitting=false,scanPending=false;int scanJob=0,viewJob=0;std::string scanDir,viewError;bool loading=false;uint64_t version=1;std::vector<InstallPackage>view,tasks;
    std::vector<InstallPackage> requests;std::string busyPath;bool journalDirty=false;int journalError=0;
    static void* entry(void*);void loop();bool saveLocked();void updateViewLocked();
public:
    PackageInstaller(std::string,InstallBackend);~PackageInstaller();bool start();
    void scan(int);void scan(int,const std::string&);void submit(const std::string&);bool snapshot(uint64_t&,std::vector<InstallPackage>&,bool&,std::string&);
};
InstallBackend ps4InstallBackend();
