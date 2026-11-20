#define PORTAL_HOST
#include "../src/installer_ps4.cpp"
#include <cassert>
#include <iostream>
static int initCalls=0,registerCalls=0,startCalls=0,resumeCalls=0,initialCalls=0,termCalls=0,nativeCalls=0;
static int initResult=0,registerResult=0,taskId=42,foregroundResult=0,foregroundUser=7,initialResult=0,initialUser=9,progressResult=0,startResult=0,resumeResult=0;
static uint32_t bgftModuleResult=0;static int nativeResult=101,nativeStartResult=0;
static std::string expectedType="PS4GD",nativePath;static int expectedUser=7;
static int bgftModuleCalls=0;
static const char*sandboxWord="testSandbox";static bool globalMissing=false,sandboxMissing=false;
static std::vector<std::string>mappedNames={"libSceLibcInternal"};
static Progress returnedProgress={};
extern "C" int32_t sceKernelGetModuleList(uint32_t*handles,size_t size,size_t*count){assert(size>=mappedNames.size());*count=mappedNames.size();for(size_t i=0;i<*count;i++)handles[i]=(uint32_t)i+20;return 0;}
extern "C" int32_t sceKernelGetModuleInfo(uint32_t h,OrbisKernelModuleInfo*info){assert(info->size==sizeof *info&&h>=20&&h-20<mappedNames.size());snprintf(info->name,sizeof info->name,"%s",mappedNames[h-20].c_str());return 0;}
extern "C" uint32_t sceSysmoduleLoadModuleInternal(enum OrbisSysModuleInternal module){
    assert(module==ORBIS_SYSMODULE_INTERNAL_BGFT);
    ++bgftModuleCalls;
    return bgftModuleResult;
}
extern "C" const char* sceKernelGetFsSandboxRandomWord(){return sandboxWord;}
extern "C" uint32_t sceKernelLoadStartModule(const char*path,size_t,const void*,uint32_t,void*,void*result){++nativeCalls;nativePath=path;*(int*)result=nativeStartResult;bool global=nativePath.find("/system/")==0;if((global&&globalMissing)||(!global&&sandboxMissing))return 0x80020002u;return (uint32_t)nativeResult;}
extern "C" int32_t sceUserServiceGetForegroundUser(int32_t*user){*user=foregroundUser;return foregroundResult;}
extern "C" int32_t sceUserServiceGetInitialUser(int32_t*user){++initialCalls;*user=initialUser;return initialResult;}
extern "C" int bgftInit(Init*p){
    ++initCalls;
    assert(moduleReady);
    assert(p->heap&&p->size==1024*1024&&((unsigned char*)p->heap)[0]==0);
    return initResult;
}
extern "C" int bgftTerm(){++termCalls;return 0;}
extern "C" int bgftRegister(ParamsEx*p,int*id){
 ++registerCalls;assert(*id==-1);assert(p->slot==0&&p->params.userId==expectedUser&&p->params.entitlementType==5);
 assert(std::string(p->params.url)=="/user/data/portal-store/downloads/7/base.pkg"&&std::string(p->params.type)==expectedType);
 assert(p->params.id&&std::string(p->params.id)=="IV0000-TEST00001_00-TESTPACKAGE00001");
 assert(p->params.option==0x10000&&p->params.size==12ULL*1024*1024*1024&&std::string(p->params.subtype).empty()&&std::string(p->params.scenario)=="0");
 *id=taskId;return registerResult;
}
extern "C" int bgftStart(int id){assert(id==42);++startCalls;return startResult;}
extern "C" int bgftResume(int id){assert(id==42);++resumeCalls;return resumeResult;}
extern "C" int bgftProgress(int id,Progress*p){assert(id==42);*p=returnedProgress;return progressResult;}
static void resetInit(){
    finalize();
    moduleReady=false;
    initCalls=termCalls=nativeCalls=bgftModuleCalls=0;
    bgftModuleResult=0;
    nativeResult=101;
    nativeStartResult=0;
    nativePath.clear();
    sandboxWord="testSandbox";
    globalMissing=sandboxMissing=false;
    mappedNames={"libSceLibcInternal"};
}
int main(){
 auto api=ps4InstallBackend();InstallPackage pkg;pkg.path="/data/portal-store/downloads/7/base.pkg";pkg.name="Base";pkg.contentId="IV0000-TEST00001_00-TESTPACKAGE00001";pkg.titleId="TEST00001";pkg.contentType=0x1a;pkg.bytes=12ULL*1024*1024*1024;
 std::string error;int id=123;
 assert(api.registerTask(pkg,id,error)&&id==42&&initCalls==1&&initialCalls==0);
 assert(bgftModuleCalls==1&&nativeCalls==0);
 for(auto type:{0x1a,0x1b,0x1c,0x1e}){pkg.contentType=type;expectedType=NullBGFT::packageType(type);assert(api.registerTask(pkg,id,error));}
 assert(initCalls==1);pkg.contentType=0x1a;expectedType="PS4GD";
 foregroundResult=-1;expectedUser=9;assert(api.registerTask(pkg,id,error)&&initialCalls==1);foregroundResult=0;foregroundUser=-1;assert(api.registerTask(pkg,id,error)&&initialCalls==2);foregroundUser=7;expectedUser=7;
 pkg.patch=true;int before=registerCalls;assert(api.registerTask(pkg,id,error)&&registerCalls==before+1);pkg.patch=false;
 pkg.titleId="DIFF00001";before=registerCalls;assert(!api.registerTask(pkg,id,error)&&id==-1&&registerCalls==before);pkg.titleId="TEST00001";
 registerResult=(int)0x80990088;assert(!api.registerTask(pkg,id,error)&&id==-1&&error.find("80990088")!=std::string::npos);registerResult=0;taskId=-1;assert(!api.registerTask(pkg,id,error)&&id==-1);taskId=42;
 assert(api.startTask(42,error)&&startCalls==1);assert(api.resumeTask(42,error)&&resumeCalls==1);resumeResult=-1;assert(api.resumeTask(42,error)&&startCalls==2);resumeResult=0;
 int percent=0;bool complete=false;returnedProgress.lengthTotal=4096;returnedProgress.transferredTotal=4096;returnedProgress.localCopy=40;
 returnedProgress.preparing=100;
 assert(api.pollTask(42,percent,complete,error)&&percent==40&&!complete);
 returnedProgress.localCopy=0;assert(api.pollTask(42,percent,complete,error)&&percent==-1&&!complete);
 returnedProgress.localCopy=-1;assert(api.pollTask(42,percent,complete,error)&&percent==-1&&!complete);
 returnedProgress.transferredTotal=1024;assert(api.pollTask(42,percent,complete,error)&&percent==25&&!complete);
 // Replay the console's first polls for a local base package. Its copy
 // counter reaches 100 before the transfer has even begun.
 returnedProgress={};
 returnedProgress.localCopy=100;
 assert(api.pollTask(42,percent,complete,error)&&percent==-1&&!complete);
 returnedProgress.lengthTotal=375980032;
 returnedProgress.transferredTotal=4653056;
 returnedProgress.preparing=100;
 assert(api.pollTask(42,percent,complete,error)&&percent==1&&!complete);
 returnedProgress.transferredTotal=187990016;
 returnedProgress.localCopy=90;
 assert(api.pollTask(42,percent,complete,error)&&percent==50&&!complete);
 returnedProgress.transferredTotal=returnedProgress.lengthTotal;
 returnedProgress.localCopy=100;
 returnedProgress.preparing=80;
 assert(api.pollTask(42,percent,complete,error)&&percent==-1&&!complete);
 returnedProgress.preparing=100;
 assert(api.pollTask(42,percent,complete,error)&&complete&&percent==100);
 returnedProgress.error=(int)0x80990001;assert(!api.pollTask(42,percent,complete,error)&&!complete);returnedProgress.error=0;
 resetInit();
 initResult=(int)0x80990050;
 assert(!api.registerTask(pkg,id,error)&&initCalls==1&&!heap&&error.find("80990050")!=std::string::npos);
 initResult=0;
 assert(api.registerTask(pkg,id,error)&&initCalls==2&&bgftModuleCalls==1);
 finalize();
 assert(!heap&&!bgftReady&&termCalls==1);
 finalize();
 assert(termCalls==1);

 resetInit();
 mappedNames={"/testSandbox/common/lib/libSceBgft.sprx"};
 assert(api.registerTask(pkg,id,error)&&bgftModuleCalls==0&&nativeCalls==0&&initCalls==1);

 resetInit();
 bgftModuleResult=0x80020008u;
 sandboxWord="anotherMount";
 globalMissing=true;
 assert(api.registerTask(pkg,id,error)&&nativeCalls==1&&nativePath=="/anotherMount/common/lib/libSceBgft.sprx");

 resetInit();
 bgftModuleResult=0x80020008u;
 sandboxWord=nullptr;
 assert(api.registerTask(pkg,id,error)&&nativeCalls==1&&nativePath=="/system/common/lib/libSceBgft.sprx");

 resetInit();
 bgftModuleResult=0x80020008u;
 sandboxMissing=true;
 assert(api.registerTask(pkg,id,error)&&nativeCalls==2&&nativePath=="/system/common/lib/libSceBgft.sprx");

 resetInit();
 bgftModuleResult=0x80020008u;
 sandboxMissing=globalMissing=true;
 assert(!api.registerTask(pkg,id,error)&&initCalls==0&&error.find("80020002")!=std::string::npos);

 resetInit();
 bgftModuleResult=0x80020008u;
 nativeResult=(int)0x8002000D;
 assert(!api.registerTask(pkg,id,error)&&initCalls==0&&nativeCalls==1&&!heap&&error.find("8002000D")!=std::string::npos);
 nativeResult=101;
 assert(api.registerTask(pkg,id,error));

 resetInit();
 bgftModuleResult=0x80020008u;
 nativeStartResult=-7;
 assert(!api.registerTask(pkg,id,error)&&initCalls==0);
 nativeStartResult=0;
 assert(api.registerTask(pkg,id,error));

 resetInit();
 std::cout<<"BGFT session setup, module fallback, task retries and progress passed.\n";
}
