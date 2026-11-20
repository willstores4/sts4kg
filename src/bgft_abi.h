#pragma once
#include <cstdint>
#include <cstddef>
// Native LP64 layouts and registration values documented by flatz's RPI and
// ps4_stub_lib_maker_v2/include/bgft.h. OpenOrbis narrows some of these fields.
namespace NullBGFT {
struct Init {void*heap;size_t size;};
static_assert(sizeof(Init)==16&&offsetof(Init,size)==8,"BGFT initialization ABI");
struct Params {int32_t userId,entitlementType;const char *id,*url,*exUrl,*name,*icon,*sku;uint32_t option;const char *scenario,*date,*type,*subtype;uint64_t size;};
struct ParamsEx {Params params;uint32_t slot;};
struct Progress {uint32_t bits;int32_t error;uint64_t length,transferred,lengthTotal,transferredTotal;uint32_t index,total,seconds,secondsTotal;int32_t preparing,localCopy;};
static_assert(sizeof(Params)==104&&offsetof(ParamsEx,slot)==104&&sizeof(ParamsEx)==112,"BGFT parameter ABI");
static_assert(sizeof(Progress)==64&&offsetof(Progress,transferred)==16&&offsetof(Progress,localCopy)==60,"BGFT progress ABI");
const uint32_t DisableCDNQuery=0x10000;
inline const char*packageType(uint32_t type){switch(type){case 0x1a:return "PS4GD";case 0x1b:return "PS4AC";case 0x1c:return "PS4AL";case 0x1e:return "PS4DP";default:return nullptr;}}
inline bool isPatch(uint32_t type,uint32_t flags){return type==0x1e||(flags&0x61100000u)!=0;}
}
