#pragma once
#include "ui.h"
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
struct Display{int video=-1;void*mem=0;off_t offset=0;void*buffers[2];int active=0;OrbisKernelEqueue queue;
    bool init(){video=sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,ORBIS_VIDEO_OUT_BUS_MAIN,0,0);if(video<0)return false;size_t n=32*1024*1024;if(sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),n,0x200000,3,&offset)<0)return false;if(sceKernelMapDirectMemory(&mem,n,0x33,0,offset,0x200000)<0)return false;buffers[0]=mem;buffers[1]=(char*)mem+1920*1080*4;OrbisVideoOutBufferAttribute attr;sceVideoOutSetBufferAttribute(&attr,0x80000000,1,0,1920,1080,1920);if(sceVideoOutRegisterBuffers(video,0,buffers,2,&attr)<0)return false;sceKernelCreateEqueue(&queue,"NULL_storeFlip");sceVideoOutAddFlipEvent(queue,video,0);sceVideoOutSetFlipRate(video,0);return true;}
    void flip(Canvas&c,int frame){memcpy(buffers[active],c.pixels.data(),c.pixels.size()*4);sceVideoOutSubmitFlip(video,active,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame);for(;;){OrbisVideoOutFlipStatus s;sceVideoOutGetFlipStatus(video,&s);if(s.flipArg==frame)break;OrbisKernelEvent e;int n;if(sceKernelWaitEqueue(queue,&e,1,&n,0))break;}active^=1;}
};
