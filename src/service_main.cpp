#include "service_server.h"
#include <cstdlib>
#ifdef PORTAL_HOST
#include "catalog.h"
static bool hostFetch(const std::string&url,std::string&out,size_t,const std::string&){const char*path=getenv(url.find("info_hash=")!=std::string::npos?"PORTAL_TEST_TRACKER":"PORTAL_TEST_META");if(!path)return false;out=readFile(path);return !out.empty();}
int main(int argc,char**argv){if(argc<2)return 1;return runPortalService(argv[1],hostFetch,argc>2?atoi(argv[2]):17941);}
#else
#include "data_root.h"
#include "network_ps4.h"
#include "service_start_ps4.h"
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include "service_log.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
static void serviceEarlyStartup() __attribute__((constructor));
static void serviceEarlyStartup(){serviceLogReopen();serviceLogInstallCrashHandlers();serviceLog("early constructor reached; entering C++ runtime, pid=%d uid=%d",getpid(),getuid());}
static void serviceAtExit(){serviceLog("C runtime exit handler reached");}
int main(){
    serviceLog("main entered; pid=%d uid=%d",getpid(),getuid());
    if(atexit(serviceAtExit)!=0)serviceLog("could not register C runtime exit marker");
    std::string error;
    serviceLog("permission elevation begin");
    bool permitted=portalServicePrivileges(error);
    serviceLogReopen();
    serviceLog("permission elevation result=%d uid=%d error=%s",permitted?1:0,getuid(),error.c_str());
    if(!permitted)return 10;
    OrbisUserServiceInitializeParams p={};p.priority=256;
    serviceLog("sceUserServiceInitialize begin");
    int user=sceUserServiceInitialize(&p);
    serviceLog("sceUserServiceInitialize => 0x%08X",(unsigned)user);
    serviceLog("sceSystemServiceRegisterDaemon begin");
    sceSystemServiceRegisterDaemon();
    serviceLog("sceSystemServiceRegisterDaemon returned");
    serviceLog("network initialization begin");
    bool networkReady=portalNetworkInit();
    serviceLog("network initialization result=%d",networkReady?1:0);
    if(!networkReady)return 11;
    const char*root=nullStoreDataRoot();
    serviceLog("service server begin; root=%s port=17941",root);
    int result=runPortalService(root,portalNetworkFetch);
    serviceLog("service server returned=%d errno=%d",result,errno);
    return result;
}
#endif
