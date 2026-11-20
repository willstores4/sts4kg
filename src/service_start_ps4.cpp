#include "data_root.h"
#include "service_start_ps4.h"
#include "service_protocol.h"
#include "queue_store.h"
#include "service_files.h"
#include "service_setup_log.h"
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <ctime>
// GoldHEN Plugin SDK syscall interface; no firmware-specific kernel offsets.
struct SdkBackup {uint32_t uid,ruid,rgid,groups;uint64_t paid,caps[2];void *prison,*cdir,*jdir,*rdir;};
static_assert(sizeof(SdkBackup)==72,"GoldHEN backup ABI");
struct JailbreakBackup {SdkBackup sdk={};bool compatibility=false;};
static int (*compatEnter)()=0;static int (*compatLeave)()=0;
static bool loadCompat(std::string&error){if(compatEnter&&compatLeave)return true;const char*paths[]={"/app0/assets/service/compat-jb.prx","/app0/sce_module/compat-jb.prx","/system/vsh/app/PRTS00001/sce_module/compat-jb.prx"};int last=-1;for(auto path:paths){int module=sceKernelLoadStartModule(path,0,0,0,0,0);if(module<0){last=module;continue;}void*enter=0;void*leave=0;if(sceKernelDlsym(module,"jailbreak_me",&enter)==0&&sceKernelDlsym(module,"rejail_multi",&leave)==0&&enter&&leave){compatEnter=(int(*)())enter;compatLeave=(int(*)())leave;return true;}last=-2;}char message[200];snprintf(message,sizeof message,"Compatibility helper could not load its permission API (0x%08X).",(unsigned)last);error=message;return false;}

extern "C" long portalSystemCall(long,...);
#ifndef PORTAL_HOST
__asm__(".text\n.globl portalSystemCall\nportalSystemCall:\nxor %rax,%rax\nmov %rcx,%r10\nsyscall\njc 1f\nret\n1: neg %rax\nret\n");
#endif
extern "C" int serviceAppId(const char*) __asm__("sceLncUtilGetAppId");
extern "C" int serviceKillApp(uint32_t) __asm__("sceLncUtilKillApp");
static const char*SERVICE_ID="PRTS00001";
static const std::string DEST="/system/vsh/app/PRTS00001";
static const char*SERVICE_INSTALL_MARKER="1.0\nstorage-layout=2\n";
static const char*SOURCES[]={"daemon.sfo","sce_module/libc.prx","sce_module/libSceFios2.prx","eboot.bin","compat-jb.prx"};
static const char*FILES[]={"sce_sys/param.sfo","sce_module/libc.prx","sce_module/libSceFios2.prx","eboot.bin","sce_module/compat-jb.prx"};
static bool grant(JailbreakBackup&backup,std::string&error){auto version=portalSystemCall(500,0,0);if((uint32_t)version==0x100){auto code=portalSystemCall(500,2,&backup.sdk);if(code==0)return true;char message[150];snprintf(message,sizeof message,"GoldHEN permission request failed (%ld).",code);error=message;return false;}if(!loadCompat(error)){char code[60];snprintf(code,sizeof code," SDK probe: 0x%08X.",(unsigned)version);error+=code;return false;}int code=compatEnter();if(code){char message[150];snprintf(message,sizeof message,"Compatibility permission request failed (0x%08X).",(unsigned)code);error=message;return false;}backup.compatibility=true;return true;}
static bool prepareQueueLockAccess(std::string&error){
    const std::string path=std::string(nullStoreDataRoot())+"/queue-owner.lock";int flags=O_RDWR|O_CREAT;
#ifdef O_NOFOLLOW
    flags|=O_NOFOLLOW;
#endif
    errno=0;int fd=open(path.c_str(),flags,0666);int openError=errno;
    serviceSetupLog(false,"prepare shared queue lock: open fd=%d errno=%d",fd,openError);
    if(fd<0){error="Cannot open the download queue lock: "+std::string(strerror(openError))+" (errno "+std::to_string(openError)+").";return false;}
    errno=0;int modeResult=fchmod(fd,0666);int modeError=errno;serviceSetupLog(false,"prepare shared queue lock: fchmod 0666 => %d errno=%d",modeResult,modeError);
    close(fd);if(modeResult){error="Cannot make the download queue lock accessible to the background service: "+std::string(strerror(modeError))+" (errno "+std::to_string(modeError)+").";return false;}return true;
}
bool portalServicePrivileges(std::string&error){JailbreakBackup backup={};return grant(backup,error);}
static bool restore(JailbreakBackup&backup,std::string&error){long code=backup.compatibility?compatLeave():portalSystemCall(500,3,&backup.sdk);if(code){char message[180];snprintf(message,sizeof message,"Permission restore failed (0x%08X). Close NULL_store and restart it.",(unsigned)code);error=message;return false;}return true;}
static bool remount(bool readonly,std::string&error){const char*names[]={"fstype","fspath","from","large","timezone","async","ignoreacl","dirmask","mask"};const char*values[]={"exfatfs","/system","/dev/da0x4.crypt","yes","static","","","511","511"};iovec args[18];for(int i=0;i<9;++i){args[i*2].iov_base=(void*)names[i];args[i*2].iov_len=strlen(names[i])+1;args[i*2+1].iov_base=(void*)values[i];args[i*2+1].iov_len=strlen(values[i])+1;}long r=portalSystemCall(378,args,18,0x10000|(readonly?1:0));serviceSetupLog(false,"nmount /system %s => %ld%s%s",readonly?"read-only":"read-write",r,r<0?" / ":"",r<0?strerror((int)-r):"");if(r){error=readonly?"Service copied, but /system could not be restored read-only. Restart the console before continuing.":"Could not remount /system for service setup (kernel result "+std::to_string(r)+").";return false;}return true;}
static bool ping(std::string&version){PortalIPC::Writer w;w.u32(PortalIPC::PING);std::string reply,error;if(!PortalIPC::request(w.bytes,reply,error))return false;try{PortalIPC::Reader r(reply);if(r.u32()!=PortalIPC::VERSION)return false;version=r.string();return true;}catch(...){return false;}}
static bool launchRooted(std::string&error){
    std::string executable;if(serviceReadRegular(DEST+"/eboot.bin",executable)!=1){error="Service not installed. Open Downloads and press L3 to set it up.";return false;}
    const std::string logPath=std::string(nullStoreDataRoot())+"/service.log";
    auto previous=readFile(logPath);size_t lastRuntimeSize=previous.size();
    int module=sceKernelLoadStartModule("/system/common/lib/libSceSystemService.sprx",0,0,0,0,0);
    serviceSetupLog(false,"load SystemService => 0x%08X",(unsigned)module);
    uint32_t before=(uint32_t)serviceAppId(SERVICE_ID);
    LncAppParam params={};params.size=sizeof params;params.user_id=(uint32_t)-1;params.LaunchAppCheck_flag=LaunchApp_SkipSystemUpdate;
    int result=(before&0xff000000u)==0x60000000u?(int)before:sceLncUtilLaunchApp(SERVICE_ID,0,&params);
    serviceSetupLog(false,"NULL_store service 1.0 launch => 0x%08X; appId before=0x%08X; installed=%s",(unsigned)result,before,readFile(DEST+"/version.txt").c_str());
    if(result>=0)for(int i=0;i<150;i++){
        std::string build;if(ping(build)){if(build!="1.0"){error="An older service is running ("+build+"). Use Install / Start to update it.";return false;}serviceSetupLog(false,"IPC ready: protocol %u, service %s, appId=0x%08X",PortalIPC::VERSION,build.c_str(),(unsigned)serviceAppId(SERVICE_ID));return true;}
        if(i%20==0){auto runtime=readFile(logPath);serviceSetupLog(false,"waiting for IPC: elapsed=%dms appId=0x%08X runtimeLogBytes=%zu delta=%lld",i*100,(unsigned)serviceAppId(SERVICE_ID),runtime.size(),(long long)runtime.size()-(long long)lastRuntimeSize);lastRuntimeSize=runtime.size();}
        sceKernelUsleep(100000);
    }
    auto current=readFile(logPath);auto details=current.size()>previous.size()?current.substr(previous.size()):std::string();
    for(char&c:details)if(c=='\n'||c=='\r'||c=='\t')c=' ';if(details.size()>180)details=details.substr(details.size()-180);
    char message[200];snprintf(message,sizeof message,"Service %s (launch 0x%08X, appId 0x%08X). ",result<0?"launch failed":"did not become ready after 15s",(unsigned)result,(unsigned)serviceAppId(SERVICE_ID));
    error=std::string(message)+(details.empty()?"See service-setup.log; no new runtime log.":details);serviceSetupLog(false,"%s",error.c_str());return false;
}
bool portalServiceLaunch(std::string&error){
    std::string build;if(ping(build))return true;struct stat installed;if(portalPathInfo((DEST+"/eboot.bin").c_str(),&installed)){error="Service not installed. Open Downloads and press L3 to set it up.";return false;}
    std::string marker;
    if(serviceReadRegular(DEST+"/version.txt",marker)==1&&marker!=SERVICE_INSTALL_MARKER){error="Open Downloads and press L3 to update the background service.";return false;}
    if(!prepareQueueLockAccess(error)){serviceSetupLog(false,"service launch preparation failed: %s",error.c_str());return false;}
    JailbreakBackup backup={};if(!grant(backup,error)){serviceSetupLog(false,"launch permission failure: %s",error.c_str());return false;}
    bool ok=launchRooted(error);bool restored=restore(backup,error);return ok&&restored;
}
static bool releaseOwnedService(std::string&error){
    uint32_t id=(uint32_t)serviceAppId(SERVICE_ID);serviceSetupLog(false,"service appId before stop: 0x%08X",id);
    if((id&0xff000000u)!=0x60000000u)return true;
    PortalIPC::Writer w;w.u32(PortalIPC::STOP);std::string reply,why;PortalIPC::request(w.bytes,reply,why);
    for(int i=0;i<20;i++){if((uint32_t)serviceAppId(SERVICE_ID)!=id)return true;sceKernelUsleep(100000);}
    int result=serviceKillApp(id);serviceSetupLog(false,"kill owned service 0x%08X => 0x%08X",id,(unsigned)result);
    for(int i=0;i<100;i++){if((uint32_t)serviceAppId(SERVICE_ID)!=id)return true;sceKernelUsleep(100000);}
    char message[160];snprintf(message,sizeof message,"Cannot stop NULL_store service for update (0x%08X).",(unsigned)result);error=message;return false;
}
static bool serviceDirectory(const std::string&path,std::string&error){
    struct stat st;int lookup=portalPathInfo(path.c_str(),&st);
    if(!lookup&&S_ISDIR(st.st_mode))return true;
    if(lookup&&errno==ENOENT&&!mkdir(path.c_str(),0755)){serviceSetupLog(false,"created directory: %s",path.c_str());return true;}
    int code=lookup?errno:ENOTDIR;serviceSetupFileLog("directory",path,-code);error="Cannot prepare service directory "+path+": "+strerror(code)+" (errno "+std::to_string(code)+")";return false;
}
bool portalServiceInstallAndLaunch(std::string&error){
    error.clear();serviceSetupLog(true,"NULL_store 1.0 service setup started; pid=%d uid=%d",getpid(),getuid());
    struct SetupRecord{std::string&error;~SetupRecord(){serviceSetupLog(false,"service setup %s%s%s",error.empty()?"succeeded":"failed",error.empty()?"":": ",error.c_str());}}record{error};
    std::string running;if(ping(running)&&running=="1.0")return true;
    // Read and validate the whole bundle before touching the installed service.
    std::string files[5];for(int i=0;i<5;i++){
        auto path=std::string("/app0/assets/service/")+SOURCES[i];files[i]=readFile(path);
        serviceSetupLog(false,"bundle %s: %zu bytes",SOURCES[i],files[i].size());
        if(files[i].empty()){error="Cannot read service bundle file: "+std::string(SOURCES[i])+". Reinstall NULL_store 1.0 or later.";return false;}
    }
    if(files[3].size()<4||(unsigned char)files[3][0]!=0x4f||(unsigned char)files[3][1]!=0x15||(unsigned char)files[3][2]!=0x3d||(unsigned char)files[3][3]!=0x1d){error="Invalid service executable in app package.";return false;}
    if(!prepareQueueLockAccess(error))return false;
    JailbreakBackup backup={};if(!grant(backup,error))return false;
    serviceSetupLog(false,"permissions acquired: uid=%d via %s",getuid(),backup.compatibility?"compatibility helper":"GoldHEN SDK");
    bool changedMount=false,ok=true;struct stat existing;int lookup=portalPathInfo(DEST.c_str(),&existing);bool exists=lookup==0;
    if(lookup&&errno!=ENOENT){int code=errno;error="Cannot inspect service destination: "+std::string(strerror(code))+" (errno "+std::to_string(code)+")";ok=false;}
    if(ok&&exists){std::string marker;if(!S_ISDIR(existing.st_mode)||serviceReadRegular(DEST+"/portal-owner.txt",marker)!=1||marker!="PORTAL_SERVICE_OWNED\n"){error="Service folder already exists and is not owned by NULL_store.";ok=false;}}
    if(ok)ok=releaseOwnedService(error);
    const std::string recoveryRoot=std::string(nullStoreDataRoot())+"/service-recovery";
    std::string recovery=recoveryRoot+"/"+std::to_string((long long)time(0))+"-"+std::to_string(getpid());
    if(ok)ok=serviceDirectory(recoveryRoot,error)&&serviceDirectory(recovery,error)&&serviceDirectory(recovery+"/sce_sys",error)&&serviceDirectory(recovery+"/sce_module",error);
    // A create-only test cannot prove rename/unlink work on PS4 exFAT. Remount
    // before every install/update, matching the working Itemzflow daemon loader.
    if(ok){ok=remount(false,error);changedMount=ok;}
    if(ok)ok=serviceDirectory(DEST,error)&&serviceDirectory(DEST+"/sce_sys",error)&&serviceDirectory(DEST+"/sce_module",error);
    if(ok)ok=serviceReplaceOwnedFile(DEST+"/portal-owner.txt","PORTAL_SERVICE_OWNED\n",recovery+"/portal-owner.txt",serviceSetupFileLog);
    for(int i=0;i<5&&ok;i++){
        ok=serviceReplaceOwnedFile(DEST+"/"+FILES[i],files[i],recovery+"/"+FILES[i],serviceSetupFileLog);
        if(!ok){int code=errno;error="Cannot install "+std::string(FILES[i])+": "+strerror(code)+" (errno "+std::to_string(code)+"). See service-setup.log for the failed operation.";}
    }
    if(ok){ok=serviceReplaceOwnedFile(DEST+"/version.txt",SERVICE_INSTALL_MARKER,recovery+"/version.txt",serviceSetupFileLog);if(!ok)error="Cannot write service version marker: "+std::string(strerror(errno));}
    if(!ok&&error.empty())error="Service file update failed: "+std::string(strerror(errno));
    if(changedMount){std::string mountError;if(!remount(true,mountError)){if(!error.empty())error+=" ";error+=mountError;ok=false;}}
    if(ok)ok=launchRooted(error);
    bool restored=restore(backup,error);serviceSetupLog(false,"permission restore => %s, uid=%d",restored?"ok":"failed",getuid());return ok&&restored;
}
