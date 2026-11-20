#include "network_ps4.h"
#include "data_root.h"
#include "catalog.h"
#include "service_log.h"
#include <pthread.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include "../build/trusted_roots.h"
extern "C" int recvTimeout(int,unsigned) __asm__("sceHttpSetRecvTimeOut");
extern "C" int sslTerminate(int) __asm__("sceSslTerm");
extern "C" int autoRedirect(int,int) __asm__("sceHttpSetAutoRedirect");
extern "C" int httpsError(int,int*,uint32_t*) __asm__("sceHttpsGetSslError");
static int pool=-1,ssl=-1,http=-1,caLoadResult=-1;static pthread_mutex_t initMutex=PTHREAD_MUTEX_INITIALIZER;
void portalNetworkTrace(const char*format,...){char message[768];va_list args;va_start(args,format);vsnprintf(message,sizeof message,format,args);va_end(args);
#if defined(PORTAL_SERVICE_DAEMON) && !defined(PORTAL_HOST)
 SERVICE_LOG("network-fetch: %s",message);
#elif !defined(PORTAL_HOST)
 char line[896];int n=snprintf(line,sizeof line,"[NETWORK 1.0][pid %d][%lld] %s\n",getpid(),(long long)time(0),message);if(n<=0)return;size_t size=(size_t)n<sizeof line?(size_t)n:sizeof line-1;const char*root=nullStoreDataRoot();char path[128];snprintf(path,sizeof path,"%s/network.log",root);int fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0666);if(fd<0){snprintf(path,sizeof path,"/user%s/network.log",root);fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0666);}if(fd>=0){size_t offset=0;while(offset<size){ssize_t wrote=write(fd,line+offset,size-offset);if(wrote<=0)break;offset+=(size_t)wrote;}close(fd);}
#endif
}
bool portalNetworkInit(){
 int lock=pthread_mutex_lock(&initMutex);if(lock){portalNetworkTrace("initialization mutex failed=%d",lock);return false;}
 if(http>=0){pthread_mutex_unlock(&initMutex);return true;}
 uint32_t netModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
 uint32_t sslModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
 uint32_t httpModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
 int netResult=sceNetInit(),ctlResult=sceNetCtlInit();
 portalNetworkTrace("init modules net=0x%08X ssl=0x%08X http=0x%08X; netInit=0x%08X ctlInit=0x%08X",netModule,sslModule,httpModule,(unsigned)netResult,(unsigned)ctlResult);
 errno=0;pool=sceNetPoolCreate("NULL_storeCatalog",1024*1024,0);int poolError=errno;
 if(pool>=0)ssl=sceSslInit(2*1024*1024);
 if(ssl>=0)http=sceHttpInit(pool,ssl,2*1024*1024);
 bool ok=false;
 if(http>=0){
  // The elevated daemon does not inherit the title's default CA store.
  // These embedded PEM records and pointer arrays outlive the HTTP context.
  caLoadResult=sceHttpsLoadCert(http,(int32_t)portalTrustCount,portalTrustRoots,0,0);
  portalNetworkTrace("HTTPS trust roots=%zu load=0x%08X sha256=%s",portalTrustCount,(unsigned)caLoadResult,portalTrustSHA256);
  ok=caLoadResult>=0;
 }
 portalNetworkTrace("init complete=%d pool=%d pool_errno=%d ssl=%d http=%d",ok?1:0,pool,poolError,ssl,http);
 if(!ok){if(http>=0)sceHttpTerm(http);if(ssl>=0)sslTerminate(ssl);if(pool>=0)sceNetPoolDestroy(pool);http=ssl=pool=-1;}
 pthread_mutex_unlock(&initMutex);return ok;
}
static bool networkURL(const std::string&url){if(url.size()>2300||url.find_first_of(" \t\r\n")!=std::string::npos)return false;size_t p=url.compare(0,8,"https://")==0?8:url.compare(0,7,"http://")==0?7:0;if(!p)return false;auto end=url.find_first_of("/?#",p);auto host=url.substr(p,end-p);return !host.empty()&&host.find('@')==std::string::npos;}
static std::string networkHost(const std::string&url){size_t p=url.compare(0,8,"https://")==0?8:url.compare(0,7,"http://")==0?7:0;if(!p)return "invalid";auto end=url.find_first_of("/?#",p);return url.substr(p,end-p);}
static std::string redirectURL(const std::string&base,std::string path){while(!path.empty()&&(path.back()=='\r'||path.back()==' '))path.pop_back();if(path.compare(0,2,"//")==0)return base.substr(0,base.find(':'))+":"+path;if(path.find("://")!=std::string::npos)return path;auto scheme=base.find("://");auto end=base.find('/',scheme+3);auto origin=base.substr(0,end);if(!path.empty()&&path[0]=='/')return origin+path;return base.substr(0,base.find_last_of('/')+1)+path;}
bool portalNetworkFetch(const std::string&initial,std::string&out,size_t cap,const std::string&referer){
 out.clear();
 if(!portalNetworkInit()){portalNetworkTrace("GET failed host=%s reason=network_init",networkHost(initial).c_str());return false;}
 std::string url=initial;
 for(int hop=0;hop<6;hop++){
  if(!networkURL(url)){portalNetworkTrace("GET failed host=%s hop=%d reason=invalid_url",networkHost(url).c_str(),hop);return false;}
  int t=sceHttpCreateTemplate(http,"NULL_store/1.0",ORBIS_HTTP_VERSION_1_1,1);
  if(t<0){portalNetworkTrace("GET failed host=%s hop=%d reason=create_template result=%d",networkHost(url).c_str(),hop,t);return false;}
  int co=-1,r=-1,code=0,sendResult=-1,readFailure=0;bool ok=false;std::string location,failure="create_connection";
  autoRedirect(t,0);sceHttpSetResolveTimeOut(t,8000000);sceHttpSetConnectTimeOut(t,8000000);sceHttpSetSendTimeOut(t,8000000);recvTimeout(t,10000000);
  // The native keepalive path retains the explicitly loaded trust context.
  // Each request/hop still deletes its connection below.
  co=sceHttpCreateConnectionWithURL(t,url.c_str(),1);
  if(co>=0){
   failure="create_request";
   r=sceHttpCreateRequestWithURL(co,ORBIS_METHOD_GET,url.c_str(),0);
   if(r>=0){
    failure="send_request";
    sceHttpAddRequestHeader(r,"Accept-Encoding","identity",0);
    if(sourceURL(url)){std::string ref=sourceForURL(referer)==sourceForURL(url)?referer:sourceOrigin(sourceForURL(url))+"/";sceHttpAddRequestHeader(r,"Referer",ref.c_str(),0);}
    sendResult=sceHttpSendRequest(r,0,0);
    if(sendResult<0){
     int nativeErrno=0,sslCode=0;uint32_t verify=0;int sslQuery=-1;
     int errnoQuery=sceHttpGetLastErrno(r,&nativeErrno);
     if(((uint32_t)sendResult&0xffff0000u)==0x80950000u)sslQuery=httpsError(r,&sslCode,&verify);
     portalNetworkTrace("request failure host=%s send=0x%08X sslQuery=0x%08X ssl=0x%08X verify=0x%08X errnoQuery=0x%08X errno=%d roots=%zu caLoad=0x%08X keepalive=1",networkHost(url).c_str(),(unsigned)sendResult,(unsigned)sslQuery,(unsigned)sslCode,verify,(unsigned)errnoQuery,nativeErrno,portalTrustCount,(unsigned)caLoadResult);
    }
    if(sendResult>=0){
     failure="get_status";
     int statusResult=sceHttpGetStatusCode(r,&code);
     if(statusResult>=0&&code==200){
      failure="read_response";
      char buffer[32768];
      for(;;){int n=sceHttpReadData(r,buffer,sizeof buffer);if(n==0){ok=true;failure="";break;}if(n<0){readFailure=n;break;}if(out.size()+(size_t)n>cap){readFailure=-2;failure="response_too_large";break;}out.append(buffer,(size_t)n);}
     }else if(statusResult>=0&&(code==301||code==302||code==303||code==307||code==308)){
      failure="redirect_without_location";
      char*headers=0;size_t size=0;
      if(sceHttpGetAllResponseHeaders(r,&headers,&size)>=0&&headers){
       std::string all(headers,size),normalized=lower(all);size_t p=normalized.find("\nlocation:");
       if(p==std::string::npos&&normalized.compare(0,9,"location:")==0)p=0;else if(p!=std::string::npos)++p;
       if(p!=std::string::npos){p+=9;while(p<all.size()&&(all[p]==' '||all[p]=='\t'))++p;location=all.substr(p,all.find('\n',p)-p);failure="redirect";}
      }
     }else if(statusResult>=0)failure="http_status";
    }
   }
  }
  if(r>=0)sceHttpDeleteRequest(r);if(co>=0)sceHttpDeleteConnection(co);sceHttpDeleteTemplate(t);
  if(ok){portalNetworkTrace("GET ok host=%s hop=%d status=%d bytes=%zu",networkHost(url).c_str(),hop,code,out.size());return true;}
  out.clear();if(!location.empty()){portalNetworkTrace("GET redirect host=%s hop=%d status=%d",networkHost(url).c_str(),hop,code);url=redirectURL(url,location);continue;}
  portalNetworkTrace("GET failed host=%s hop=%d reason=%s status=%d send=%d read=%d",networkHost(url).c_str(),hop,failure.c_str(),code,sendResult,readFailure);return false;
 }
 portalNetworkTrace("GET failed host=%s reason=redirect_limit",networkHost(url).c_str());return false;
}
