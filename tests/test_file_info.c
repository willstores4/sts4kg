#define PORTAL_HOST
#include "../src/path_info.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 /* Native kernel response: 0x50 is allocated blocks, not byte size. */
 PortalKernelFileInfo raw={0};raw.dev=19;raw.ino=123;raw.mode=S_IFREG|0644;raw.links=2;raw.uid=1;raw.gid=0;
 raw.byteSize=3608805376LL;raw.blocks=7048448;raw.blockSize=32768;
 struct stat out;portalDecodeFileInfo(&raw,&out);
 assert(S_ISREG(out.st_mode)&&out.st_nlink==2&&out.st_uid==1&&out.st_size==3608805376LL&&out.st_blocks==7048448);
 raw.byteSize=2321219584LL;portalDecodeFileInfo(&raw,&out);assert(out.st_size==2321219584LL);
 raw.byteSize=12LL*1024*1024*1024;portalDecodeFileInfo(&raw,&out);assert(out.st_size==12LL*1024*1024*1024);
 puts("Native PS4 stat layout, mode/link decoding and 64-bit game sizes passed.");return 0;
}
