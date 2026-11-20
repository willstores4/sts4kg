#pragma once
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* The shipped OpenOrbis header makes mode_t 32 bits, shifting st_size to 0x50.
   libkernel writes the native 16-bit-mode layout, whose size is at 0x48. Keep
   raw kernel storage separate, then assign fields into the caller's structure.
   Do not interpose libc++ or change the SDK's public types globally. */
typedef struct {int64_t sec,nsec;} PortalKernelFileTime;
typedef struct {
 uint32_t dev,ino;uint16_t mode,links;uint32_t uid,gid,rdev;
 PortalKernelFileTime accessTime,modifyTime,changeTime;
 int64_t byteSize,blocks;uint32_t blockSize,flags,generation;int32_t spare;
 PortalKernelFileTime birthTime;
} PortalKernelFileInfo;
#ifdef __cplusplus
static_assert(offsetof(PortalKernelFileInfo,byteSize)==0x48&&sizeof(PortalKernelFileInfo)==120,"PS4 file-info ABI");
#else
_Static_assert(offsetof(PortalKernelFileInfo,byteSize)==0x48&&sizeof(PortalKernelFileInfo)==120,"PS4 file-info ABI");
#endif
static inline void portalDecodeFileInfo(const PortalKernelFileInfo*raw,struct stat*out){
 memset(out,0,sizeof *out);out->st_dev=raw->dev;out->st_ino=raw->ino;out->st_mode=raw->mode;out->st_nlink=raw->links;out->st_uid=raw->uid;out->st_gid=raw->gid;out->st_rdev=raw->rdev;
 out->st_size=raw->byteSize;out->st_blocks=raw->blocks;out->st_blksize=raw->blockSize;
#ifndef PORTAL_HOST
 out->st_atim.tv_sec=raw->accessTime.sec;out->st_atim.tv_nsec=raw->accessTime.nsec;out->st_mtim.tv_sec=raw->modifyTime.sec;out->st_mtim.tv_nsec=raw->modifyTime.nsec;out->st_ctim.tv_sec=raw->changeTime.sec;out->st_ctim.tv_nsec=raw->changeTime.nsec;out->st_birthtim.tv_sec=raw->birthTime.sec;out->st_birthtim.tv_nsec=raw->birthTime.nsec;out->st_flags=raw->flags;out->st_gen=raw->generation;
#endif
}
static inline int portalDescriptorInfo(int fd,struct stat*out){
 if(!out){errno=EFAULT;return -1;}
#ifdef PORTAL_HOST
 return fstat(fd,out);
#else
 uint64_t storage[32]={0};int result=fstat(fd,(struct stat*)(void*)storage);
 if(!result){PortalKernelFileInfo raw;memcpy(&raw,storage,sizeof raw);portalDecodeFileInfo(&raw,out);}else if(((uint32_t)result>>16)==0x8002){errno=(uint32_t)result&0xffff;return -1;}return result;
#endif
}
/* OpenOrbis libc lstat routes through a permanently-ENOSYS fstatat stub.
   Descriptor lookup avoids that stub and never follows the final symlink. */
static inline int portalPathInfo(const char*path,struct stat*out){
#if defined(PORTAL_HOST) && !defined(PORTAL_DESCRIPTOR_STAT)
 return lstat(path,out);
#else
 int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;
 int result=portalDescriptorInfo(fd,out),saved=errno;close(fd);errno=saved;return result;
#endif
}
