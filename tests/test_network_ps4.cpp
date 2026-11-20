#define PORTAL_HOST
#ifndef NULLSTORE_NETWORK_SOURCE
#define NULLSTORE_NETWORK_SOURCE "../src/network_ps4.cpp"
#endif
#include NULLSTORE_NETWORK_SOURCE
#include <cassert>
#include <cstring>
#include <iostream>

// Model native requests rejecting HTTPS when the context has no root CAs.
// This exercises the actual PS4 fetch implementation, not desktop TLS.
static SslPem** loadedRoots=0;
static int rootCount=0,loadCalls=0,loadResult=0,sendFailure=0,sslQueries=0;
static int termCalls=0,sslTermCalls=0,poolTermCalls=0;
static bool keepalive=false,bodySent=false;
static void checkRoots(){
 assert(loadedRoots&&rootCount==118);
 for(int i=0;i<rootCount;i++){
  auto* record=loadedRoots[i];assert(record&&record->ptr&&record->size>100&&record->size<16384);
  std::string pem(record->ptr,record->size);
  assert(pem.find("-----BEGIN CERTIFICATE-----\r\n")==0);
  assert(pem.substr(pem.size()-27)=="-----END CERTIFICATE-----\r\n");
  assert(record->ptr[record->size]==0);
  for(size_t p=0;p<pem.size();p++)if(pem[p]=='\n')assert(p>0&&pem[p-1]=='\r');
 }
}
extern "C" uint32_t sceSysmoduleLoadModuleInternal(enum OrbisSysModuleInternal){return 0;}
extern "C" int32_t sceNetInit(){return 0;}
extern "C" int32_t sceNetCtlInit(){return 0;}
extern "C" int32_t sceNetPoolCreate(const char*,int32_t,int32_t){return 1;}
extern "C" void sceNetPoolDestroy(int32_t id){assert(id==1);++poolTermCalls;}
extern "C" int32_t sceSslInit(size_t){return 2;}
extern "C" int sslTerminate(int id){assert(id==2);++sslTermCalls;return 0;}
extern "C" int32_t sceHttpInit(int32_t p,int32_t s,size_t){assert(p==1&&s==2);return 3;}
extern "C" int32_t sceHttpTerm(int32_t id){assert(id==3);loadedRoots=0;rootCount=0;++termCalls;return 0;}
extern "C" int32_t sceHttpsLoadCert(int32_t id,int32_t count,void* roots,void* cert,void* key){
 assert(id==3&&!cert&&!key);++loadCalls;
 if(loadResult<0)return loadResult;
 loadedRoots=static_cast<SslPem**>(roots);rootCount=count;checkRoots();return 0;
}
extern "C" int32_t sceHttpCreateTemplate(int32_t id,const char*,int32_t,int32_t){assert(id==3);return 4;}
extern "C" int32_t sceHttpDeleteTemplate(int32_t id){assert(id==4);return 0;}
extern "C" int autoRedirect(int id,int enabled){assert(id==4&&!enabled);return 0;}
extern "C" int32_t sceHttpSetResolveTimeOut(int32_t,uint32_t){return 0;}
extern "C" int32_t sceHttpSetConnectTimeOut(int32_t,uint32_t){return 0;}
extern "C" int32_t sceHttpSetSendTimeOut(int32_t,uint32_t){return 0;}
extern "C" int recvTimeout(int,unsigned){return 0;}
extern "C" int32_t sceHttpCreateConnectionWithURL(int32_t id,const char*,bool value){assert(id==4);keepalive=value;return 5;}
extern "C" int32_t sceHttpDeleteConnection(int32_t id){assert(id==5);return 0;}
extern "C" int32_t sceHttpCreateRequestWithURL(int32_t id,int32_t method,const char*,uint64_t){assert(id==5&&method==ORBIS_METHOD_GET);bodySent=false;return 6;}
extern "C" int32_t sceHttpDeleteRequest(int32_t id){assert(id==6);return 0;}
extern "C" int32_t sceHttpAddRequestHeader(int32_t,const char*,const char*,int32_t){return 0;}
extern "C" int32_t sceHttpSendRequest(int32_t id,const void*,size_t){
 assert(id==6);
 if(!loadedRoots||!keepalive)return (int32_t)0x8095f00c;
 checkRoots();return sendFailure;
}
extern "C" int32_t sceHttpGetLastErrno(int32_t,int32_t* number){*number=0;return 0;}
extern "C" int httpsError(int,int* number,uint32_t* flags){++sslQueries;*number=(int)0x8095f00a;*flags=0x20;return 0;}
extern "C" int32_t sceHttpGetStatusCode(int32_t,int32_t* code){*code=200;return 0;}
extern "C" int32_t sceHttpGetAllResponseHeaders(int32_t,char**,size_t*){return -1;}
extern "C" int32_t sceHttpReadData(int32_t,void* data,uint32_t){if(bodySent)return 0;checkRoots();memcpy(data,"page",4);bodySent=true;return 4;}
static void resetNetwork(){if(http>=0)sceHttpTerm(http);if(ssl>=0)sslTerminate(ssl);if(pool>=0)sceNetPoolDestroy(pool);http=ssl=pool=-1;}
int main(){
 std::string page;
 assert(portalNetworkFetch("https://ps4torrent.net/test/",page,1024,"https://ps4torrent.net/")&&page=="page");
 assert(keepalive&&loadCalls==1);auto** retained=loadedRoots;
 assert(portalNetworkFetch("https://gametorrent.org/test/",page,1024,"https://gametorrent.org/")&&page=="page");
 assert(loadCalls==1&&loadedRoots==retained);checkRoots();
 sendFailure=(int)0x8095f00c;
 assert(!portalNetworkFetch("https://love-games1.net/test/",page,1024,"")&&page.empty()&&sslQueries==1);
 sendFailure=0;resetNetwork();loadResult=-1;
 int terms=termCalls,sslTerms=sslTermCalls,poolTerms=poolTermCalls;
 assert(!portalNetworkInit()&&http<0&&ssl<0&&pool<0);
 assert(termCalls==terms+1&&sslTermCalls==sslTerms+1&&poolTermCalls==poolTerms+1);
 loadResult=0;assert(portalNetworkInit());checkRoots();resetNetwork();
 std::cout<<"HTTPS trust registration/lifetime, keepalive, certificate rejection and init retry passed.\n";
}
