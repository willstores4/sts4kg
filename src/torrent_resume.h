/* Resume indexes are hints, never proof of a verified piece. All saved complete
   pieces are SHA-1 checked on reopen; partial blocks have their own checksums and
   still pass the torrent's whole-piece hash before becoming available to peers.
   Sidecars live next to the metainfo, outside the torrent's file namespace. */
#define RESUME_BLOCKS 4096u
#define RESUME_PEERS 128u
#define RESUME_HEADER 72u
typedef struct { uint32_t piece, block; uint8_t hash[20]; } resume_block;
typedef struct {
    uint32_t ip, seen, retry;
    uint16_t port;
    uint8_t fails, worked, banned;
} resume_peer;
struct resume_state {
    char path[1100];
    uint8_t *indexed;
    resume_block *blocks;
    uint32_t nblocks, npeers;
    resume_peer peers[RESUME_PEERS];
    int valid, first_piece_saved, warned;
    int64_t save_at;
};
static uint32_t resume_u32(const uint8_t*p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void resume_put32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static uint64_t resume_u64(const uint8_t*p){return (uint64_t)resume_u32(p)<<32|resume_u32(p+4);}
static void resume_put64(uint8_t*p,uint64_t v){resume_put32(p,(uint32_t)(v>>32));resume_put32(p+4,(uint32_t)v);}
static void resume_free(resume_state*r){if(r){free(r->indexed);free(r->blocks);free(r);}}
static int resume_open_read(const char*path){
    int flags=O_RDONLY;
#ifdef O_NOFOLLOW
    flags|=O_NOFOLLOW;
#endif
    return open(path,flags);
}
static void resume_init(torrent_t*t,const char*metainfo){
    resume_state*r=calloc(1,sizeof *r);if(!r)return;t->resume=r;
    int k=snprintf(r->path,sizeof r->path,"%s.resume",metainfo);
    if(k<0||(size_t)k>=sizeof r->path){r->path[0]=0;LOG("Resume disabled: metainfo path is too long");return;}
    r->indexed=calloc(t->npieces,1);r->blocks=calloc(RESUME_BLOCKS,sizeof *r->blocks);
    if(!r->indexed||!r->blocks){LOG("Resume disabled: out of memory");r->path[0]=0;return;}
    int fd=resume_open_read(r->path);if(fd<0){LOG("Resume index unavailable (%s); full file scan",strerror(errno));return;}
    struct stat st;size_t cap=RESUME_HEADER+t->npieces+RESUME_BLOCKS*28+RESUME_PEERS*20+20;
    uint8_t*data=NULL;size_t n=0,done=0;
    if(portalDescriptorInfo(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<(off_t)(RESUME_HEADER+t->npieces+20)||(uint64_t)st.st_size>cap)goto bad;
    n=(size_t)st.st_size;data=malloc(n);if(!data)goto bad;
    while(done<n){ssize_t z=read(fd,data+done,n-done);if(z<0&&errno==EINTR)continue;if(z<=0)goto bad;done+=(size_t)z;}
    {uint8_t hash[20],binding[20];sha1(data,n-20,hash);sha1((const uint8_t*)output_dir,strlen(output_dir),binding);
     if(memcmp(data,"NULLRS02",8)||memcmp(data+8,t->ihash,20)||resume_u32(data+28)!=t->plen||resume_u32(data+32)!=t->npieces||resume_u64(data+36)!=t->total||memcmp(data+52,binding,20)||memcmp(hash,data+n-20,20))goto bad;}
    {uint32_t nb=resume_u32(data+44),np=resume_u32(data+48);
     if(nb>RESUME_BLOCKS||np>RESUME_PEERS||n!=RESUME_HEADER+t->npieces+(size_t)nb*28+(size_t)np*20+20)goto bad;
     const uint8_t*p=data+RESUME_HEADER;
     for(uint32_t i=0;i<t->npieces;i++)if(p[i]>1)goto bad;
     memcpy(r->indexed,p,t->npieces);p+=t->npieces;
     unsigned pieces=0,blocks_in_piece=0;uint32_t previous=UINT32_MAX,prevblock=0;
     unsigned maxpieces=16u*1024*1024/t->plen;if(maxpieces<1)maxpieces=1;if(maxpieces>64)maxpieces=64;
     for(uint32_t i=0;i<nb;i++,p+=28){resume_block*b=&r->blocks[i];b->piece=resume_u32(p);b->block=resume_u32(p+4);memcpy(b->hash,p+8,20);
         if(b->piece>=t->npieces||r->indexed[b->piece]||b->block>=(piece_len(t,b->piece)+BLOCK-1)/BLOCK)goto bad;
         if(i&&(b->piece<previous||(b->piece==previous&&b->block<=prevblock)))goto bad;
         if(b->piece!=previous){if(++pieces>maxpieces)goto bad;blocks_in_piece=0;}
         if(++blocks_in_piece>=(piece_len(t,b->piece)+BLOCK-1)/BLOCK)goto bad;
         previous=b->piece;prevblock=b->block;}
     for(uint32_t i=0;i<np;i++,p+=20){resume_peer*v=&r->peers[i];memcpy(&v->ip,p,4);v->port=(uint16_t)((unsigned)p[4]<<8|p[5]);v->fails=p[6];v->worked=p[7];v->seen=resume_u32(p+8);v->retry=resume_u32(p+12);v->banned=p[16];if(v->worked>1||v->banned>1)goto bad;}
     r->nblocks=nb;r->npeers=np;}
    r->valid=1;free(data);close(fd);
    {unsigned saved=0;for(uint32_t i=0;i<t->npieces;i++)saved+=r->indexed[i];LOG("Resume index loaded: %u saved pieces, %u partial blocks, %u remembered peers",saved,r->nblocks,r->npeers);}
    return;
bad:
    free(data);close(fd);memset(r->indexed,0,t->npieces);r->nblocks=r->npeers=0;
    LOG("Resume index damaged or belongs to different files; full file scan");
}
/* Data is flushed before publishing the index. A crash leaves either the old
   checked index or the new one; a torn/corrupt index triggers a full scan. */
static int resume_publish(torrent_t*t,const resume_block*blocks,uint32_t nb,const resume_peer*peers,uint32_t np){
    resume_state*r=t->resume;if(!r||!r->path[0])return -1;
    size_t n=RESUME_HEADER+t->npieces+(size_t)nb*28+(size_t)np*20+20;
    uint8_t*data=calloc(n,1);if(!data)return -1;
    memcpy(data,"NULLRS02",8);memcpy(data+8,t->ihash,20);resume_put32(data+28,t->plen);resume_put32(data+32,t->npieces);resume_put64(data+36,t->total);resume_put32(data+44,nb);resume_put32(data+48,np);
    sha1((const uint8_t*)output_dir,strlen(output_dir),data+52);
    uint8_t*p=data+RESUME_HEADER;memcpy(p,t->have,t->npieces);p+=t->npieces;
    for(uint32_t i=0;i<nb;i++,p+=28){resume_put32(p,blocks[i].piece);resume_put32(p+4,blocks[i].block);memcpy(p+8,blocks[i].hash,20);}
    for(uint32_t i=0;i<np;i++,p+=20){const resume_peer*v=&peers[i];memcpy(p,&v->ip,4);p[4]=v->port>>8;p[5]=v->port;p[6]=v->fails;p[7]=v->worked;resume_put32(p+8,v->seen);resume_put32(p+12,v->retry);p[16]=v->banned;}
    sha1(data,n-20,data+n-20);
    char tmp[1120];snprintf(tmp,sizeof tmp,"%s.tmp",r->path);
    int flags=O_WRONLY|O_CREAT|O_TRUNC;
#ifdef O_NOFOLLOW
    flags|=O_NOFOLLOW;
#endif
    int fd=open(tmp,flags,0666),ok=fd>=0;size_t done=0;
    while(ok&&done<n){ssize_t z=write(fd,data+done,n-done);if(z<0&&errno==EINTR)continue;if(z<=0){ok=0;break;}done+=(size_t)z;}
    if(ok&&fsync(fd))ok=0;if(fd>=0&&close(fd))ok=0;
    if(ok&&rename(tmp,r->path))ok=0;
    if(!ok)unlink(tmp);free(data);return ok?0:-1;
}
