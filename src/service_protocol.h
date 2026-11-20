#pragma once
#include "download_model.h"
#include <stdint.h>
#include <cstring>
#include <stdexcept>
namespace PortalIPC {
static const int PORT=17941;static const unsigned VERSION=8,MAX_FRAME=16*1024*1024;
enum Command {PING=1,QUEUE=2,DISCOVER=3,SOURCES=4,ENQUEUE=5,PAUSE=6,RESUME=7,CANCEL=8,STOP=9,REMOVE=10,REMOVE_DELETE=11};
struct Writer {std::string bytes;void u32(uint32_t v){for(int i=3;i>=0;--i)bytes+=char(v>>(i*8));}void u64(uint64_t v){u32(v>>32);u32(v);}void string(const std::string&s){if(s.size()>MAX_FRAME)throw std::runtime_error("IPC field too large");u32(s.size());bytes+=s;}};
struct Reader {const std::string&bytes;size_t pos=0;Reader(const std::string&s):bytes(s){}uint32_t u32(){if(bytes.size()-pos<4)throw std::runtime_error("Incomplete IPC integer");uint32_t v=0;for(int i=0;i<4;++i)v=v<<8|(unsigned char)bytes[pos++];return v;}uint64_t u64(){uint64_t hi=u32();return hi<<32|u32();}std::string string(){size_t n=u32();if(n>MAX_FRAME||n>bytes.size()-pos)throw std::runtime_error("Incomplete IPC string");auto s=bytes.substr(pos,n);pos+=n;return s;}void end(){if(pos!=bytes.size())throw std::runtime_error("Unexpected IPC data");}};
inline void game(Writer&w,const Game&g){w.u32(g.id);for(auto&s:{g.title,g.year,g.size,g.url,g.image})w.string(s);}
inline Game game(Reader&r){Game g;g.id=r.u32();g.title=r.string();g.year=r.string();g.size=r.string();g.url=r.string();g.image=r.string();if(g.id<=0||g.title.empty()||!sourceURL(g.url))throw std::runtime_error("Invalid game");return g;}
inline void row(Writer&w,const DownloadRow&j){w.u32(j.id);w.u32(j.gameId);w.u32((unsigned)j.state);for(auto&s:{j.title,j.url,j.page,j.phase,j.error,j.outputDir})w.string(s);w.u64(j.done);w.u64(j.total);w.u64(j.rate>0?uint64_t(j.rate):0);w.u32(j.peers);}
inline DownloadRow row(Reader&r){DownloadRow j;j.id=r.u32();j.gameId=r.u32();auto state=r.u32();if(state>6)throw std::runtime_error("Invalid download state");j.state=DownloadState(state);j.title=r.string();j.url=r.string();j.page=r.string();j.phase=r.string();j.error=r.string();j.outputDir=r.string();j.done=r.u64();j.total=r.u64();j.rate=r.u64();j.peers=r.u32();return j;}
bool sendFrame(int,const std::string&);bool receiveFrame(int,std::string&);bool request(const std::string&,std::string&,std::string&,int port=PORT);
}
