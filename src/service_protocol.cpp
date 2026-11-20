#include "service_protocol.h"
#include "net_compat.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
namespace PortalIPC {
static bool transfer(int fd,void*buf,size_t n,bool sending){char*p=(char*)buf;while(n){ssize_t k;
#ifdef MSG_NOSIGNAL
k=sending?send(fd,p,n,MSG_NOSIGNAL):recv(fd,p,n,0);
#else
k=sending?send(fd,p,n,0):recv(fd,p,n,0);
#endif
if(k<0&&errno==EINTR)continue;if(k<=0)return false;p+=k;n-=k;}return true;}
bool sendFrame(int fd,const std::string&s){if(s.size()>MAX_FRAME)return false;Writer h;h.u32(s.size());return transfer(fd,&h.bytes[0],4,true)&&(s.empty()||transfer(fd,(void*)s.data(),s.size(),true));}
bool receiveFrame(int fd,std::string&s){std::string head(4,0);if(!transfer(fd,&head[0],4,false))return false;Reader r(head);unsigned n=r.u32();if(n>MAX_FRAME||n<4)return false;s.assign(n,0);return transfer(fd,&s[0],n,false);}
bool request(const std::string&message,std::string&response,std::string&error,int port){int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){error="Cannot open service connection";return false;}int flags=fcntl(fd,F_GETFL,0);fcntl(fd,F_SETFL,flags|O_NONBLOCK);sockaddr_in to={};portalAddress4(&to,htonl(INADDR_LOOPBACK),port);int result=connect(fd,(sockaddr*)&to,sizeof to);if(result<0&&errno==EINPROGRESS){fd_set set;FD_ZERO(&set);FD_SET(fd,&set);timeval timeout={0,300000};result=select(fd+1,0,&set,0,&timeout);int code=0;socklen_t len=sizeof code;if(result<=0||getsockopt(fd,SOL_SOCKET,SO_ERROR,&code,&len)||code)result=-1;else result=0;}if(result<0){close(fd);error="Background service is not running";return false;}fcntl(fd,F_SETFL,flags);timeval timeout={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
#ifdef SO_NOSIGPIPE
int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
#endif
bool ok=sendFrame(fd,message)&&receiveFrame(fd,response);close(fd);if(!ok){error="Background service connection was interrupted";return false;}try{Reader r(response);if(r.u32()!=0){error=r.string();return false;}response=response.substr(r.pos);}catch(const std::exception&e){error=e.what();return false;}return true;}
}
