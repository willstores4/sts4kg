#include "path_info.h"
#include "installer.h"
#include "queue_store.h"
#include "download_paths.h"
#include "install_log.h"
#include "bgft_abi.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
static uint32_t big32(const unsigned char*p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
static uint64_t big64(const unsigned char*p){return uint64_t(big32(p))<<32|big32(p+4);}
bool inspectPackage(const std::string&path,InstallPackage&pkg,std::string&error){
    int flags=O_RDONLY;
#ifdef O_NOFOLLOW
    flags|=O_NOFOLLOW;
#endif
    int fd=open(path.c_str(),flags);if(fd<0){error="Cannot open package";installLog("open %s: errno %d",path.c_str(),errno);return false;}struct stat st;unsigned char h[0x1000];bool ok=portalDescriptorInfo(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>=0x1000;size_t got=0;while(ok&&got<sizeof h){ssize_t n=pread(fd,h+got,sizeof h-got,got);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}got+=(size_t)n;}close(fd);
    if(!ok||big32(h)!=0x7f434e54){error="Not a PS4 PKG file";return false;}
    uint64_t size=big64(h+0x430),body=big64(h+0x20),bodySize=big64(h+0x28);installLog("inspect %s: file=%llu declared=%llu body=%llu+%llu type=0x%X",path.c_str(),(unsigned long long)st.st_size,(unsigned long long)size,(unsigned long long)body,(unsigned long long)bodySize,big32(h+0x74));if(size!=(uint64_t)st.st_size){error="Package size mismatch: header "+std::to_string(size)+" bytes, file "+std::to_string((uint64_t)st.st_size)+" bytes. Resume or recheck the download.";return false;}if(body>size||bodySize>size-body){error="Invalid package body range";return false;}
    std::string cid((const char*)h+0x40,36);for(unsigned char c:cid)if(!(isalnum(c)||c=='-'||c=='_')){error="Invalid package content ID";return false;}
    if(cid[6]!='-'||cid[16]!='_'||cid[19]!='-'){error="Invalid package content ID";return false;}uint32_t type=big32(h+0x74);if(type!=0x1a&&type!=0x1b&&type!=0x1c&&type!=0x1e){error="Unsupported PS4 package type";return false;}
    pkg.path=path;pkg.bytes=size;pkg.contentId=cid;pkg.titleId=cid.substr(7,9);pkg.contentType=type;pkg.contentFlags=big32(h+0x78);pkg.patch=NullBGFT::isPatch(type,pkg.contentFlags);auto slash=path.find_last_of('/');pkg.name=path.substr(slash==std::string::npos?0:slash+1);pkg.note=pkg.patch?"Update package; base game required":"Ready to install";return true;
}
static void scanDir(const std::string&path,int job,std::vector<InstallPackage>&v,std::string&error,int depth,int&entries){if(depth>16||entries>8192||v.size()>=128)return;struct stat root;if(portalPathInfo(path.c_str(),&root)||!S_ISDIR(root.st_mode))return;DIR*d=opendir(path.c_str());if(!d){error="Cannot read downloaded files";return;}while(auto*e=readdir(d)){std::string name=e->d_name;if(name=="."||name=="..")continue;if(++entries>8192)break;std::string p=path+"/"+name;struct stat st;if(portalPathInfo(p.c_str(),&st))continue;if(S_ISDIR(st.st_mode))scanDir(p,job,v,error,depth+1,entries);else if(S_ISREG(st.st_mode)&&name.size()>4&&lower(name.substr(name.size()-4))==".pkg"){InstallPackage pkg;std::string why;if(inspectPackage(p,pkg,why)){pkg.jobId=job;v.push_back(pkg);}else error=name+": "+why;}}closedir(d);}
std::vector<InstallPackage> scanPackages(const std::string&root,int job,const std::string&downloadDir,std::string&error){std::vector<InstallPackage>v;int entries=0;if(job<=0||!validDownloadDir(root,job,downloadDir)){error="Select a valid completed download first";installLog("scan packages rejected: job=%d path=%s",job,downloadDir.c_str());return v;}std::string path=downloadDir;installLog("scan packages begin: job=%d path=%s",job,path.c_str());scanDir(path,job,v,error,0,entries);std::sort(v.begin(),v.end(),[](const InstallPackage&a,const InstallPackage&b){return lower(a.name)<lower(b.name);});if(v.empty()&&error.empty())error="No complete .pkg files found. Archives must be extracted first.";installLog("scan packages complete: job=%d entries=%d packages=%zu error=%s",job,entries,v.size(),error.c_str());return v;}
std::vector<InstallPackage> scanPackages(const std::string&root,int job,std::string&error){return scanPackages(root,job,defaultDownloadDir(root,job),error);}
static std::string cleanInstall(std::string s){for(char&c:s)if(c=='\t'||c=='\r'||c=='\n')c=' ';return s;}
PackageInstaller::PackageInstaller(std::string p,InstallBackend b):root(p),backend(b){pthread_mutex_init(&mutex,0);pthread_cond_init(&changed,0);auto raw=loadQueue(root+"/installs.tsv");size_t p0=0;while(p0<raw.size()&&tasks.size()<256){auto end=raw.find('\n',p0);if(end==std::string::npos)break;auto line=raw.substr(p0,end-p0);p0=end+1;std::vector<std::string>f;size_t k=0;for(;;){auto e=line.find('\t',k);f.push_back(line.substr(k,e-k));if(e==std::string::npos)break;k=e+1;}if(f.size()!=9)continue;InstallPackage t;t.jobId=atoi(f[0].c_str());t.path=f[1];t.name=f[2];t.contentId=f[3];t.bytes=strtoull(f[4].c_str(),0,10);t.taskId=atoi(f[5].c_str());int state=atoi(f[6].c_str());t.percent=std::max(-1,std::min(100,atoi(f[7].c_str())));t.note=f[8];bool safe=pathInsideDownloadDir(root,t.jobId,defaultDownloadDir(root,t.jobId),t.path);for(int i=0;i<8&&!safe;i++){std::string dir="/mnt/usb"+std::to_string(i)+"/NULL_store/downloads/"+std::to_string(t.jobId);safe=pathInsideDownloadDir(root,t.jobId,dir,t.path);}if(t.jobId<=0||!safe||state<0||state>6||t.taskId<0)continue;t.titleId=t.contentId.size()==36?t.contentId.substr(7,9):"";t.state=InstallState(state);if(t.state==InstallState::Error&&t.note.find("System task registered, but its recovery record") == 0){t.state=InstallState::Installing;t.percent=-1;t.note="Installing";}tasks.push_back(t);}}
PackageInstaller::~PackageInstaller(){pthread_mutex_lock(&mutex);quitting=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);if(started)pthread_join(worker,0);pthread_cond_destroy(&changed);pthread_mutex_destroy(&mutex);}
bool PackageInstaller::start(){if(started)return true;started=pthread_create(&worker,0,entry,this)==0;return started;}
void* PackageInstaller::entry(void*p){static_cast<PackageInstaller*>(p)->loop();return 0;}
bool PackageInstaller::saveLocked(){std::string s;for(auto&t:tasks)s+=std::to_string(t.jobId)+"\t"+cleanInstall(t.path)+"\t"+cleanInstall(t.name)+"\t"+t.contentId+"\t"+std::to_string(t.bytes)+"\t"+std::to_string(t.taskId)+"\t"+std::to_string(int(t.state))+"\t"+std::to_string(t.percent)+"\t"+cleanInstall(t.note)+"\n";bool wasDirty=journalDirty;
    bool ok=saveQueue(root+"/installs.tsv",s,0644);int code=ok?0:errno;
    journalDirty=!ok;
    if(!ok&&(!wasDirty||journalError!=code))installLog("Installer recovery save failed: path=%s/installs.tsv errno=%d (%s); continuing task monitoring",root.c_str(),code,strerror(code));
    if(ok&&wasDirty)installLog("Installer recovery save restored: %zu tasks",tasks.size());
    journalError=code;if(wasDirty!=journalDirty)++version;
    return ok;
}
void PackageInstaller::updateViewLocked(){for(auto&v:view)for(auto&t:tasks)if(v.path==t.path)v=t;++version;}
void PackageInstaller::scan(int job){scan(job,defaultDownloadDir(root,job));}
void PackageInstaller::scan(int job,const std::string&dir){pthread_mutex_lock(&mutex);scanJob=viewJob=job;scanDir=dir;scanPending=loading=true;view.clear();viewError.clear();++version;pthread_cond_signal(&changed);pthread_mutex_unlock(&mutex);}
void PackageInstaller::submit(const std::string&path){pthread_mutex_lock(&mutex);for(auto&p:view)if(p.path==path){if(p.state==InstallState::Registering||(p.taskId>=0&&p.state!=InstallState::Error&&p.state!=InstallState::Unavailable)){viewError="Already handed to the system installer. Check PS4 Notifications.";++version;break;}bool duplicate=busyPath==path||std::find_if(requests.begin(),requests.end(),[&](const InstallPackage&r){return r.path==path;})!=requests.end();if(!duplicate){requests.push_back(p);p.state=InstallState::Registering;p.note="Preparing system installation";++version;pthread_cond_signal(&changed);}break;}pthread_mutex_unlock(&mutex);}
bool PackageInstaller::snapshot(uint64_t&v,std::vector<InstallPackage>&out,bool&busy,std::string&error){pthread_mutex_lock(&mutex);bool changed=v!=version;if(changed){out=view;busy=loading;error=viewError;if(error.empty()&&journalDirty)error="Installation continues. Keep NULL_store open while its progress record is being saved.";v=version;}pthread_mutex_unlock(&mutex);return changed;}
void PackageInstaller::loop(){for(;;){pthread_mutex_lock(&mutex);if(quitting){pthread_mutex_unlock(&mutex);return;}
    if(scanPending){int job=scanJob;std::string dir=scanDir;scanPending=false;pthread_mutex_unlock(&mutex);std::string error;auto found=scanPackages(root,job,dir,error);pthread_mutex_lock(&mutex);if(viewJob==job&&scanDir==dir){view=found;viewError=error;loading=false;updateViewLocked();}pthread_mutex_unlock(&mutex);continue;}
    if(!requests.empty()){
        InstallPackage request=requests.front();requests.erase(requests.begin());std::string path=request.path;busyPath=path;int job=request.jobId;
        pthread_mutex_unlock(&mutex);
        InstallPackage pkg;std::string error;bool reused=request.taskId>=0,alreadyComplete=false,ok=false;
        if(reused){
            pkg=request;int percent=pkg.percent;
            ok=backend.pollTask&&backend.pollTask(pkg.taskId,percent,alreadyComplete,error);pkg.percent=percent;
            if(ok&&!alreadyComplete)ok=backend.resumeTask?backend.resumeTask(pkg.taskId,error):backend.startTask&&backend.startTask(pkg.taskId,error);
            installLog("Retry owned task: path=%s task=%d complete=%d ok=%d",path.c_str(),pkg.taskId,alreadyComplete,ok);
        }else{
            ok=job>0&&inspectPackage(path,pkg,error);pkg.jobId=job;
            if(ok)ok=backend.registerTask&&backend.registerTask(pkg,pkg.taskId,error)&&pkg.taskId>=0;
        }
        pthread_mutex_lock(&mutex);
        if(ok&&!reused){
            pkg.state=InstallState::Registered;pkg.note="Registered with PS4 installer";
            tasks.erase(std::remove_if(tasks.begin(),tasks.end(),[&](const InstallPackage&t){return t.path==path;}),tasks.end());tasks.push_back(pkg);
            saveLocked();
        }
        pthread_mutex_unlock(&mutex);
        if(ok&&!reused)ok=backend.startTask&&backend.startTask(pkg.taskId,error);
        pthread_mutex_lock(&mutex);
        if(ok){
            viewError.clear();for(auto&t:tasks)if(t.taskId==pkg.taskId){t.state=alreadyComplete?InstallState::Complete:InstallState::Installing;t.percent=alreadyComplete?100:std::max(-1,std::min(99,pkg.percent));t.note=alreadyComplete?"Installed":"Installing";}saveLocked();
        }else{
            viewError=error.empty()?"Could not install this package":error;
            for(auto&v:view)if(v.path==path){v.state=InstallState::Error;v.note=viewError;}
            for(auto&t:tasks)if(t.path==path){t.state=InstallState::Error;t.note=viewError;}saveLocked();
        }
        busyPath.clear();updateViewLocked();pthread_mutex_unlock(&mutex);continue;
    }
    // Only IDs created by Portal are polled; never adopt a task by content ID.
    std::vector<InstallPackage>poll;for(auto&t:tasks)if(t.state==InstallState::Registered||t.state==InstallState::Installing||t.state==InstallState::Unavailable)poll.push_back(t);pthread_mutex_unlock(&mutex);
    for(auto&t:poll){std::string error;bool ok=true;if(t.state==InstallState::Registered)ok=backend.resumeTask?backend.resumeTask(t.taskId,error):backend.startTask&&backend.startTask(t.taskId,error);int percent=t.percent;bool complete=false;if(ok)ok=backend.pollTask&&backend.pollTask(t.taskId,percent,complete,error);pthread_mutex_lock(&mutex);for(auto&owned:tasks)if(owned.taskId==t.taskId){InstallState state=!ok?InstallState::Unavailable:complete?InstallState::Complete:InstallState::Installing;int value=complete?100:std::max(-1,std::min(99,percent));std::string note=!ok?error:complete?"Installed":"Installing";bool changed=owned.state!=state||owned.percent!=value||owned.note!=note;owned.state=state;owned.percent=value;owned.note=note;if(changed){saveLocked();updateViewLocked();}break;}pthread_mutex_unlock(&mutex);}
    pthread_mutex_lock(&mutex);if(journalDirty)saveLocked();if(!quitting&&!scanPending&&requests.empty()){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=1;pthread_cond_timedwait(&changed,&mutex,&until);}pthread_mutex_unlock(&mutex);
}}
