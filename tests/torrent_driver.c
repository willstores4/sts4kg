#define _POSIX_C_SOURCE 200809L
#include "../src/torrent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
typedef struct {const char*tracker;uint64_t done,stop;char error[256];uint64_t started_ms,stop_ms;} State;
static uint64_t clock_ms(void){struct timeval tv;gettimeofday(&tv,NULL);return (uint64_t)tv.tv_sec*1000+tv.tv_usec/1000;}
static unsigned char* http(void*p,const char*url,size_t*n){State*s=p;*n=0;if(!strstr(url,"info_hash=")||!strstr(url,"compact=1"))return NULL;FILE*f=fopen(s->tracker,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);unsigned char*b=malloc(size);*n=fread(b,1,size,f);fclose(f);return b;}
static int stopped(void*p){State*s=p;return (s->stop&&s->done>=s->stop)||(s->stop_ms&&clock_ms()-s->started_ms>=s->stop_ms);}
static void progress(void*p,const char*phase,uint64_t done,uint64_t total,unsigned peers){State*s=p;s->done=done;if(getenv("PORTAL_TORRENT_PHASE_LOG"))fprintf(stderr,"phase=%s\n",phase);(void)total;(void)peers;}
static void error(void*p,const char*why){State*s=p;snprintf(s->error,sizeof s->error,"%s",why);}
int main(int argc,char**argv){if(argc<4)return 3;State state={argv[3],0,argc>4?strtoull(argv[4],0,10):0,"",clock_ms(),argc>5?strtoull(argv[5],0,10):0};torrent_callbacks cb={&state,http,stopped,progress,error};int r=torrent_run(argv[1],argv[2],&cb);printf("result=%d bytes=%llu error=%s\n",r,(unsigned long long)state.done,state.error);return r==0?0:r==1?2:1;}
