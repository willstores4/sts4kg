/* Included after the swarm's connection/piece helpers. Only the event-loop
   thread snapshots blocks. Candidate copies use st_lock; disk I/O does not. */
static int resume_block_order(const void*a,const void*b){
    const resume_block*x=a,*y=b;
    if(x->piece!=y->piece)return x->piece<y->piece?-1:1;
    return x->block<y->block?-1:x->block>y->block;
}
static int checkpoint_restore(torrent_t*t){
    resume_state*r=t->resume;if(!r||!r->valid)return 0;
    unsigned restored=0,rejected=0;uint64_t bytes=0;uint8_t hash[20];
    for(uint32_t i=0;i<r->nblocks&&!stopped();i++){
        resume_block*b=&r->blocks[i];if(t->have[b->piece])continue;
        int a=act_find(b->piece);if(a<0)a=act_new(t,b->piece);if(a<0){fail("Out of memory restoring saved blocks");return -1;}
        active_t*p=&act[a];uint32_t n=blen(p,b->block),off=b->block*BLOCK;
        if(io_range(t,(uint64_t)b->piece*t->plen+off,p->buf+off,n,0)){rejected++;continue;}
        sha1(p->buf+off,n,hash);if(memcmp(hash,b->hash,20)){rejected++;continue;}
        p->got[b->block]=p->saved[b->block]=1;p->ngot++;restored++;bytes+=n;
    }
    if(stopped()){LOG("Partial restore interrupted; previous checkpoint retained");return -1;}
    for(int a=nact-1;a>=0;a--){if(act[a].ngot==act[a].nblocks)piece_done(t,a,now_ms());else if(!act[a].ngot)act_free(a);}
    LOG("Partial resume: %u blocks / %llu bytes restored, %u missing or corrupt blocks discarded",restored,(unsigned long long)bytes,rejected);
    r->nblocks=0;
    uint32_t wall=(uint32_t)time(NULL);int64_t now=now_ms();unsigned useful=0,cooling=0,expired=0;
    for(uint32_t i=0;i<r->npeers;i++){
        resume_peer*p=&r->peers[i];uint32_t ttl=p->worked?7u*86400:86400;
        if(!p->seen||p->seen>(uint64_t)wall+300||(wall>p->seen&&wall-p->seen>ttl)||!valid_peer(p->ip,p->port)){expired++;continue;}
        add_candidate(p->ip,p->port,SRC_CACHE);
        pthread_mutex_lock(&st_lock);
        for(size_t j=0;j<ncands;j++)if(cands[j].ip==p->ip&&cands[j].port==p->port){cand_t*c=&cands[j];c->worked=p->worked;c->fails=p->fails;c->seen=p->seen;
            uint32_t wait=p->retry>wall?p->retry-wall:0;if(wait>600)wait=600;
            c->retry_at=now+(int64_t)wait*1000;if(p->banned&&wait)c->state=C_BANNED;
            if(wait)cooling++;else if(c->worked)useful++;break;}
        pthread_mutex_unlock(&st_lock);
    }
    LOG("Peer resume: %u productive peers ready, %u cooling down, %u expired",useful,cooling,expired);
    return 0;
}
static int checkpoint_save(torrent_t*t,int force){
    resume_state*r=t->resume;if(!r||!r->path[0]||io_failed)return -1;
    int64_t now=now_ms();if(!force&&now<r->save_at)return 0;r->save_at=now+5000;
    resume_block*blocks=calloc(RESUME_BLOCKS,sizeof *blocks);if(!blocks)return -1;
    uint32_t nb=0,np=0;resume_peer peers[RESUME_PEERS];uint64_t partial_bytes=0;
    for(int a=0;a<nact;a++){active_t*p=&act[a];for(uint32_t b=0;b<p->nblocks;b++)if(p->got[b]){
        if(nb==RESUME_BLOCKS)goto failed;
        uint32_t n=blen(p,b),off=b*BLOCK;
        if(!p->saved[b]){if(io_range(t,(uint64_t)p->idx*t->plen+off,p->buf+off,n,1))goto failed;p->saved[b]=1;}
        blocks[nb].piece=p->idx;blocks[nb].block=b;sha1(p->buf+off,n,blocks[nb].hash);nb++;partial_bytes+=n;
    }}
    qsort(blocks,nb,sizeof *blocks,resume_block_order);
    for(size_t i=0;i<t->nfiles;i++)if(t->files[i].dirty){if(fsync(t->files[i].fd)){io_failed=1;fail("Could not flush download checkpoint to disk");goto failed;}t->files[i].dirty=0;}
    uint32_t wall=(uint32_t)time(NULL);
    pthread_mutex_lock(&st_lock);
    if(cands){
        /* Keep productive endpoints first, then failed peers with their cooldowns,
           then untried endpoints, within this torrent's bounded peer history. */
        for(int pass=0;pass<3;pass++)for(size_t j=0;j<ncands&&np<RESUME_PEERS;j++){
            cand_t*c=&cands[j];int group=c->worked?0:(c->fails||c->state==C_BANNED?1:2);if(group!=pass)continue;
            if(c->seen&&wall>c->seen&&wall-c->seen>(c->worked?7u*86400:86400))continue;
            resume_peer*p=&peers[np++];memset(p,0,sizeof *p);p->ip=c->ip;p->port=c->port;p->seen=c->seen;p->fails=c->fails;p->worked=c->worked;p->banned=c->state==C_BANNED;
            int64_t wait=c->retry_at-now;if(p->banned)wait=600000;
            if(wait>600000)wait=600000;if(wait>0&&(!c->worked||c->fails||p->banned))p->retry=wall+(uint32_t)((wait+999)/1000);
        }
    }else{np=r->npeers;memcpy(peers,r->peers,np*sizeof *peers);}
    pthread_mutex_unlock(&st_lock);
    if(resume_publish(t,blocks,nb,peers,np))goto failed;
    memcpy(r->peers,peers,np*sizeof *peers);r->npeers=np;r->valid=1;r->warned=0;
    LOG("Resume saved: %u verified pieces, %u partial blocks / %llu bytes, %u peers (%lldms)",count_have(t),nb,(unsigned long long)partial_bytes,np,(long long)(now_ms()-now));
    free(blocks);return 0;
failed:
    if(!r->warned){LOG("Resume checkpoint failed: %s; existing checkpoint retained",strerror(errno));r->warned=1;}
    free(blocks);return -1;
}
