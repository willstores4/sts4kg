#ifndef PORTAL_HOST
#define PORTAL_HOST
#endif
// Opaque test-only placeholders for APIs absent from macOS; never used by mocks.
typedef int pthread_barrier_t;typedef int pthread_barrierattr_t;
#include "../src/service_start_ps4.cpp"
#include <cassert>
#include <cstdarg>
#include <iostream>
static long sdkVersion=-1;static int sdkEnters=0,sdkRestores=0,compatEnters=0,compatRestores=0;static bool missing=false;static int compatCode=0;
static int enter(){++compatEnters;return compatCode;}static int leave(){++compatRestores;return 0;}
extern "C" long portalSystemCall(long n,...){va_list ap;va_start(ap,n);int cmd=va_arg(ap,int);va_end(ap);assert(n==500);if(cmd==0)return sdkVersion;if(cmd==2){++sdkEnters;return 0;}if(cmd==3){++sdkRestores;return 0;}return -1;}
extern "C" uint32_t sceKernelLoadStartModule(const char*,size_t,const void*,uint32_t,void*,void*){return missing?(uint32_t)-13:1;}
extern "C" int32_t sceKernelDlsym(int32_t,const char*name,void**address){*address=!strcmp(name,"jailbreak_me")?(void*)enter:(void*)leave;return 0;}
extern "C" int32_t sceKernelUsleep(uint32_t){return 0;}
extern "C" int32_t sceLncUtilLaunchApp(const char*,const char*[],LncAppParam*){return -1;}
extern "C" int serviceAppId(const char*){return -1;}
extern "C" int serviceKillApp(uint32_t){assert(false);return -1;}
int main(){std::string error;JailbreakBackup b={};assert(grant(b,error)&&b.compatibility);assert(compatEnters==1&&sdkEnters==0);assert(restore(b,error)&&compatRestores==1&&sdkRestores==0);sdkVersion=0x100;b={};assert(grant(b,error)&&!b.compatibility);assert(restore(b,error));assert(sdkEnters==1&&sdkRestores==1);sdkVersion=-1;compatEnter=compatLeave=0;missing=true;b={};assert(!grant(b,error)&&error.find("SDK probe")!=std::string::npos);missing=false;compatCode=37;assert(!grant(b,error)&&error.find("00000025")!=std::string::npos);std::cout<<"Missing SDK fallback, matching restore path, helper errors and SDK path passed with mocked platform calls.\n";}
