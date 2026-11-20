#if defined(PORTAL_HOST) && defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "data_root.h"
// BitTorrent transfer engine.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <limits.h>
#include <sys/statvfs.h>
#include "torrent.h"
#include "net_compat.h"
#include <pthread.h>
#include <poll.h>
#ifdef PORTAL_HOST
#include <netdb.h>
#else
#include <orbis/libkernel.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#endif

typedef struct {
    const char *ctx_output_dir;
    const torrent_callbacks *ctx_callbacks;
    char ctx_last_error[256];
    int ctx_io_failed, ctx_g_quit;
    unsigned ctx_log_id;
    int ctx_socket_compat;
    int ctx_socket_tuning_logged;
    int64_t ctx_connect_retry_at, ctx_connect_limit_log;
    pthread_mutex_t ctx_st_lock, ctx_time_lock;
    int64_t ctx_time_last, ctx_time_offset, ctx_log_t0;
    void *ctx_dns_cache, *ctx_cands, *ctx_trk, *ctx_g_t, *ctx_conns, *ctx_act, *ctx_act_flag, *ctx_avail;
    size_t ctx_dns_next, ctx_ncands, ctx_ntrk;
    unsigned ctx_src_count[4];
    int ctx_need_peers;
    uint16_t ctx_listen_port;
    uint64_t ctx_st_left, ctx_st_down, ctx_st_up;
    uint32_t ctx_tracker_key;
    char ctx_dht_status[120];
    int ctx_rids[3], ctx_conn_cap, ctx_nact, ctx_act_cap;
    uint32_t ctx_g_nhave, ctx_g_maxmsg;
    uint64_t ctx_g_have_bytes, ctx_g_up_bytes;
    int ctx_g_halt, ctx_g_stopped, ctx_g_private;
    int64_t ctx_g_last_block;
    uint8_t ctx_peer_id[20];
    int ctx_peer_log_budget;
    unsigned ctx_random_state;
    struct {
        unsigned attempts, ok, refused, timeout, unreach, other, hs_fail;
        unsigned handshakes, choked_out, idle_out, incoming, hashfail, self, banned;
        uint64_t blocks, payload_bytes;
    } stats;
} torrent_session;
static pthread_key_t session_key;
static pthread_once_t session_key_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t log_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned next_log_id;
static void create_session_key(void){pthread_key_create(&session_key,NULL);}
static torrent_session*session_current(void){pthread_once(&session_key_once,create_session_key);return (torrent_session*)pthread_getspecific(session_key);}
static void session_bind(torrent_session*s){pthread_once(&session_key_once,create_session_key);pthread_setspecific(session_key,s);}
static unsigned session_random(void){unsigned x=__atomic_add_fetch(&session_current()->ctx_random_state,0x9e3779b9u,__ATOMIC_RELAXED);x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
#define output_dir (session_current()->ctx_output_dir)
#define callbacks (session_current()->ctx_callbacks)
#define last_error (session_current()->ctx_last_error)
#define io_failed (session_current()->ctx_io_failed)
#define g_quit (session_current()->ctx_g_quit)
#define st_lock (session_current()->ctx_st_lock)
#define time_lock (session_current()->ctx_time_lock)
#define time_last (session_current()->ctx_time_last)
#define time_offset (session_current()->ctx_time_offset)
#define log_t0 (session_current()->ctx_log_t0)
#define dns_cache (*(dns_entry**)&session_current()->ctx_dns_cache)
#define dns_next (session_current()->ctx_dns_next)
#define cands (*(cand_t**)&session_current()->ctx_cands)
#define ncands (session_current()->ctx_ncands)
#define src_count (session_current()->ctx_src_count)
#define need_peers (session_current()->ctx_need_peers)
#define listen_port (session_current()->ctx_listen_port)
#define st_left (session_current()->ctx_st_left)
#define st_down (session_current()->ctx_st_down)
#define st_up (session_current()->ctx_st_up)
#define tracker_key (session_current()->ctx_tracker_key)
#define dht_status (session_current()->ctx_dht_status)
#define trk (*(tracker_t**)&session_current()->ctx_trk)
#define ntrk (session_current()->ctx_ntrk)
#define g_t (*(torrent_t**)&session_current()->ctx_g_t)
#define rids (session_current()->ctx_rids)
#define conns (*(conn_t**)&session_current()->ctx_conns)
#define conn_cap (session_current()->ctx_conn_cap)
#define act (*(active_t**)&session_current()->ctx_act)
#define nact (session_current()->ctx_nact)
#define act_cap (session_current()->ctx_act_cap)
#define act_flag (*(uint8_t**)&session_current()->ctx_act_flag)
#define avail (*(uint16_t**)&session_current()->ctx_avail)
#define g_nhave (session_current()->ctx_g_nhave)
#define g_maxmsg (session_current()->ctx_g_maxmsg)
#define g_have_bytes (session_current()->ctx_g_have_bytes)
#define g_up_bytes (session_current()->ctx_g_up_bytes)
#define g_halt (session_current()->ctx_g_halt)
#define g_stopped (session_current()->ctx_g_stopped)
#define g_private (session_current()->ctx_g_private)
#define g_last_block (session_current()->ctx_g_last_block)
#define stats (session_current()->stats)
#define peer_id (session_current()->ctx_peer_id)
#define peer_log_budget (session_current()->ctx_peer_log_budget)
#define socket_compat (session_current()->ctx_socket_compat)

#define BLOCK 16384
#define PIPE 8
#define MAX_META (8u*1024*1024)
static int stopped(void) { return callbacks && callbacks->stopped && callbacks->stopped(callbacks->user); }
static void fail(const char *s) { snprintf(last_error,sizeof last_error,"%s",s); }
/* ---------------- threads, time, logging ---------------- */
static int quitting(void){return __atomic_load_n(&g_quit,__ATOMIC_ACQUIRE);}
static void init_locks(void){pthread_mutex_init(&st_lock,NULL);pthread_mutex_init(&time_lock,NULL);}
/* Wall clock adjusted so it never runs backwards. */
static int64_t now_ms(void){
    struct timeval tv;gettimeofday(&tv,NULL);int64_t raw=(int64_t)tv.tv_sec*1000+tv.tv_usec/1000;
    pthread_mutex_lock(&time_lock);int64_t v=raw+time_offset;if(v<time_last){time_offset+=time_last-v;v=time_last;}time_last=v;pthread_mutex_unlock(&time_lock);return v;
}
static void sleep_ms(int ms){struct timespec d={ms/1000,(long)(ms%1000)*1000000L};nanosleep(&d,NULL);}
#ifndef PORTAL_HOST
static FILE*log_file;static long log_size;
#endif
static void LOG(const char *fmt, ...) {
    char line[600];va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);
    int64_t t=now_ms()-log_t0;if(t<0)t=0;
    pthread_mutex_lock(&log_lock);
#ifdef PORTAL_HOST
    if(getenv("PORTAL_TORRENT_LOG"))fprintf(stderr,"[torrent %5lld.%03lld][job %u] %s\n",(long long)(t/1000),(long long)(t%1000),session_current()->ctx_log_id,line);
#else
    if(!log_file||log_size>1024*1024){
        const char *root=nullStoreDataRoot();
        char path[128],previous[128];
        snprintf(path,sizeof path,"%s/torrent.log",root);
        snprintf(previous,sizeof previous,"%s/torrent.log.previous",root);
        if(!log_file){
            mkdir(root,0777);
            log_file=fopen(path,"a");
            if(log_file&&fseek(log_file,0,SEEK_END)==0)log_size=ftell(log_file);
        }
        if(log_file&&log_size>1024*1024){
            fclose(log_file);
            rename(path,previous);
            log_file=fopen(path,"w");
            log_size=0;
        }
    }
    if(log_file){int k=fprintf(log_file,"[%5lld.%03lld][job %u] %s\n",(long long)(t/1000),(long long)(t%1000),session_current()->ctx_log_id,line);if(k>0)log_size+=k;fflush(log_file);}
#endif
    pthread_mutex_unlock(&log_lock);
}
static void log_close(void){
#ifndef PORTAL_HOST
    pthread_mutex_lock(&log_lock);if(log_file)fflush(log_file);pthread_mutex_unlock(&log_lock);
#endif
}
#ifdef PORTAL_HOST
#define LOG_HINT ""
#else
#define LOG_HINT " See torrent.log."
#endif
static const char *ename(int e){
    switch(e){
    case 0:return "no error";case EINTR:return "interrupted";case ENOENT:return "not found";case EACCES:return "permission denied";
    case EMFILE:return "too many open files";case EPIPE:return "broken pipe";case EAGAIN:return "would block";
    case EINPROGRESS:return "in progress";case EALREADY:return "already in progress";case EADDRINUSE:return "address in use";
    case EADDRNOTAVAIL:return "address not available";case ENETDOWN:return "network down";case ENETUNREACH:return "network unreachable";
    case ECONNABORTED:return "connection aborted";case ECONNRESET:return "connection reset";case ENOBUFS:return "no buffer space";
    case EISCONN:return "already connected";case ENOTCONN:return "not connected";case ETIMEDOUT:return "timed out";
    case ECONNREFUSED:return "connection refused";
    case EPROTONOSUPPORT:return "protocol unsupported";case EOPNOTSUPP:return "operation unsupported";case ECANCELED:return "cancelled";
#ifdef EHOSTDOWN
    case EHOSTDOWN:return "host down";
#endif
    case EHOSTUNREACH:return "host unreachable";
    default:return "error";
    }
}
#define EFMT "%s (errno %d)"
#define EARG(e) ename(e),(e)
static int ci_eq(const char*a,const char*b,size_t n){for(size_t i=0;i<n;i++){int x=(unsigned char)a[i],y=(unsigned char)b[i];if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y||!x)return 0;}return 1;}
static void addr_str(char out[24],uint32_t ip_be,uint16_t port){const uint8_t*b=(const uint8_t*)&ip_be;snprintf(out,24,"%u.%u.%u.%u:%u",b[0],b[1],b[2],b[3],port);}
/* ---------------- SHA1 ---------------- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_t;
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void sha1_block(sha1_t *c, const uint8_t *p) {
    uint32_t w[80], a, b, cc, d, e, t;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1_init(sha1_t *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->len = 0; c->n = 0;
}
static void sha1_update(sha1_t *c, const uint8_t *d, size_t l) {
    c->len += l;
    while (l) {
        size_t k = 64 - c->n; if (k > l) k = l;
        memcpy(c->buf + c->n, d, k); c->n += k; d += k; l -= k;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}
static void sha1_final(sha1_t *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha1_update(c, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = bits >> (56 - 8 * i);
    sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4] = c->h[i] >> 24; out[i*4+1] = c->h[i] >> 16;
        out[i*4+2] = c->h[i] >> 8; out[i*4+3] = c->h[i];
    }
}
static void sha1(const uint8_t *d, size_t l, uint8_t out[20]) {
    sha1_t c; sha1_init(&c); sha1_update(&c, d, l); sha1_final(&c, out);
}

/* ---------------- bencode ---------------- */
typedef struct B {
    char t;                    // i, s, l, d
    long long i;
    const uint8_t *s; size_t n;
    struct B **v; size_t cnt;  // list items / dict key,value pairs
    const uint8_t *st, *en;    // raw span
} B;

static void bfree(B *b) { if(!b)return;for(size_t i=0;i<b->cnt;i++)bfree(b->v[i]);free(b->v);free(b); }
static B *bparse(const uint8_t **p, const uint8_t *end, int depth) {
    if(*p>=end || depth>32)return NULL;
    B *b=calloc(1,sizeof *b);if(!b)return NULL;b->st=*p;int c=*(*p)++;
    if(c=='i') {
        b->t='i';int neg=0;if(*p<end&&**p=='-'){neg=1;(*p)++;}
        const uint8_t *start=*p;uint64_t v=0;
        while(*p<end&&**p>='0'&&**p<='9'){unsigned digit=*(*p)++-'0';if(v>((uint64_t)LLONG_MAX-digit)/10)goto bad;v=v*10+digit;}
        if(*p==start||*p>=end||**p!='e'||(*start=='0'&&*p-start>1)||(neg&&!v))goto bad;
        (*p)++;b->i=neg?-(long long)v:(long long)v;
    } else if(c=='l'||c=='d') {
        b->t=c;size_t cap=0;
        while(*p<end&&**p!='e'){
            if(b->cnt>=131072)goto bad;
            B *x=bparse(p,end,depth+1);if(!x)goto bad;
            if(b->cnt==cap){size_t nc=cap?cap*2:8;B **tmp=realloc(b->v,nc*sizeof *tmp);if(!tmp){bfree(x);goto bad;}b->v=tmp;cap=nc;}
            b->v[b->cnt++]=x;
        }
        if(*p>=end||**p!='e')goto bad;(*p)++;
        if(c=='d'){if(b->cnt%2)goto bad;for(size_t i=0;i<b->cnt;i+=2){B*k=b->v[i];if(k->t!='s')goto bad;if(i){B *q=b->v[i-2];size_t n=q->n<k->n?q->n:k->n;int cmp=memcmp(q->s,k->s,n);if(cmp>0||(cmp==0&&q->n>=k->n))goto bad;}}}
    } else if(c>='0'&&c<='9') {
        b->t='s';size_t n=c-'0';const uint8_t*start=*p-1;
        while(*p<end&&**p>='0'&&**p<='9'){unsigned d=*(*p)++-'0';if(n>(MAX_META-d)/10)goto bad;n=n*10+d;}
        if(*p>=end||**p!=':'||(c=='0'&&*p-start>1))goto bad;(*p)++;
        if(n>(size_t)(end-*p))goto bad;b->s=*p;b->n=n;*p+=n;
    }else goto bad;
    b->en=*p;return b;
bad:bfree(b);return NULL;
}
static B *bget(B *d, const char *key) {
    if (!d || d->t != 'd') return NULL;
    size_t kl = strlen(key);
    for (size_t k = 0; k + 1 < d->cnt; k += 2)
        if (d->v[k]->n == kl && !memcmp(d->v[k]->s, key, kl)) return d->v[k + 1];
    return NULL;
}
/* ---------------- sockets ---------------- */
/* Descriptor mode changes can be denied in a PS4 package process. In that
   case keep plain sockets, prove MSG_DONTWAIT works, and run connect() on
   bounded detached workers. Workers never reference torrent/session state. */
#ifndef PORTAL_HOST
#ifndef FIONBIO
#define FIONBIO _IOW('f', 126, int)
#endif
#endif
static int set_nonblocking(int fd,int verbose){
#ifndef PORTAL_HOST
    int enabled=1,native_result=setsockopt(fd,SOL_SOCKET,PORTAL_SO_NBIO,&enabled,sizeof enabled),native_error=native_result<0?errno:0;
    if(verbose)LOG("Socket probe: SO_NBIO=0x%x result=%d errno=%d",PORTAL_SO_NBIO,native_result,native_error);
    if(native_result==0)return 0;
#endif
#ifdef PORTAL_HOST
    if(getenv("PORTAL_TORRENT_FORCE_COMPAT")){
        if(verbose)LOG("Socket probe: simulated F_GETFL/F_SETFL permission denial");
        errno=EACCES;return -1;
    }
#endif
    int flags=fcntl(fd,F_GETFL,0),get_error=flags<0?errno:0,set_result=-1,set_error=get_error;
    if(flags>=0){set_result=fcntl(fd,F_SETFL,flags|O_NONBLOCK);set_error=set_result<0?errno:0;}
    if(verbose){if(flags<0)LOG("Socket probe: F_GETFL=%d errno=%d; F_SETFL skipped",flags,get_error);else LOG("Socket probe: F_GETFL=0x%x; F_SETFL=%d errno=%d",flags,set_result,set_error);}
    if(set_result==0)return 0;
#ifndef PORTAL_HOST
    int yes=1,r=ioctl(fd,FIONBIO,&yes),e=r<0?errno:0;
    if(verbose)LOG("Socket probe: FIONBIO=0x%08lx result=%d errno=%d",(unsigned long)FIONBIO,r,e);
    if(r==0)return 0;
    errno=e;
#else
    errno=set_error;
#endif
    return -1;
}
static int socket_timeouts(int fd){
    struct timeval tv={0,250000};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv)<0)return -1;
    return setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
}
static int init_socket_transport(void){
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0){LOG("Socket probe: socket() failed: " EFMT,EARG(errno));return -1;}
    if(socket_timeouts(fd)<0){int e=errno;close(fd);LOG("Socket probe: cannot set safety timeouts: " EFMT,EARG(e));errno=e;return -1;}
    int mode_result=set_nonblocking(fd,1),mode_error=mode_result<0?errno:0;
    struct sockaddr_in sa;portalAddress4(&sa,htonl(INADDR_LOOPBACK),0);
    if(bind(fd,(struct sockaddr*)&sa,sizeof sa)<0){int e=errno;close(fd);LOG("Socket probe: UDP bind failed: " EFMT,EARG(e));errno=e;return -1;}
    if(mode_result==0){
        char byte;int64_t began=now_ms();ssize_t n=recv(fd,&byte,1,0);int e=n<0?errno:0;int64_t elapsed=now_ms()-began;
        LOG("Socket probe: descriptor empty recv=%d errno=%d elapsed=%lldms (no MSG_DONTWAIT)",(int)n,e,(long long)elapsed);
        if(n<0&&(e==EAGAIN||e==EWOULDBLOCK)&&elapsed<100){close(fd);LOG("Socket transport: descriptor nonblocking verified; no connector workers needed");return 0;}
        mode_error=EOPNOTSUPP;LOG("Socket probe: mode setter returned success but descriptor nonblocking was not confirmed");
    }
    char byte;int64_t began=now_ms();ssize_t n=recv(fd,&byte,1,MSG_DONTWAIT);int e=n<0?errno:0;int64_t elapsed=now_ms()-began;close(fd);
    LOG("Socket probe: MSG_DONTWAIT=0x%x empty UDP recv=%d errno=%d elapsed=%lldms",MSG_DONTWAIT,(int)n,e,(long long)elapsed);
    if(n<0&&(e==EAGAIN||e==EWOULDBLOCK)&&elapsed<100){
        socket_compat=1;LOG("Socket transport: compatibility mode enabled after mode-change errno=%d; per-call nonblocking I/O, connector workers, outgoing peers only",mode_error);return 0;
    }
    LOG("Socket probe FAILED: per-call nonblocking read was not confirmed; stopping before peer I/O");errno=EOPNOTSUPP;return -1;
}
static int nb_socket(int domain,int type){
    int fd=socket(domain,type,0);if(fd<0){LOG("Socket creation type=%d failed: " EFMT,type,EARG(errno));return fd;}
    if((socket_compat?socket_timeouts(fd):set_nonblocking(fd,0))<0){int e=errno;LOG("Socket setup fd=%d type=%d method=%s failed: " EFMT,fd,type,socket_compat?"safety timeouts":"nonblocking",EARG(e));close(fd);errno=e;return -1;}return fd;
}
static int nb_accept(int fd,struct sockaddr*address,socklen_t*length){
    int client=accept(fd,address,length);if(client<0)return client;if(set_nonblocking(client,0)<0){int e=errno;close(client);errno=e;return -1;}return client;
}
/* Main and worker jointly own each job until it is harvested or cancelled.
   Cancellation shuts down the socket but leaves close() to its owner. An
   uninterruptible kernel connect can finish later without touching freed
   session state. The global limit includes cancelled jobs still in kernel. */
typedef struct {
    pthread_mutex_t lock;
    int fd,owner,done,cancelled,error;
    uint32_t ip;uint16_t port;
    int64_t elapsed;
} connect_job;
static int connect_workers;
#define MAX_CONNECT_WORKERS 32
static pthread_mutex_t connect_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static connect_job*connect_registry[MAX_CONNECT_WORKERS];
static int connect_job_register(connect_job*j){
    int slot=-1;pthread_mutex_lock(&connect_registry_lock);
    for(int i=0;i<MAX_CONNECT_WORKERS;i++){
        connect_job*other=connect_registry[i];
        if(other&&other->ip==j->ip&&other->port==j->port){pthread_mutex_unlock(&connect_registry_lock);errno=EALREADY;return -1;}
        if(!other&&slot<0)slot=i;
    }
    if(slot<0){pthread_mutex_unlock(&connect_registry_lock);errno=EAGAIN;return -1;}
    connect_registry[slot]=j;__atomic_add_fetch(&connect_workers,1,__ATOMIC_RELEASE);pthread_mutex_unlock(&connect_registry_lock);return 0;
}
static void connect_job_unregister(connect_job*j){
    pthread_mutex_lock(&connect_registry_lock);for(int i=0;i<MAX_CONNECT_WORKERS;i++)if(connect_registry[i]==j){connect_registry[i]=NULL;__atomic_sub_fetch(&connect_workers,1,__ATOMIC_RELEASE);break;}pthread_mutex_unlock(&connect_registry_lock);
}
static int64_t connect_clock(void){struct timeval tv;gettimeofday(&tv,NULL);return (int64_t)tv.tv_sec*1000+tv.tv_usec/1000;}
static void connect_job_free(connect_job*j){pthread_mutex_destroy(&j->lock);free(j);}
static void*connect_worker(void*arg){
    connect_job*j=arg;struct sockaddr_in sa;portalAddress4(&sa,j->ip,j->port);
    int64_t began=connect_clock(),end=began+10000;
    int r=connect(j->fd,(struct sockaddr*)&sa,sizeof sa),e=r<0?errno:0;
    if(r<0&&(e==EINPROGRESS||e==EINTR||e==EALREADY||e==EAGAIN||e==EWOULDBLOCK)){
        while(connect_clock()<end){
            pthread_mutex_lock(&j->lock);int cancel=j->cancelled;pthread_mutex_unlock(&j->lock);if(cancel){e=ECANCELED;break;}
            struct pollfd p={j->fd,POLLOUT,0};int ready=poll(&p,1,100);
            if(ready<0){if(errno==EINTR)continue;e=errno;break;}
            if(!ready)continue;
            socklen_t len=sizeof e;e=0;if(getsockopt(j->fd,SOL_SOCKET,SO_ERROR,&e,&len)<0)e=errno;if(!e&&!(p.revents&POLLOUT))e=ENOTCONN;break;
        }
        if(e==EINPROGRESS||e==EINTR||e==EALREADY||e==EAGAIN||e==EWOULDBLOCK)e=ETIMEDOUT;
    }
    pthread_mutex_lock(&j->lock);connect_job_unregister(j);j->error=e;j->elapsed=connect_clock()-began;j->done=1;
    if(j->cancelled&&j->fd>=0){close(j->fd);j->fd=-1;}
    int destroy=!j->owner;pthread_mutex_unlock(&j->lock);
    if(destroy)connect_job_free(j);return NULL;
}
static connect_job*connect_job_start(int fd,uint32_t ip,uint16_t port){
    connect_job*j=calloc(1,sizeof *j);if(!j){close(fd);errno=ENOMEM;return NULL;}
    pthread_mutex_init(&j->lock,NULL);j->fd=fd;j->owner=1;j->ip=ip;j->port=port;
    if(connect_job_register(j)<0){int e=errno;close(fd);connect_job_free(j);errno=e;return NULL;}
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);pthread_attr_setstacksize(&attr,128*1024);
    pthread_t thread;int e=pthread_create(&thread,&attr,connect_worker,j);pthread_attr_destroy(&attr);
    if(e){connect_job_unregister(j);close(fd);connect_job_free(j);errno=e;return NULL;}return j;
}
static void connect_job_cancel(connect_job*j){
    pthread_mutex_lock(&j->lock);j->cancelled=1;j->owner=0;int done=j->done;
    if(j->fd>=0){shutdown(j->fd,SHUT_RDWR);if(done){close(j->fd);j->fd=-1;}}
    pthread_mutex_unlock(&j->lock);if(done)connect_job_free(j);
}
/* Returns 0 pending, 1 complete; completed sockets transfer to caller. */
static int connect_job_take(connect_job*j,int*fd,int*error,int64_t*elapsed){
    pthread_mutex_lock(&j->lock);if(!j->done){pthread_mutex_unlock(&j->lock);return 0;}
    *fd=j->fd;*error=j->error;*elapsed=j->elapsed;j->fd=-1;j->owner=0;pthread_mutex_unlock(&j->lock);connect_job_free(j);return 1;
}
static void no_sigpipe(int fd){
#ifdef SO_NOSIGPIPE
    int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
#else
    (void)fd;
#endif
    int rx=256*1024,tx=64*1024,nodelay=1;
    int rr=setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rx,sizeof rx),re=rr<0?errno:0;
    int sr=setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&tx,sizeof tx),se=sr<0?errno:0;
    int nr=setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&nodelay,sizeof nodelay),ne=nr<0?errno:0;
    if(__sync_bool_compare_and_swap(&session_current()->ctx_socket_tuning_logged,0,1)){
        socklen_t n=sizeof rx;int actual_rx=-1,actual_tx=-1;getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&actual_rx,&n);n=sizeof tx;getsockopt(fd,SOL_SOCKET,SO_SNDBUF,&actual_tx,&n);
        LOG("TCP buffers: receive=%d (target %d, errno %d), send=%d (target %d, errno %d); TCP_NODELAY errno=%d",actual_rx,rx,re,actual_tx,tx,se,ne);
    }
}
static ssize_t sock_send(int fd,const void*b,size_t n){
#ifdef MSG_NOSIGNAL
    return send(fd,b,n,MSG_NOSIGNAL|MSG_DONTWAIT);
#else
    return send(fd,b,n,MSG_DONTWAIT);
#endif
}
/* Waits for fd readiness in short slices so quit requests are honoured. 1 ready, 0 timeout, -1 error/quit. */
static int wait_fd(int fd,short events,int timeout_ms){
    int64_t end=now_ms()+timeout_ms;
    for(;;){
        if(quitting())return -1;
        int64_t left=end-now_ms();if(left<=0)return 0;
        struct pollfd p={fd,events,0};int r=poll(&p,1,left>250?250:(int)left);
        if(r<0){if(errno==EINTR)continue;return -1;}
        if(r>0)return 1;
    }
}
/* TCP connect with timeout, using an isolated worker in compatibility mode. */
static int tcp_open(uint32_t ip,uint16_t port,int timeout_ms,int*err){
    *err=0;int fd=nb_socket(AF_INET,SOCK_STREAM);if(fd<0){*err=errno;return -1;}
    no_sigpipe(fd);
    if(socket_compat){
        char address[24];addr_str(address,ip,port);connect_job*j=connect_job_start(fd,ip,port);if(!j){*err=errno;return -1;}
        LOG("TCP %s: compatibility connect worker started (deadline %dms)",address,timeout_ms);
        int64_t end=now_ms()+timeout_ms,elapsed=0;
        while(!quitting()&&now_ms()<end){
            if(connect_job_take(j,&fd,err,&elapsed)){LOG("TCP %s: connect completed in %lldms: " EFMT,address,(long long)elapsed,EARG(*err));if(*err){close(fd);return -1;}return fd;}
            sleep_ms(20);
        }
        connect_job_cancel(j);*err=quitting()?ECANCELED:ETIMEDOUT;LOG("TCP %s: connect worker cancelled: " EFMT,address,EARG(*err));return -1;
    }
    struct sockaddr_in sa;memset(&sa,0,sizeof sa);portalAddress4(&sa,ip,port);
    if(connect(fd,(struct sockaddr*)&sa,sizeof sa)<0){
        if(errno!=EINPROGRESS&&errno!=EINTR&&errno!=EALREADY&&errno!=EWOULDBLOCK&&errno!=EAGAIN){*err=errno;close(fd);return -1;}
        int w=wait_fd(fd,POLLOUT,timeout_ms);if(w<=0){*err=w==0?ETIMEDOUT:EINTR;close(fd);return -1;}
        int e=0;socklen_t el=sizeof e;if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&el)<0)e=errno;if(e){*err=e;close(fd);return -1;}
    }
    return fd;
}
static int send_wait(int fd,const void*b,size_t n,int timeout_ms){
    const uint8_t*p=b;int64_t end=now_ms()+timeout_ms;
    while(n){ssize_t k=sock_send(fd,p,n);if(k>0){p+=k;n-=k;continue;}
        if(k<0&&(errno==EAGAIN||errno==EINTR)){int64_t left=end-now_ms();if(left<=0||wait_fd(fd,POLLOUT,(int)left)<=0)return -1;continue;}
        return -1;}
    return 0;
}

/* ---------------- DNS ---------------- */
typedef struct{char host[128];uint32_t ip;int ok;int64_t until;} dns_entry;
#ifndef PORTAL_HOST
static size_t dns_skip(const uint8_t*r,size_t n,size_t o){while(o<n){uint8_t l=r[o];if(!l)return o+1;if((l&0xC0)==0xC0)return o+2<=n?o+2:0;if(l&0xC0)return 0;o+=1+(size_t)l;}return 0;}
/* Plain UDP DNS A query, used when the system resolver fails. */
static int dns_raw(uint32_t server,const char*host,uint32_t*ip){
    uint8_t q[300];uint16_t id=(uint16_t)session_random();size_t n=12;memset(q,0,12);q[0]=id>>8;q[1]=id;q[2]=1;q[5]=1;
    for(const char*p=host;*p;){const char*dot=strchr(p,'.');size_t l=dot?(size_t)(dot-p):strlen(p);if(!l||l>63||n+l+6>sizeof q)return -1;q[n++]=(uint8_t)l;memcpy(q+n,p,l);n+=l;p+=l;if(*p=='.')p++;}
    q[n++]=0;q[n++]=0;q[n++]=1;q[n++]=0;q[n++]=1;
    int fd=nb_socket(AF_INET,SOCK_DGRAM);if(fd<0)return -1;
    struct sockaddr_in sa;memset(&sa,0,sizeof sa);portalAddress4(&sa,server,53);int rc=-1;
    if(connect(fd,(struct sockaddr*)&sa,sizeof sa)<0||sock_send(fd,q,n)!=(ssize_t)n)goto out;
    int64_t end=now_ms()+2000;
    while(now_ms()<end){
        if(wait_fd(fd,POLLIN,(int)(end-now_ms()))<=0)break;
        uint8_t r[1500];ssize_t k=recv(fd,r,sizeof r,MSG_DONTWAIT);if(k<12)continue;
        if((r[0]<<8|r[1])!=id||!(r[2]&0x80))continue;
        if(r[3]&0x0F)break;
        unsigned qd=r[4]<<8|r[5],an=r[6]<<8|r[7];size_t o=12;
        for(unsigned i=0;i<qd&&o;i++){o=dns_skip(r,k,o);if(o)o+=4;}
        for(unsigned i=0;i<an&&o&&o<(size_t)k;i++){o=dns_skip(r,k,o);if(!o||o+10>(size_t)k)break;unsigned type=r[o]<<8|r[o+1],cls=r[o+2]<<8|r[o+3],rl=r[o+8]<<8|r[o+9];o+=10;if(o+rl>(size_t)k)break;if(type==1&&cls==1&&rl==4){memcpy(ip,r+o,4);rc=0;goto out;}o+=rl;}
        break;
    }
out:close(fd);return rc;
}
static int system_resolve(int rid,const char*host,uint32_t*ip){
    OrbisNetInAddr a;int r=sceNetResolverStartNtoa(rid,host,&a,0,0,0);
    if(r>=0){*ip=a.s_addr;return 0;}
    LOG("DNS %s: system resolver error 0x%08x; retrying",host,(unsigned)r);
    if(quitting())return -1;
    r=sceNetResolverStartNtoa(rid,host,&a,5,2,0);
    if(r>=0){*ip=a.s_addr;return 0;}
    LOG("DNS %s: system resolver retry error 0x%08x; trying direct DNS",host,(unsigned)r);
    uint32_t servers[4];int ns=0;OrbisNetCtlInfo info;
    sceNetCtlInit();
    for(int code=17;code<=18;code++){memset(&info,0,sizeof info);if(sceNetCtlGetInfo(code,&info)>=0){char s[17];memcpy(s,info.primary_dns,16);s[16]=0;if(code==18){memcpy(s,info.secondary_dns,16);s[16]=0;}struct in_addr ia;if(inet_pton(AF_INET,s,&ia)==1&&ia.s_addr)servers[ns++]=ia.s_addr;}}
    struct in_addr ia;if(inet_pton(AF_INET,"1.1.1.1",&ia)==1)servers[ns++]=ia.s_addr;if(inet_pton(AF_INET,"8.8.8.8",&ia)==1)servers[ns++]=ia.s_addr;
    for(int i=0;i<ns&&!quitting();i++){if(!dns_raw(servers[i],host,ip)){char a2[24];addr_str(a2,servers[i],53);LOG("DNS %s: resolved via direct query to %s",host,a2);return 0;}}
    LOG("DNS %s: all resolution methods failed",host);return -1;
}
#endif
static int resolve(const char*host,uint32_t*ip,int rid){
    struct in_addr a;if(inet_pton(AF_INET,host,&a)==1){*ip=a.s_addr;return 0;}
    if(strlen(host)>=sizeof dns_cache[0].host)return -1;
    int64_t now=now_ms();
    pthread_mutex_lock(&st_lock);
    for(int i=0;i<32;i++)if(dns_cache[i].host[0]&&!strcmp(dns_cache[i].host,host)&&dns_cache[i].until>now){int ok=dns_cache[i].ok;*ip=dns_cache[i].ip;pthread_mutex_unlock(&st_lock);return ok?0:-1;}
    pthread_mutex_unlock(&st_lock);
    int rc;
#ifdef PORTAL_HOST
    (void)rid;struct addrinfo hints,*ai=NULL;memset(&hints,0,sizeof hints);hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    rc=getaddrinfo(host,NULL,&hints,&ai)?-1:0;if(!rc){*ip=((struct sockaddr_in*)ai->ai_addr)->sin_addr.s_addr;freeaddrinfo(ai);}
    if(rc)LOG("DNS %s: lookup failed",host);
#else
    rc=system_resolve(rid,host,ip);
#endif
    if(quitting())return -1;
    pthread_mutex_lock(&st_lock);dns_entry*e=&dns_cache[dns_next++%32];snprintf(e->host,sizeof e->host,"%s",host);e->ok=!rc;e->ip=rc?0:*ip;e->until=now_ms()+(rc?5:30)*60000LL;pthread_mutex_unlock(&st_lock);
    return rc;
}

/* ---------------- raw HTTP client (plain http only) ---------------- */
static int split_url(const char*url,const char*scheme,char*host,size_t hcap,uint16_t*port,uint16_t defport,const char**path){
    size_t sl=strlen(scheme);if(strncmp(url,scheme,sl))return -1;const char*h=url+sl,*e=h;
    while(*e&&*e!=':'&&*e!='/'&&*e!='?')e++;size_t hl=e-h;if(!hl||hl>=hcap)return -1;memcpy(host,h,hl);host[hl]=0;*port=defport;
    if(*e==':'){char*end;long p=strtol(e+1,&end,10);if(p<1||p>65535)return -1;*port=(uint16_t)p;e=end;}
    if(path)*path=*e?e:"/";return 0;
}
static const char*hdr_find(const char*h,const char*end,const char*name){
    size_t nl=strlen(name);
    for(const char*p=h;p&&p<end;){const char*eol=strstr(p,"\r\n");if(!eol||eol>end)eol=end;if((size_t)(eol-p)>nl&&ci_eq(p,name,nl)&&p[nl]==':'){p+=nl+1;while(*p==' '||*p=='\t')p++;return p;}p=eol+2;}
    return NULL;
}
/* Decodes a chunked body in place. Returns 1 complete, 0 need more, -1 malformed. */
static int unchunk(uint8_t*b,size_t n,size_t*outlen){
    size_t i=0,o=0;
    for(;;){
        size_t j=i;while(j+1<n&&!(b[j]=='\r'&&b[j+1]=='\n'))j++;if(j+1>=n)return 0;
        char*end;unsigned long long len=strtoull((const char*)b+i,&end,16);if(end==(char*)b+i)return -1;
        i=j+2;if(!len){*outlen=o;return 1;}
        if(len>n||i+len+2>n)return 0;
        memmove(b+o,b+i,(size_t)len);o+=len;i+=len+2;
    }
}
/* GET over plain HTTP. Returns HTTP status (body malloc'd) or -1 on network failure, -2 when unsupported (https). */
static int http_raw(const char*url,int rid,uint8_t**body,size_t*blen,char*why,size_t whycap,int depth){
    *body=NULL;*blen=0;char host[256];uint16_t port;const char*path;
    if(!strncmp(url,"https://",8)){snprintf(why,whycap,"https");return -2;}
    if(split_url(url,"http://",host,sizeof host,&port,80,&path)){snprintf(why,whycap,"bad URL");return -1;}
    uint32_t ip;if(resolve(host,&ip,rid)){snprintf(why,whycap,"DNS failed for %.60s",host);return -1;}
    int err;int fd=tcp_open(ip,port,8000,&err);if(fd<0){snprintf(why,whycap,"connect: " EFMT,EARG(err));return -1;}
    size_t rcap=strlen(path)+strlen(host)+200;char*req=malloc(rcap);if(!req){close(fd);snprintf(why,whycap,"out of memory");return -1;}
    char hp[16]="";if(port!=80)snprintf(hp,sizeof hp,":%u",port);
    int rl=snprintf(req,rcap,"GET %s HTTP/1.1\r\nHost: %s%s\r\nUser-Agent: NULL_store/1.0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",path,host,hp);
    if(send_wait(fd,req,rl,8000)){int e=errno;free(req);close(fd);snprintf(why,whycap,"send: " EFMT,EARG(e));return -1;}
    free(req);
    size_t cap=16384,n=0;uint8_t*buf=malloc(cap+1);if(!buf){close(fd);snprintf(why,whycap,"out of memory");return -1;}
    int64_t end=now_ms()+15000;size_t hlen=0;long long clen=-1;int chunked=0,status=0,done=0;
    while(!done){
        int64_t left=end-now_ms();if(left<=0){snprintf(why,whycap,"response timed out");goto bad;}
        int w=wait_fd(fd,POLLIN,(int)left);if(w<0){snprintf(why,whycap,"cancelled");goto bad;}if(w==0)continue;
        if(n==cap){if(cap>=4u*1024*1024){snprintf(why,whycap,"response too large");goto bad;}cap*=2;uint8_t*nb=realloc(buf,cap+1);if(!nb){snprintf(why,whycap,"out of memory");goto bad;}buf=nb;}
        ssize_t k=recv(fd,buf+n,cap-n,MSG_DONTWAIT);
        if(k<0){if(errno==EAGAIN||errno==EINTR)continue;int e=errno;snprintf(why,whycap,"recv: " EFMT,EARG(e));goto bad;}
        if(k==0)done=1;else n+=k;buf[n]=0;
        if(!hlen){uint8_t*he=(uint8_t*)strstr((char*)buf,"\r\n\r\n");if(!he){if(done){snprintf(why,whycap,"truncated headers");goto bad;}continue;}hlen=he-buf+4;
            if(strncmp((char*)buf,"HTTP/1.",7)||n<12){snprintf(why,whycap,"not an HTTP response");goto bad;}status=atoi((char*)buf+9);
            const char*cl=hdr_find((char*)buf,(char*)buf+hlen,"Content-Length");if(cl)clen=strtoll(cl,NULL,10);
            const char*te=hdr_find((char*)buf,(char*)buf+hlen,"Transfer-Encoding");if(te&&ci_eq(te,"chunked",7))chunked=1;
            if((status==301||status==302||status==303||status==307||status==308)){
                const char*loc=hdr_find((char*)buf,(char*)buf+hlen,"Location");close(fd);fd=-1;
                if(!loc||depth>=3){snprintf(why,whycap,"redirect %d without usable location",status);free(buf);return -1;}
                const char*le=loc;while(*le&&*le!='\r'&&*le!='\n')le++;size_t ll=le-loc;char*next=malloc(ll+300);if(!next){free(buf);return -1;}
                if(*loc=='/')snprintf(next,ll+300,"http://%s%s%.*s",host,hp,(int)ll,loc);else snprintf(next,ll+300,"%.*s",(int)ll,loc);
                LOG("HTTP %s: redirect %d -> %.200s",host,status,next);free(buf);
                int r=http_raw(next,rid,body,blen,why,whycap,depth+1);free(next);return r;
            }
        }
        if(hlen){size_t bl=n-hlen;
            if(chunked){uint8_t*tmp=malloc(bl+1);if(!tmp){snprintf(why,whycap,"out of memory");goto bad;}memcpy(tmp,buf+hlen,bl);size_t ol;int u=unchunk(tmp,bl,&ol);
                if(u<0){free(tmp);snprintf(why,whycap,"bad chunked encoding");goto bad;}
                if(u==1){tmp[ol]=0;close(fd);free(buf);*body=tmp;*blen=ol;return status;}
                free(tmp);if(done){snprintf(why,whycap,"truncated chunked body");goto bad;}}
            else if(clen>=0&&(long long)bl>=clen)done=1;
            if(done&&!chunked){if(clen>=0&&(long long)bl<clen){snprintf(why,whycap,"truncated body (%u of %lld bytes)",(unsigned)bl,clen);goto bad;}
                size_t keep=clen>=0?(size_t)clen:bl;memmove(buf,buf+hlen,keep);buf[keep]=0;close(fd);*body=buf;*blen=keep;return status;}
        }
    }
bad:if(fd>=0)close(fd);free(buf);return -1;
}
/* ---------------- torrent model ---------------- */
typedef struct { char path[1024]; uint64_t len, off; int fd, dirty; } tfile_t;
typedef struct resume_state resume_state;
typedef struct {
    uint8_t ihash[20];
    char name[256];
    uint64_t total; uint32_t plen, npieces;
    const uint8_t *hashes;
    tfile_t *files; size_t nfiles;
    uint8_t *have; uint32_t nhave;
    char **trackers; size_t ntrackers;
    B *root;
    resume_state *resume;
} torrent_t;
static uint32_t piece_len(torrent_t*t,uint32_t i);
#include "torrent_resume.h"

int torrent_identity(const unsigned char*data,size_t length,char out[41]){const uint8_t*p=data;B*root=bparse(&p,data+length,0);B*info=bget(root,"info");if(!root||p!=data+length||!info||info->t!='d'){bfree(root);return -1;}uint8_t hash[20];sha1(info->st,info->en-info->st,hash);for(int i=0;i<20;i++)sprintf(out+i*2,"%02x",hash[i]);out[40]=0;bfree(root);return 0;}
static int mkpath(const char *path) {
 char tmp[1100];if(!path||!*path||strlen(path)>=sizeof tmp){fail("Download directory path is empty or too long");return -1;}strcpy(tmp,path);
 for(char*p=tmp+1;;p++)if(*p=='/'||!*p){char c=*p;*p=0;struct stat st;
  if(portalPathInfo(tmp,&st)==0){if(!S_ISDIR(st.st_mode)){snprintf(last_error,sizeof last_error,"Download path is not a directory: %.170s",tmp);return -1;}}
  else if(errno!=ENOENT){int code=errno;snprintf(last_error,sizeof last_error,"Cannot inspect directory %.140s: %s (%d)",tmp,strerror(code),code);return -1;}
  else if(mkdir(tmp,0777)&&(errno!=EEXIST||portalPathInfo(tmp,&st)||!S_ISDIR(st.st_mode))){int code=errno;snprintf(last_error,sizeof last_error,"Cannot create directory %.140s: %s (%d)",tmp,strerror(code),code);return -1;}
  *p=c;if(!c)break;
 }return 0;
}
static int component(char *out,size_t cap,B *b) {
    if(!b||b->t!='s'||!b->n||b->n>=cap)return -1;
    for(size_t i=0;i<b->n;i++)if(b->s[i]<32||b->s[i]=='/'||b->s[i]=='\\'||b->s[i]==':')return -1;
    memcpy(out,b->s,b->n);out[b->n]=0;if(!strcmp(out,".")||!strcmp(out,".."))return -1;return 0;
}
static int add_tracker(torrent_t*t,const uint8_t*s,size_t n) {
    if(!n||n>1800||memchr(s,0,n)||t->ntrackers>=64)return 0;
    char*u=malloc(n+1);if(!u)return -1;memcpy(u,s,n);u[n]=0;
    if(strncmp(u,"http://",7)&&strncmp(u,"https://",8)&&strncmp(u,"udp://",6)){free(u);return 0;}
    for(size_t i=0;i<t->ntrackers;i++)if(!strcmp(u,t->trackers[i])){free(u);return 0;}
    char**v=realloc(t->trackers,(t->ntrackers+1)*sizeof *v);if(!v){free(u);return -1;}t->trackers=v;t->trackers[t->ntrackers++]=u;return 0;
}
static void free_torrent(torrent_t*t) {
    for(size_t i=0;i<t->nfiles;i++)if(t->files[i].fd>=0)close(t->files[i].fd);
    for(size_t i=0;i<t->ntrackers;i++)free(t->trackers[i]);free(t->trackers);free(t->files);free(t->have);resume_free(t->resume);bfree(t->root);memset(t,0,sizeof *t);
}
static int load_torrent(torrent_t*t,const uint8_t*data,size_t len) {
    memset(t,0,sizeof *t);const uint8_t*p=data;t->root=bparse(&p,data+len,0);B*info=bget(t->root,"info");
    if(!info||info->t!='d'||p!=data+len){fail("Invalid torrent metadata");return -1;}
    B*nm=bget(info,"name"),*pl=bget(info,"piece length"),*pcs=bget(info,"pieces"),*fl=bget(info,"files"),*ln=bget(info,"length");
    if(component(t->name,sizeof t->name,nm)||!pl||pl->t!='i'||pl->i<1||pl->i>32*1024*1024||!pcs||pcs->t!='s'||!pcs->n||pcs->n%20||((fl!=NULL)==(ln!=NULL))){fail("Unsupported or invalid torrent (BitTorrent v1 required)");return -1;}
    t->plen=pl->i;t->hashes=pcs->s;t->npieces=pcs->n/20;sha1(info->st,info->en-info->st,t->ihash);
    if(fl&&(fl->t!='l'||!fl->cnt||fl->cnt>8192)){fail("Invalid torrent file list");return -1;}
    t->nfiles=fl?fl->cnt:1;t->files=calloc(t->nfiles,sizeof *t->files);if(!t->files){t->nfiles=0;return -1;}
    for(size_t i=0;i<t->nfiles;i++)t->files[i].fd=-1;
    for(size_t i=0;i<t->nfiles;i++){
        tfile_t*f=&t->files[i];B*length=fl?bget(fl->v[i],"length"):ln;
        if(!length||length->t!='i'||length->i<0||length->i>(1LL<<43)||t->total>(1ULL<<43)-(uint64_t)length->i){fail("Invalid torrent file size");return -1;}
        int pos=snprintf(f->path,sizeof f->path,"%s/%s",output_dir,t->name);if(pos<0||(size_t)pos>=sizeof f->path)return -1;
        if(fl){B*path=bget(fl->v[i],"path");if(!path||path->t!='l'||!path->cnt){fail("Invalid torrent path");return -1;}for(size_t j=0;j<path->cnt;j++){char part[256];if(component(part,sizeof part,path->v[j])){fail("Unsafe torrent path rejected");return -1;}size_t need=strlen(part)+1;if((size_t)pos+need>=sizeof f->path){fail("Torrent path too long");return -1;}f->path[pos++]='/';strcpy(f->path+pos,part);pos+=strlen(part);}}
        for(size_t j=0;j<i;j++)if(!strcmp(f->path,t->files[j].path)){fail("Duplicate torrent file path");return -1;}
        f->off=t->total;f->len=length->i;t->total+=f->len;
    }
    if(!t->total||t->npieces!=(t->total+t->plen-1)/t->plen){fail("Piece hashes do not match torrent size");return -1;}
    B*an=bget(t->root,"announce");if(an&&an->t=='s'&&add_tracker(t,an->s,an->n))return -1;
    B*al=bget(t->root,"announce-list");if(al&&al->t=='l')for(size_t i=0;i<al->cnt;i++)if(al->v[i]->t=='l')for(size_t j=0;j<al->v[i]->cnt;j++){B*v=al->v[i]->v[j];if(v->t=='s'&&add_tracker(t,v->s,v->n))return -1;}
    B*private_flag=bget(info,"private");if(!t->ntrackers&&private_flag&&private_flag->t=='i'&&private_flag->i==1){fail("Private torrent has no supported trackers");return -1;}
    t->have=calloc(t->npieces,1);if(!t->have)return -1;
    if(mkpath(output_dir))return -1;
    for(size_t i=0;i<t->nfiles;i++){char dir[1100];strcpy(dir,t->files[i].path);char*sl=strrchr(dir,'/');*sl=0;if(mkpath(dir))return -1;
        int flags=O_RDWR|O_CREAT;
#ifdef O_NOFOLLOW
        flags|=O_NOFOLLOW;
#endif
        t->files[i].fd=open(t->files[i].path,flags,0666);struct stat st;
        if(t->files[i].fd<0||portalDescriptorInfo(t->files[i].fd,&st)||!S_ISREG(st.st_mode)){fail("Cannot open download file");return -1;}
    }return 0;
}
static int io_range(torrent_t*t,uint64_t off,uint8_t*buf,size_t n,int wr) {
    for(size_t i=0;i<t->nfiles&&n;i++){tfile_t*f=&t->files[i];if(off>=f->off+f->len)continue;if(off<f->off)return -1;uint64_t fo=off-f->off;size_t k=f->len-fo;if(k>n)k=n;size_t done=0;while(done<k){ssize_t r=wr?pwrite(f->fd,buf+done,k-done,fo+done):pread(f->fd,buf+done,k-done,fo+done);if(r<0&&errno==EINTR)continue;if(r<=0){if(wr){io_failed=1;fail("Disk write failed. Check free space.");}return -1;}if(wr)f->dirty=1;done+=r;}buf+=k;off+=k;n-=k;}return n?-1:0;
}
static uint32_t piece_len(torrent_t *t, uint32_t i) {
    return (i == t->npieces - 1) ? (uint32_t)(t->total - (uint64_t)i * t->plen) : t->plen;
}
static uint32_t count_have(torrent_t *t) { uint32_t c = 0; for (uint32_t i = 0; i < t->npieces; i++) c += t->have[i]; return c; }

static uint64_t have_bytes(torrent_t*t){uint64_t n=0;for(uint32_t i=0;i<t->npieces;i++)if(t->have[i])n+=piece_len(t,i);return n;}
static void report(torrent_t*t,const char*phase,unsigned peers){if(callbacks&&callbacks->progress)callbacks->progress(callbacks->user,phase,have_bytes(t),t->total,peers);}
static int recheck(torrent_t*t) {
    uint8_t*buf=malloc(t->plen),h[20];if(!buf)return -1;
    int indexed=t->resume&&t->resume->valid;uint64_t work=0,checked=0,read_bytes=0;unsigned rejected=0;
    for(uint32_t i=0;i<t->npieces;i++)if(!indexed||t->resume->indexed[i])work+=piece_len(t,i);
    int64_t start=now_ms(),last=start;char phase[100];
    snprintf(phase,sizeof phase,indexed?"Verifying saved pieces: 0%%":"Checking files: 0%% (full scan)");report(t,phase,0);
    for(uint32_t i=0;i<t->npieces&&!stopped();i++){
        if(indexed&&!t->resume->indexed[i])continue;
        uint32_t n=piece_len(t,i);
        if(io_range(t,(uint64_t)i*t->plen,buf,n,0)==0){sha1(buf,n,h);t->have[i]=!memcmp(h,t->hashes+i*20,20);read_bytes+=n;}
        if(indexed&&!t->have[i])rejected++;checked+=n;
        int64_t now=now_ms();if(now-last>=200||checked==work){last=now;unsigned percent=work?(unsigned)(checked*100/work):100;snprintf(phase,sizeof phase,indexed?"Verifying saved pieces: %u%%":"Checking files: %u%% (full scan)",percent);report(t,phase,0);}
    }
    if(!work&&!stopped())report(t,"Verifying saved pieces: 100%",0);
    LOG("File verification: %s, %llu bytes read, %u saved pieces rejected, %lldms%s",indexed?"resume index":"full scan",(unsigned long long)read_bytes,rejected,(long long)(now_ms()-start),stopped()?" (stopped)":"");
    free(buf);return stopped()?-1:0;
}

/* ---------------- trackers ---------------- */
typedef struct { uint32_t ip; uint16_t port; } peer_t;

static void urlenc(char *o, const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) o += sprintf(o, "%%%02X", d[i]);
}
static size_t parse_compact(peer_t *out, size_t max, const uint8_t *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i + 6 <= n && c < max; i += 6, c++) {
        memcpy(&out[c].ip, s + i, 4);
        out[c].port = s[i+4] << 8 | s[i+5];
    }
    return c;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
/* ---------------- shared discovery state (st_lock) ---------------- */
#define MAX_CAND 2000
#define STARVED_REANNOUNCE_S 45
#define MIN_USEFUL_PEERS 5
#define MIN_ELIGIBLE_PEERS 10
enum{SRC_TRACKER=0,SRC_DHT=1,SRC_PEX=2,SRC_CACHE=3,SRC_COUNT=4};
enum{C_IDLE=0,C_ACTIVE=1,C_BANNED=2};
typedef struct{uint32_t ip,seen;uint16_t port;uint8_t src,state,fails,hashfail,worked;int64_t retry_at;} cand_t;
static int valid_peer(uint32_t ip,uint16_t port){
    const uint8_t*b=(const uint8_t*)&ip;if(!port||!ip||b[0]==0||b[0]>=224)return 0;
#ifndef PORTAL_HOST
    if(b[0]==127)return 0;
#endif
    return 1;
}
static int add_candidate(uint32_t ip,uint16_t port,int src){
    if(!valid_peer(ip,port))return 0;int added=0;
    pthread_mutex_lock(&st_lock);
    size_t i=0;for(;i<ncands;i++)if(cands[i].ip==ip&&cands[i].port==port)break;
    if(i==ncands&&cands&&ncands<MAX_CAND){memset(&cands[ncands],0,sizeof cands[0]);cands[ncands].ip=ip;cands[ncands].port=port;cands[ncands].src=(uint8_t)src;ncands++;src_count[src]++;added=1;}
    if(i<ncands)cands[i].seen=(uint32_t)time(NULL);
    pthread_mutex_unlock(&st_lock);return added;
}

/* ---------------- trackers ---------------- */
typedef struct{
    const char*url;char host[64];int udp,started,fails,interval,min_interval,seeders,leechers;
    int64_t next,last_try,last_success,cid_time;unsigned last_peers,total_peers;uint8_t cid[8];char trackerid[200];char status[112];
} tracker_t;
static void tracker_done(tracker_t*tr,int ok,unsigned peers,const char*why){
    int64_t now=now_ms();
    pthread_mutex_lock(&st_lock);
    tr->last_try=now;
    if(ok){tr->fails=0;tr->last_success=now;tr->last_peers=peers;tr->total_peers+=peers;tr->started=1;
        int iv=tr->interval>0?tr->interval:1800;if(iv<60)iv=60;if(iv>3600)iv=3600;tr->next=now+iv*1000LL;
        snprintf(tr->status,sizeof tr->status,"%u peers",peers);}
    else{tr->fails++;int64_t back=30000LL<<(tr->fails>7?6:tr->fails-1);if(back>1800000)back=1800000;tr->next=now+back;snprintf(tr->status,sizeof tr->status,"%s",why);}
    pthread_mutex_unlock(&st_lock);
    if(ok)LOG("Tracker %s: %u peers (seeders %d, leechers %d), next announce in %ds",tr->host,peers,tr->seeders,tr->leechers,(int)((tr->next-now)/1000));
    else LOG("Tracker %s: FAILED (%s); retry %d in %ds",tr->host,why,tr->fails,(int)((tr->next-now)/1000));
}
static unsigned add_compact(const uint8_t*s,size_t n,int src){
    unsigned c=0;peer_t p[1];for(size_t i=0;i+6<=n;i+=6){parse_compact(p,1,s+i,6);c+=add_candidate(p[0].ip,p[0].port,src);}return c;
}
static void urlenc_bytes(char*o,const uint8_t*d,size_t n){for(size_t i=0;i<n;i++){unsigned char c=d[i];if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')*o++=c;else o+=sprintf(o,"%%%02X",c);}*o=0;}
static void announce_http(tracker_t*tr,torrent_t*t,int rid){
    char ih[61],pi[61];urlenc(ih,t->ihash,20);urlenc(pi,peer_id,20);
    pthread_mutex_lock(&st_lock);uint64_t left=st_left,down=st_down,up=st_up;unsigned port=listen_port;pthread_mutex_unlock(&st_lock);
    size_t ul=strlen(tr->url)+800;char*url=malloc(ul);if(!url){tracker_done(tr,0,0,"out of memory");return;}
    snprintf(url,ul,"%s%cinfo_hash=%s&peer_id=%s&port=%u&uploaded=%llu&downloaded=%llu&left=%llu&corrupt=0&key=%08x&numwant=200&compact=1&no_peer_id=1%s%s%s",
        tr->url,strchr(tr->url,'?')?'&':'?',ih,pi,port?port:6881,(unsigned long long)up,(unsigned long long)down,(unsigned long long)left,tracker_key,
        tr->started?"":"&event=started",tr->trackerid[0]?"&trackerid=":"",tr->trackerid);
    char why[96]="";uint8_t*body=NULL;size_t len=0;int status=http_raw(url,rid,&body,&len,why,sizeof why,0);
    const char*via="direct";B*r=NULL;
    if(status==200&&body){const uint8_t*p=body;r=bparse(&p,body+len,0);if(!r||r->t!='d'||p!=body+len){bfree(r);r=NULL;snprintf(why,sizeof why,"invalid tracker response (%u bytes)",(unsigned)len);}}
    else if(status>0)snprintf(why,sizeof why,"HTTP status %d",status);
    if(!r){
        if(status!=-2)LOG("Tracker %s: direct HTTP failed (%s)%s",tr->host,why,callbacks&&callbacks->http?"; trying system HTTP":"");
        free(body);body=NULL;len=0;
        if(callbacks&&callbacks->http&&!quitting()){via="system";body=callbacks->http(callbacks->user,url,&len);
            if(body){const uint8_t*p=body;r=bparse(&p,body+len,0);if(!r||r->t!='d'||p!=body+len){bfree(r);r=NULL;snprintf(why,sizeof why,"invalid tracker response (%u bytes)",(unsigned)len);}}
            else if(status==-2)snprintf(why,sizeof why,"HTTPS request failed");
            else{char tmp[96];snprintf(tmp,sizeof tmp,"%s; system HTTP also failed",why);snprintf(why,sizeof why,"%s",tmp);}}
    }
    free(url);
    if(!r){free(body);tracker_done(tr,0,0,why);return;}
    B*failure=bget(r,"failure reason");
    if(failure&&failure->t=='s'){snprintf(why,sizeof why,"rejected: %.*s",(int)(failure->n>80?80:failure->n),failure->s);bfree(r);free(body);tracker_done(tr,0,0,why);return;}
    B*warn=bget(r,"warning message");if(warn&&warn->t=='s')LOG("Tracker %s warning: %.*s",tr->host,(int)(warn->n>160?160:warn->n),warn->s);
    B*iv=bget(r,"interval"),*mi=bget(r,"min interval"),*tid=bget(r,"tracker id"),*co=bget(r,"complete"),*inc=bget(r,"incomplete");
    tr->interval=iv&&iv->t=='i'?(int)(iv->i>3600?3600:iv->i):1800;tr->min_interval=mi&&mi->t=='i'?(int)(mi->i>3600?3600:mi->i):0;
    if(tid&&tid->t=='s'&&tid->n<60)urlenc_bytes(tr->trackerid,tid->s,tid->n);
    tr->seeders=co&&co->t=='i'?(int)co->i:-1;tr->leechers=inc&&inc->t=='i'?(int)inc->i:-1;
    unsigned got=0,listed=0;B*pe=bget(r,"peers");
    if(pe&&pe->t=='s'){listed=pe->n/6;got=add_compact(pe->s,pe->n,SRC_TRACKER);}
    else if(pe&&pe->t=='l')for(size_t i=0;i<pe->cnt;i++){B*ip=bget(pe->v[i],"ip"),*po=bget(pe->v[i],"port");if(!ip||ip->t!='s'||ip->n>=46||!po||po->t!='i'||po->i<1||po->i>65535)continue;char h[48];memcpy(h,ip->s,ip->n);h[ip->n]=0;struct in_addr a;listed++;if(inet_pton(AF_INET,h,&a)==1)got+=add_candidate(a.s_addr,(uint16_t)po->i,SRC_TRACKER);}
    LOG("Tracker %s (%s HTTP): %u peers listed, %u new",tr->host,via,listed,got);
    bfree(r);free(body);tracker_done(tr,1,listed,"");
}
/* One request/response exchange on a connected UDP socket. Returns length, 0 timeout, -1 error. */
static int udp_xchg(int fd,const uint8_t*req,size_t rl,uint8_t*rsp,size_t cap,uint32_t tx,int timeout_ms,int*err){
    *err=0;if(send_wait(fd,req,rl,timeout_ms)<0){*err=errno;return -1;}
    int64_t end=now_ms()+timeout_ms;
    for(;;){int64_t left=end-now_ms();if(left<=0)return 0;int w=wait_fd(fd,POLLIN,(int)left);if(w<0)return -1;if(w==0)return 0;
        ssize_t n=recv(fd,rsp,cap,MSG_DONTWAIT);if(n<0){if(errno==EAGAIN||errno==EINTR)continue;*err=errno;return -1;}
        if(n>=8&&be32(rsp+4)==tx)return (int)n;}
}
static void announce_udp(tracker_t*tr,torrent_t*t,int rid){
    char host[256],why[96];uint16_t port;
    if(split_url(tr->url,"udp://",host,sizeof host,&port,80,NULL)){tracker_done(tr,0,0,"bad URL");return;}
    uint32_t ip;if(resolve(host,&ip,rid)){snprintf(why,sizeof why,"DNS failed for %.60s",host);tracker_done(tr,0,0,why);return;}
    int fd=nb_socket(AF_INET,SOCK_DGRAM);if(fd<0){int e=errno;snprintf(why,sizeof why,"socket: " EFMT,EARG(e));tracker_done(tr,0,0,why);return;}
    struct sockaddr_in sa;memset(&sa,0,sizeof sa);portalAddress4(&sa,ip,port);
    if(connect(fd,(struct sockaddr*)&sa,sizeof sa)<0){int e=errno;close(fd);snprintf(why,sizeof why,"connect: " EFMT,EARG(e));tracker_done(tr,0,0,why);return;}
    uint8_t req[98],rsp[2048];int err=0,n=0;
    if(now_ms()-tr->cid_time>60000){
        for(int attempt=0;attempt<2&&!quitting();attempt++){
            uint32_t tx=session_random();put32(req,0x417);put32(req+4,0x27101980);put32(req+8,0);put32(req+12,tx);
            n=udp_xchg(fd,req,16,rsp,sizeof rsp,tx,attempt?6000:4000,&err);
            if(n>=16&&be32(rsp)==0){memcpy(tr->cid,rsp+8,8);tr->cid_time=now_ms();break;}
            if(n>=8&&be32(rsp)==3){snprintf(why,sizeof why,"error: %.*s",n-8>70?70:n-8,(char*)rsp+8);close(fd);tracker_done(tr,0,0,why);return;}
            if(n<0)break;
        }
        if(now_ms()-tr->cid_time>60000){close(fd);
            if(err==ECONNREFUSED)snprintf(why,sizeof why,"port unreachable (UDP refused)");
            else if(err)snprintf(why,sizeof why,"UDP " EFMT,EARG(err));else snprintf(why,sizeof why,"no reply from %s:%u",host,port);
            tracker_done(tr,0,0,why);return;}
    }
    pthread_mutex_lock(&st_lock);uint64_t left=st_left,down=st_down,up=st_up;unsigned lp=listen_port;pthread_mutex_unlock(&st_lock);
    uint32_t tx=session_random();memset(req,0,sizeof req);memcpy(req,tr->cid,8);put32(req+8,1);put32(req+12,tx);
    memcpy(req+16,t->ihash,20);memcpy(req+36,peer_id,20);
    put32(req+56,down>>32);put32(req+60,(uint32_t)down);put32(req+64,left>>32);put32(req+68,(uint32_t)left);put32(req+72,up>>32);put32(req+76,(uint32_t)up);
    put32(req+80,tr->started?0:2);put32(req+88,tracker_key);put32(req+92,200);if(!lp)lp=6881;req[96]=lp>>8;req[97]=lp&255;
    for(int attempt=0;attempt<2&&!quitting();attempt++){n=udp_xchg(fd,req,98,rsp,sizeof rsp,tx,attempt?8000:5000,&err);if(n!=0)break;}
    close(fd);
    if(n>=20&&be32(rsp)==1){tr->interval=(int)be32(rsp+8);tr->leechers=(int)be32(rsp+12);tr->seeders=(int)be32(rsp+16);
        unsigned listed=(n-20)/6,got=add_compact(rsp+20,n-20,SRC_TRACKER);LOG("Tracker %s (UDP): %u peers listed, %u new",tr->host,listed,got);tracker_done(tr,1,listed,"");return;}
    if(n>=8&&be32(rsp)==3){snprintf(why,sizeof why,"error: %.*s",n-8>70?70:n-8,(char*)rsp+8);tr->cid_time=0;}
    else if(n<0&&err==ECONNREFUSED)snprintf(why,sizeof why,"port unreachable (UDP refused)");
    else if(n<0)snprintf(why,sizeof why,"UDP " EFMT,EARG(err));
    else{snprintf(why,sizeof why,"announce got no reply");tr->cid_time=0;}
    tracker_done(tr,0,0,why);
}
static void tracker_loop(int udp,int rid){
    while(!quitting()){
        int64_t now=now_ms();tracker_t*due=NULL;
        pthread_mutex_lock(&st_lock);int np=need_peers;
        for(size_t i=0;i<ntrk&&!due;i++){tracker_t*tr=&trk[i];if(tr->udp!=udp)continue;
            if(now>=tr->next)due=tr;
            else if(np&&tr->last_success&&tr->fails==0){int gap=tr->min_interval>STARVED_REANNOUNCE_S?tr->min_interval:STARVED_REANNOUNCE_S;if(now-tr->last_try>=gap*1000LL)due=tr;}}
        pthread_mutex_unlock(&st_lock);
        if(!due){sleep_ms(250);continue;}
        LOG("Announcing to %s",due->url);
        if(udp)announce_udp(due,g_t,rid);else announce_http(due,g_t,rid);
    }
}
static void*tracker_http_thread(void*a){session_bind((torrent_session*)a);tracker_loop(0,rids[0]);return NULL;}
static void*tracker_udp_thread(void*a){session_bind((torrent_session*)a);tracker_loop(1,rids[1]);return NULL;}

/* ---------------- DHT (BEP 5 get_peers, read-only BEP 43) ---------------- */
#define DHT_MAX 256
typedef struct{uint8_t id[20];uint32_t ip;uint16_t port;uint8_t queried,replied;} dht_node;
static int dht_closer(const uint8_t*a,const uint8_t*b,const uint8_t*target){for(int i=0;i<20;i++){unsigned x=a[i]^target[i],y=b[i]^target[i];if(x!=y)return x<y;}return 0;}
static void dht_add(dht_node*nodes,size_t*count,const uint8_t*id,uint32_t ip,uint16_t port,const uint8_t*target){
    if(!valid_peer(ip,port))return;for(size_t i=0;i<*count;i++)if(nodes[i].ip==ip&&nodes[i].port==port)return;
    size_t pos=*count;if(pos>=DHT_MAX){pos=0;for(size_t i=1;i<*count;i++)if(dht_closer(nodes[pos].id,nodes[i].id,target))pos=i;if(!dht_closer(id,nodes[pos].id,target))return;}else ++*count;
    memcpy(nodes[pos].id,id,20);nodes[pos].ip=ip;nodes[pos].port=port;nodes[pos].queried=0;nodes[pos].replied=0;
}
static void dht_set_status(const char*fmt,...){char s[120];va_list ap;va_start(ap,fmt);vsnprintf(s,sizeof s,fmt,ap);va_end(ap);pthread_mutex_lock(&st_lock);snprintf(dht_status,sizeof dht_status,"%s",s);pthread_mutex_unlock(&st_lock);}
static size_t dht_seed(torrent_t*t,dht_node*nodes,size_t*count,int rid){
    uint8_t zero[20]={0};size_t before=*count;
    B*bootstrap=bget(t->root,"nodes");
    if(bootstrap&&bootstrap->t=='l')for(size_t i=0;i<bootstrap->cnt&&i<32&&!quitting();i++){B*n=bootstrap->v[i];if(n->t!='l'||n->cnt!=2||n->v[0]->t!='s'||n->v[0]->n>250||memchr(n->v[0]->s,0,n->v[0]->n)||n->v[1]->t!='i'||n->v[1]->i<1||n->v[1]->i>65535)continue;char host[256];memcpy(host,n->v[0]->s,n->v[0]->n);host[n->v[0]->n]=0;uint32_t ip;if(!resolve(host,&ip,rid))dht_add(nodes,count,zero,ip,(uint16_t)n->v[1]->i,t->ihash);else LOG("DHT: cannot resolve torrent node %s",host);}
#ifndef PORTAL_HOST
    const char*routers[]={"dht.libtorrent.org","router.bittorrent.com","dht.transmissionbt.com","router.utorrent.com","dht.aelitis.com"};const uint16_t ports[]={25401,6881,6881,6881,6881};
    for(int i=0;i<5&&!quitting();i++){uint32_t ip;if(!resolve(routers[i],&ip,rid)){dht_add(nodes,count,zero,ip,ports[i],t->ihash);char a[24];addr_str(a,ip,ports[i]);LOG("DHT: bootstrap %s -> %s",routers[i],a);}else LOG("DHT: cannot resolve bootstrap router %s",routers[i]);}
#endif
    return *count-before;
}
static void*dht_thread(void*arg){
    session_bind((torrent_session*)arg);torrent_t*t=g_t;int rid=rids[2];
    dht_node*nodes=calloc(DHT_MAX,sizeof *nodes);int fd=nb_socket(AF_INET,SOCK_DGRAM);
    if(!nodes||fd<0){int e=errno;LOG("DHT: cannot start: " EFMT,EARG(e));dht_set_status("DHT unavailable (socket error)");free(nodes);if(fd>=0)close(fd);return NULL;}
    size_t count=0;uint8_t local_id[20];sha1(peer_id,20,local_id);int64_t next_run=0,last_run=0;int runs=0;
    while(!quitting()){
        int64_t now=now_ms();pthread_mutex_lock(&st_lock);int np=need_peers;pthread_mutex_unlock(&st_lock);
        /* Normal cadence is next_run; while short of peers, rerun at most once a minute. */
        if(runs&&now<next_run&&!(np&&now-last_run>=60000)){sleep_ms(250);continue;}
        last_run=now;
        size_t alive=0;for(size_t i=0;i<count;i++)if(nodes[i].replied)nodes[alive++]=nodes[i];count=alive;
        if(count<8)dht_seed(t,nodes,&count,rid);
        for(size_t i=0;i<count;i++)nodes[i].queried=0;
        runs++;
        if(!count){if(runs==1||runs%10==0)LOG("DHT: no bootstrap nodes reachable (attempt %d); retrying every minute",runs);dht_set_status("DHT: no bootstrap nodes (DNS failed?)");next_run=now_ms()+60000;continue;}
        LOG("DHT lookup %d starting with %u nodes",runs,(unsigned)count);
        struct{uint32_t tx,ip;uint16_t port;int64_t sent;int used;} fl[8];memset(fl,0,sizeof fl);
        unsigned queries=0,replies=0,found=0,newp=0,bad_packets=0,unmatched=0,send_errors=0,recv_errors=0;int64_t deadline=now_ms()+45000,last_progress=now_ms();int inflight=0;
        while(!quitting()&&now_ms()<deadline&&queries<250){
            int64_t tnow=now_ms();
            if(tnow-last_progress>=5000){last_progress=tnow;LOG("DHT lookup %d progress: %u queries, %u replies, %u peers, %u in flight, %u nodes (%u send, %u receive errors)",runs,queries,replies,found,(unsigned)inflight,(unsigned)count,send_errors,recv_errors);dht_set_status("DHT: lookup %d, %u/%u replies",runs,replies,queries);}
            for(int s=0;s<8;s++)if(fl[s].used&&tnow-fl[s].sent>3000){fl[s].used=0;inflight--;}
            while(inflight<8&&queries<250){
                size_t best=count;for(size_t i=0;i<count;i++)if(!nodes[i].queried&&(best==count||dht_closer(nodes[i].id,nodes[best].id,t->ihash)))best=i;
                if(best==count)break;nodes[best].queried=1;
                uint8_t req[180],*p=req;const char*start="d1:ad2:id20:";memcpy(p,start,strlen(start));p+=strlen(start);memcpy(p,local_id,20);p+=20;const char*mid="9:info_hash20:";memcpy(p,mid,strlen(mid));p+=strlen(mid);memcpy(p,t->ihash,20);p+=20;const char*tail="e1:q9:get_peers2:roi1e1:t4:";memcpy(p,tail,strlen(tail));p+=strlen(tail);
                uint32_t tx=session_random();put32(p,tx);p+=4;memcpy(p,"1:y1:qe",7);p+=7;
                struct sockaddr_in to;memset(&to,0,sizeof to);portalAddress4(&to,nodes[best].ip,nodes[best].port);
                if(sendto(fd,req,p-req,MSG_DONTWAIT,(struct sockaddr*)&to,sizeof to)!=(ssize_t)(p-req)){int e=errno;char a[24];addr_str(a,nodes[best].ip,nodes[best].port);send_errors++;LOG("DHT: send to %s failed: " EFMT,a,EARG(e));continue;}
                int s=0;while(fl[s].used)s++;fl[s].used=1;fl[s].tx=tx;fl[s].ip=nodes[best].ip;fl[s].port=nodes[best].port;fl[s].sent=now_ms();inflight++;queries++;
            }
            if(!inflight)break;
            int ready=wait_fd(fd,POLLIN,200);if(ready<0){if(!quitting())LOG("DHT lookup %d poll failed: " EFMT,runs,EARG(errno));break;}if(ready==0)continue;
            for(;;){
                uint8_t rsp[4096];struct sockaddr_in from;socklen_t fl_len=sizeof from;ssize_t n=recvfrom(fd,rsp,sizeof rsp,MSG_DONTWAIT,(struct sockaddr*)&from,&fl_len);
                if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)break;int e=errno;recv_errors++;LOG("DHT: recvfrom failed: " EFMT,EARG(e));break;}
                if(n==0){recv_errors++;LOG("DHT: ignored empty UDP datagram");break;}
                const uint8_t*p=rsp;B*root=bparse(&p,rsp+n,0);B*tx=bget(root,"t"),*y=bget(root,"y"),*r=bget(root,"r");
                if(!root||p!=rsp+n||!tx||tx->t!='s'||tx->n!=4||!y||y->t!='s'||y->n!=1||y->s[0]!='r'||!r||r->t!='d'){bad_packets++;bfree(root);continue;}
                int m=-1;for(int s=0;s<8;s++)if(fl[s].used&&fl[s].tx==be32(tx->s)&&fl[s].ip==from.sin_addr.s_addr&&fl[s].port==ntohs(from.sin_port)){m=s;break;}
                if(m<0){unmatched++;bfree(root);continue;}fl[m].used=0;inflight--;replies++;
                for(size_t i=0;i<count;i++)if(nodes[i].ip==fl[m].ip&&nodes[i].port==fl[m].port){nodes[i].replied=1;B*id=bget(r,"id");if(id&&id->t=='s'&&id->n==20)memcpy(nodes[i].id,id->s,20);}
                B*values=bget(r,"values");if(values&&values->t=='l')for(size_t i=0;i<values->cnt;i++){B*v=values->v[i];if(v->t!='s'||v->n!=6)continue;found++;newp+=add_compact(v->s,6,SRC_DHT);}
                B*list=bget(r,"nodes");if(list&&list->t=='s'&&list->n%26==0)for(size_t i=0;i+26<=list->n;i+=26){uint32_t ip;memcpy(&ip,list->s+i+20,4);uint16_t port=(uint16_t)(list->s[i+24]*256+list->s[i+25]);dht_add(nodes,&count,list->s+i,ip,port,t->ihash);}
                bfree(root);
            }
        }
        LOG("DHT lookup %d: %u queries, %u replies, %u peers (%u new), %u nodes known, %u malformed, %u unmatched, %u send and %u receive errors",runs,queries,replies,found,newp,(unsigned)count,bad_packets,unmatched,send_errors,recv_errors);
        if(!replies)dht_set_status("DHT: no replies (%u queries, %u send/%u receive errors)",queries,send_errors,recv_errors);
        else dht_set_status("DHT: %u replies, %u peers",replies,found);
        pthread_mutex_lock(&st_lock);np=need_peers;pthread_mutex_unlock(&st_lock);
        next_run=now_ms()+(np?60000:300000);
    }
    close(fd);free(nodes);return NULL;
}
/* ---------------- peer wire (many peers, one poll loop) ---------------- */
static int bit(const uint8_t *bf, uint32_t i) { return bf[i >> 3] & (0x80 >> (i & 7)); }
#define MAX_CONN 60
#define MAX_HALF 24
#define MAX_REQ 128
#define MAX_UPQ 32
enum{PS_FREE=0,PS_CONNECTING,PS_HANDSHAKE,PS_ACTIVE};
enum{K_FAIL=0,K_IDLE,K_CHOKED,K_BAN,K_QUIT};
typedef struct{uint32_t piece,off,len;int64_t sent;} preq_t;
typedef struct{
    int fd,state,incoming,cand,ext,ut_pex,reqq,snubbed,dead,kind;char why[96];
    connect_job*connector;
    uint32_t ip;uint16_t port;char addr[24];uint8_t pid[20];
    uint8_t*rbuf;size_t rlen,rcap;uint8_t*wbuf;size_t wlen,wcap;
    uint8_t*bf;uint32_t wanted,npeer;
    int peer_choking,am_interested,peer_interested,am_choking,optimistic,request_window;
    int64_t t_open,last_recv,last_send,since_choked,last_interest,last_block,pick_retry;
    uint64_t got,sent,rate_mark;double rate;
    preq_t rq[MAX_REQ];int nrq;preq_t up[MAX_UPQ];int nup;
} conn_t;
typedef struct{uint32_t idx,len,nblocks,ngot;uint8_t*buf,*got,*rq,*saved;int*from;} active_t;
#define PLOG(...) do{if(peer_log_budget>0){peer_log_budget--;LOG(__VA_ARGS__);}}while(0)

static void conn_kill(conn_t*c,int kind,const char*fmt,...){if(c->dead)return;c->dead=1;c->kind=kind;va_list ap;va_start(ap,fmt);vsnprintf(c->why,sizeof c->why,fmt,ap);va_end(ap);}
static void wq(conn_t*c,const void*d,size_t n){
    if(c->dead)return;
    if(c->wlen+n>c->wcap){size_t nc=c->wcap?c->wcap:4096;while(nc<c->wlen+n)nc*=2;if(nc>(size_t)2*1024*1024){conn_kill(c,K_FAIL,"send buffer overflow");return;}uint8_t*nb=realloc(c->wbuf,nc);if(!nb){conn_kill(c,K_FAIL,"out of memory");return;}c->wbuf=nb;c->wcap=nc;}
    memcpy(c->wbuf+c->wlen,d,n);c->wlen+=n;
}
static void msg(conn_t*c,uint8_t id,const void*payload,uint32_t n){uint8_t h[5];put32(h,n+1);h[4]=id;wq(c,h,5);if(n)wq(c,payload,n);}
static void msg3(conn_t*c,uint8_t id,int cnt,uint32_t a,uint32_t b,uint32_t d){uint8_t p[12];put32(p,a);put32(p+4,b);put32(p+8,d);msg(c,id,p,cnt*4);}
static void conn_flush(conn_t*c,int64_t now){
    while(c->wlen&&!c->dead){ssize_t k=sock_send(c->fd,c->wbuf,c->wlen);
        if(k>0){memmove(c->wbuf,c->wbuf+k,c->wlen-k);c->wlen-=k;c->last_send=now;continue;}
        if(k<0&&(errno==EAGAIN||errno==EINTR))break;int e=k<0?errno:EPIPE;conn_kill(c,K_FAIL,"send: " EFMT,EARG(e));}
}
static int act_find(uint32_t idx){if(!act_flag[idx])return -1;for(int a=0;a<nact;a++)if(act[a].idx==idx)return a;return -1;}
static uint32_t blen(const active_t*p,uint32_t b){return b+1==p->nblocks?p->len-b*BLOCK:BLOCK;}
static void req_drop(conn_t*c,int i,int cancel){
    preq_t r=c->rq[i];int a=act_find(r.piece);if(a>=0){uint32_t b=r.off/BLOCK;if(b<act[a].nblocks&&act[a].rq[b])act[a].rq[b]--;}
    if(cancel)msg3(c,8,3,r.piece,r.off,r.len);c->rq[i]=c->rq[--c->nrq];
}
static int act_new(torrent_t*t,uint32_t idx){
    if(nact>=act_cap)return -1;active_t*p=&act[nact];memset(p,0,sizeof *p);p->idx=idx;p->len=piece_len(t,idx);p->nblocks=(p->len+BLOCK-1)/BLOCK;
    p->buf=malloc(p->len);p->got=calloc(p->nblocks,1);p->rq=calloc(p->nblocks,1);p->saved=calloc(p->nblocks,1);p->from=malloc(p->nblocks*sizeof(int));
    if(!p->buf||!p->got||!p->rq||!p->saved||!p->from){free(p->buf);free(p->got);free(p->rq);free(p->saved);free(p->from);LOG("Out of memory for piece buffer (%u bytes)",p->len);return -1;}
    for(uint32_t b=0;b<p->nblocks;b++)p->from[b]=-1;act_flag[idx]=1;return nact++;
}
static void act_free(int a){active_t*p=&act[a];act_flag[p->idx]=0;free(p->buf);free(p->got);free(p->rq);free(p->saved);free(p->from);memmove(&act[a],&act[a+1],(size_t)(nact-a-1)*sizeof *act);nact--;}
static int checkpoint_save(torrent_t*t,int force);
static int conn_has_req(conn_t*c,uint32_t idx,uint32_t off){for(int i=0;i<c->nrq;i++)if(c->rq[i].piece==idx&&c->rq[i].off==off)return 1;return 0;}
static int pick(torrent_t*t,conn_t*c,uint32_t*idx,uint32_t*off,uint32_t*len){
    for(int a=0;a<nact;a++){active_t*p=&act[a];if(!bit(c->bf,p->idx))continue;
        for(uint32_t b=0;b<p->nblocks;b++)if(!p->got[b]&&!p->rq[b]){*idx=p->idx;*off=b*BLOCK;*len=blen(p,b);p->rq[b]++;return 1;}}
    if(nact<act_cap){int64_t best=-1;
        for(uint32_t i=0;i<t->npieces;i++)if(!t->have[i]&&!act_flag[i]&&bit(c->bf,i)&&(best<0||avail[i]<avail[best]))best=i;
        if(best>=0){int a=act_new(t,(uint32_t)best);if(a>=0){active_t*p=&act[a];*idx=p->idx;*off=0;*len=blen(p,0);p->rq[0]++;return 1;}}}
    if((uint32_t)nact==t->npieces-g_nhave)
        for(int a=0;a<nact;a++){active_t*p=&act[a];if(!bit(c->bf,p->idx))continue;
            for(uint32_t b=0;b<p->nblocks;b++)if(!p->got[b]&&p->rq[b]<3&&!conn_has_req(c,p->idx,b*BLOCK)){*idx=p->idx;*off=b*BLOCK;*len=blen(p,b);p->rq[b]++;return 1;}}
    return 0;
}
static void fill_requests(torrent_t*t,conn_t*c,int64_t now){
    if(c->dead||c->state!=PS_ACTIVE||c->peer_choking||!c->am_interested||now<c->pick_retry)return;
    int target=16+(int)(c->rate*1.5/BLOCK);if(target>MAX_REQ)target=MAX_REQ;if(target>c->reqq)target=c->reqq;if(c->snubbed)target=1;
    if(!c->request_window)PLOG("Peer %s: request window %d blocks (%d KB), remote limit %d",c->addr,target,target*BLOCK/1024,c->reqq);c->request_window=target;
    while(c->nrq<target&&!c->dead){uint32_t i,o,l;if(!pick(t,c,&i,&o,&l)){c->pick_retry=now+1000;break;}
        preq_t*r=&c->rq[c->nrq++];r->piece=i;r->off=o;r->len=l;r->sent=now;msg3(c,6,3,i,o,l);}
}
static void report_now(const char*phase,unsigned peers){if(callbacks&&callbacks->progress)callbacks->progress(callbacks->user,phase,g_have_bytes,g_t->total,peers);if(callbacks&&callbacks->traffic)callbacks->traffic(callbacks->user,stats.payload_bytes);}
static unsigned active_peers(void){unsigned n=0;for(int k=0;k<MAX_CONN;k++)if(conns[k].state==PS_ACTIVE&&!conns[k].dead)n++;return n;}
static void piece_done(torrent_t*t,int a,int64_t now){
    active_t*p=&act[a];uint8_t h[20];sha1(p->buf,p->len,h);uint32_t idx=p->idx;
    if(memcmp(h,t->hashes+(size_t)idx*20,20)){
        stats.hashfail++;LOG("Piece %u failed hash check; downloading it again",idx);
        for(uint32_t b=0;b<p->nblocks;b++){int ci=p->from[b];if(ci<0)continue;int dup=0;for(uint32_t k=0;k<b&&!dup;k++)dup=p->from[k]==ci;if(dup)continue;
            int ban=0;pthread_mutex_lock(&st_lock);if((size_t)ci<ncands){cands[ci].hashfail++;ban=cands[ci].hashfail>=6;}pthread_mutex_unlock(&st_lock);
            if(ban)for(int k=0;k<MAX_CONN;k++)if(conns[k].state&&conns[k].cand==ci)conn_kill(&conns[k],K_BAN,"sent corrupt data repeatedly");}
        memset(p->got,0,p->nblocks);memset(p->saved,0,p->nblocks);memset(p->rq,0,p->nblocks);for(uint32_t b=0;b<p->nblocks;b++)p->from[b]=-1;p->ngot=0;
        for(int k=0;k<MAX_CONN;k++){conn_t*c=&conns[k];for(int i=c->nrq-1;i>=0;i--)if(c->rq[i].piece==idx)c->rq[i]=c->rq[--c->nrq];c->pick_retry=0;}
        return;
    }
    if(io_range(t,(uint64_t)idx*t->plen,p->buf,p->len,1)){LOG("Disk write failed for piece %u: " EFMT,idx,EARG(errno));g_halt=1;return;}
    uint32_t plen=p->len;act_free(a);t->have[idx]=1;g_nhave++;g_have_bytes+=plen;
    for(int k=0;k<MAX_CONN;k++){conn_t*c=&conns[k];c->pick_retry=0;if(c->state!=PS_ACTIVE||c->dead)continue;msg3(c,4,1,idx,0,0);
        if(bit(c->bf,idx)&&c->wanted){c->wanted--;if(!c->wanted&&c->am_interested){c->am_interested=0;msg(c,3,NULL,0);}}}
    if(g_nhave==1||t->npieces<=100||g_nhave%(t->npieces/100)==0||g_nhave==t->npieces)LOG("Piece %u ok (%u/%u pieces, %llu/%llu bytes)",idx,g_nhave,t->npieces,(unsigned long long)g_have_bytes,(unsigned long long)t->total);
    if(t->resume&&(!t->resume->first_piece_saved||now>=t->resume->save_at)){checkpoint_save(t,1);t->resume->first_piece_saved=1;}
    report_now("Downloading",active_peers());
    if(stopped()){g_stopped=1;g_halt=1;}
    (void)now;
}
static void bf_merge(torrent_t*t,conn_t*c,const uint8_t*src){
    for(uint32_t i=0;i<t->npieces;i++)if(bit(src,i)&&!bit(c->bf,i)){c->bf[i>>3]|=0x80>>(i&7);avail[i]++;c->npeer++;if(!t->have[i])c->wanted++;}
    c->pick_retry=0;
}
static void handle_ext(torrent_t*t,conn_t*c,const uint8_t*m,uint32_t n){
    if(n<2)return;const uint8_t*p=m+2;B*d=bparse(&p,m+n,0);if(!d||d->t!='d'){bfree(d);return;}
    if(m[1]==0){B*mm=bget(d,"m"),*px=bget(mm,"ut_pex"),*rq=bget(d,"reqq"),*v=bget(d,"v");
        if(px&&px->t=='i'&&px->i>0&&px->i<256)c->ut_pex=(int)px->i;if(rq&&rq->t=='i'&&rq->i>0)c->reqq=rq->i>250?250:(int)rq->i;
        if(v&&v->t=='s')PLOG("Peer %s: client %.*s",c->addr,(int)(v->n>40?40:v->n),v->s);}
    else if(m[1]==1&&!g_private){B*ad=bget(d,"added");if(ad&&ad->t=='s'){size_t lim=ad->n>300?300:ad->n;unsigned k=add_compact(ad->s,lim-lim%6,SRC_PEX);if(k)PLOG("Peer %s: PEX gave %u new peers",c->addr,k);}}
    bfree(d);(void)t;
}
static void handle_msg(torrent_t*t,conn_t*c,const uint8_t*m,uint32_t n,int64_t now){
    switch(m[0]){
    case 0:if(!c->peer_choking){c->peer_choking=1;c->since_choked=now;while(c->nrq)req_drop(c,c->nrq-1,0);}break;
    case 1:if(c->peer_choking){c->peer_choking=0;c->pick_retry=0;PLOG("Peer %s unchoked us after %ds",c->addr,(int)((now-c->t_open)/1000));}break;
    case 2:c->peer_interested=1;break;
    case 3:c->peer_interested=0;c->nup=0;break;
    case 4:if(n>=5){uint32_t i=be32(m+1);if(i<t->npieces&&!bit(c->bf,i)){c->bf[i>>3]|=0x80>>(i&7);avail[i]++;c->npeer++;if(!t->have[i]){c->wanted++;c->pick_retry=0;}}}break;
    case 5:if(n-1<(t->npieces+7)/8){conn_kill(c,K_FAIL,"bad bitfield length");break;}bf_merge(t,c,m+1);break;
    case 0x0E:{size_t bl=(t->npieces+7)/8;uint8_t*all=malloc(bl);if(all){memset(all,0xFF,bl);bf_merge(t,c,all);free(all);}}break;
    case 0x0F:break;
    case 6:if(n>=13&&!c->am_choking&&c->nup<MAX_UPQ){uint32_t i=be32(m+1),o=be32(m+5),l=be32(m+9);if(i<t->npieces&&t->have[i]&&l&&l<=32768&&o<=piece_len(t,i)&&l<=piece_len(t,i)-o){preq_t*r=&c->up[c->nup++];r->piece=i;r->off=o;r->len=l;r->sent=now;}}break;
    case 8:if(n>=13){uint32_t i=be32(m+1),o=be32(m+5);for(int k=0;k<c->nup;k++)if(c->up[k].piece==i&&c->up[k].off==o){memmove(&c->up[k],&c->up[k+1],(size_t)(c->nup-k-1)*sizeof c->up[0]);c->nup--;break;}}break;
    case 0x10:if(n>=13){uint32_t i=be32(m+1),o=be32(m+5);for(int k=0;k<c->nrq;k++)if(c->rq[k].piece==i&&c->rq[k].off==o){req_drop(c,k,0);break;}c->pick_retry=now+2000;}break;
    case 7:{if(n<9)break;uint32_t idx=be32(m+1),off=be32(m+5),len=n-9;
        for(int k=0;k<c->nrq;k++)if(c->rq[k].piece==idx&&c->rq[k].off==off){req_drop(c,k,0);break;}
        int a=idx<t->npieces?act_find(idx):-1;if(a<0||off%BLOCK)break;active_t*p=&act[a];uint32_t b=off/BLOCK;if(b>=p->nblocks||len!=blen(p,b)||p->got[b])break;
        memcpy(p->buf+off,m+9,len);p->got[b]=1;p->ngot++;p->from[b]=c->cand;
        if(!c->got){pthread_mutex_lock(&st_lock);if(c->cand>=0&&(size_t)c->cand<ncands){cands[c->cand].worked=1;cands[c->cand].fails=0;cands[c->cand].seen=(uint32_t)time(NULL);}pthread_mutex_unlock(&st_lock);}
        if(!stats.blocks)LOG("First payload block: peer %s, piece=%u offset=%u bytes=%u after %lldms",c->addr,idx,off,len,(long long)(now-log_t0));
        if(!c->got)PLOG("Peer %s: first block after %ds",c->addr,(int)((now-c->t_open)/1000));
        c->got+=len;c->last_block=now;g_last_block=now;c->snubbed=0;stats.blocks++;stats.payload_bytes+=len;
        if(p->rq[b])for(int k=0;k<MAX_CONN;k++){conn_t*o=&conns[k];if(o==c||o->state!=PS_ACTIVE)continue;for(int i=0;i<o->nrq;i++)if(o->rq[i].piece==idx&&o->rq[i].off==off){req_drop(o,i,1);break;}}
        if(p->ngot==p->nblocks)piece_done(t,a,now);
        break;}
    case 20:handle_ext(t,c,m,n);break;
    default:break;
    }
    if(!c->dead&&c->wanted&&!c->am_interested){c->am_interested=1;msg(c,2,NULL,0);if(c->peer_choking)c->since_choked=now;}
}
static void send_handshake(conn_t*c){uint8_t hs[68]={19};memcpy(hs+1,"BitTorrent protocol",19);hs[25]=0x10;memcpy(hs+28,g_t->ihash,20);memcpy(hs+48,peer_id,20);wq(c,hs,68);}
static void conn_handshake(torrent_t*t,conn_t*c,int64_t now){
    const uint8_t*h=c->rbuf;
    if(h[0]!=19||memcmp(h+1,"BitTorrent protocol",19)){stats.hs_fail++;conn_kill(c,K_FAIL,"not a plain BitTorrent handshake (encrypted peer?)");return;}
    if(memcmp(h+28,t->ihash,20)){stats.hs_fail++;conn_kill(c,K_FAIL,"peer is serving a different torrent");return;}
    if(!memcmp(h+48,peer_id,20)){stats.self++;conn_kill(c,K_BAN,"connected to ourselves");return;}
    for(int k=0;k<MAX_CONN;k++){conn_t*o=&conns[k];if(o!=c&&o->state==PS_ACTIVE&&!o->dead&&!memcmp(o->pid,h+48,20)){conn_kill(c,K_IDLE,"duplicate connection");return;}}
    memcpy(c->pid,h+48,20);c->ext=(h[25]&0x10)!=0;
    c->bf=calloc((t->npieces+7)/8,1);if(!c->bf){conn_kill(c,K_FAIL,"out of memory");return;}
    c->state=PS_ACTIVE;c->peer_choking=1;c->am_choking=1;c->reqq=64;c->since_choked=now;c->last_interest=now;c->last_recv=now;stats.handshakes++;
    if(c->incoming)send_handshake(c);
    if(c->ext){char e[128];unsigned lp;pthread_mutex_lock(&st_lock);lp=listen_port;pthread_mutex_unlock(&st_lock);
        int el=g_private?snprintf(e,sizeof e,"d1:mde1:pi%ue4:reqqi250e1:v12:NULL_store 1e",lp):snprintf(e,sizeof e,"d1:md6:ut_pexi1ee1:pi%ue4:reqqi250e1:v12:NULL_store 1e",lp);uint8_t hdr[6];put32(hdr,(uint32_t)el+2);hdr[4]=20;hdr[5]=0;wq(c,hdr,6);wq(c,e,el);}
    if(g_nhave){size_t bl=(t->npieces+7)/8;uint8_t*bf=calloc(bl,1);if(bf){for(uint32_t i=0;i<t->npieces;i++)if(t->have[i])bf[i>>3]|=0x80>>(i&7);msg(c,5,bf,(uint32_t)bl);free(bf);}}
    if(stats.handshakes<=3)LOG("Peer %s: handshake ok after %dms%s%s",c->addr,(int)(now-c->t_open),c->ext?", extensions":"",c->incoming?", incoming":"");
    else PLOG("Peer %s: handshake ok after %dms%s%s",c->addr,(int)(now-c->t_open),c->ext?", extensions":"",c->incoming?", incoming":"");
}
static void conn_process(torrent_t*t,conn_t*c,int64_t now){
    size_t o=0;
    if(c->state==PS_HANDSHAKE){if(c->rlen<68){if(c->rlen&&c->rbuf[0]!=19){stats.hs_fail++;conn_kill(c,K_FAIL,"not a plain BitTorrent handshake (encrypted peer?)");}return;}conn_handshake(t,c,now);o=68;}
    while(!c->dead&&!g_halt&&c->state==PS_ACTIVE&&c->rlen-o>=4){
        uint32_t L=be32(c->rbuf+o);if(L>g_maxmsg){conn_kill(c,K_FAIL,"oversized message (%u bytes)",L);break;}
        if(c->rlen-o<4+(size_t)L)break;
        if(L)handle_msg(t,c,c->rbuf+o+4,L,now);
        o+=4+(size_t)L;
    }
    if(o){memmove(c->rbuf,c->rbuf+o,c->rlen-o);c->rlen-=o;}
    if(c->rlen>=4&&c->state==PS_ACTIVE){size_t need=4+(size_t)be32(c->rbuf);if(need>c->rcap&&need<=4+(size_t)g_maxmsg){uint8_t*nb=realloc(c->rbuf,need);if(!nb)conn_kill(c,K_FAIL,"out of memory");else{c->rbuf=nb;c->rcap=need;}}}
}
static void conn_read(torrent_t*t,conn_t*c,int64_t now){
    for(int rounds=0;rounds<8&&!c->dead&&!g_halt;rounds++){
        if(c->rlen==c->rcap){conn_process(t,c,now);if(c->rlen==c->rcap)break;continue;}
        ssize_t k=recv(c->fd,c->rbuf+c->rlen,c->rcap-c->rlen,MSG_DONTWAIT);
        if(k==0){conn_process(t,c,now);if(c->dead||g_halt)break;if(c->state==PS_HANDSHAKE)stats.hs_fail++;conn_kill(c,K_FAIL,c->state==PS_HANDSHAKE?"closed by peer during handshake":"closed by peer");break;}
        if(k<0){if(errno==EAGAIN||errno==EINTR)break;int e=errno;conn_process(t,c,now);if(c->dead||g_halt)break;if(c->state==PS_HANDSHAKE)stats.hs_fail++;conn_kill(c,K_FAIL,"recv: " EFMT,EARG(e));break;}
        c->rlen+=(size_t)k;c->last_recv=now;
    }
    if(!g_halt)conn_process(t,c,now);
}
static void conn_close(torrent_t*t,conn_t*c,int64_t now){
    if(c->state==PS_FREE)return;
    if(c->connector){connect_job_cancel(c->connector);c->connector=NULL;PLOG("Peer %s: connector cancelled; no session state is retained by worker",c->addr);}
    while(c->nrq)req_drop(c,c->nrq-1,0);
    if(c->bf){for(uint32_t i=0;i<t->npieces;i++)if(bit(c->bf,i)&&avail[i])avail[i]--;free(c->bf);}
    free(c->rbuf);free(c->wbuf);if(c->fd>=0)close(c->fd);
    pthread_mutex_lock(&st_lock);
    if(c->cand>=0&&(size_t)c->cand<ncands){cand_t*cd=&cands[c->cand];
        if(c->kind==K_BAN){cd->state=C_BANNED;stats.banned++;}
        else if(cd->state!=C_BANNED){cd->state=C_IDLE;
            if(c->got){cd->fails=0;cd->worked=1;cd->retry_at=now+10000;}
            else if(c->kind==K_IDLE)cd->retry_at=now+300000;
            else if(c->kind==K_CHOKED)cd->retry_at=now+120000;
            else if(c->kind==K_QUIT){}
            else{if(cd->fails<255)cd->fails++;unsigned shift=cd->fails?cd->fails-1:0;if(shift>3)shift=3;cd->retry_at=now+(15000LL<<shift);}}}
    pthread_mutex_unlock(&st_lock);
    if(c->kind!=K_QUIT)PLOG("Peer %s closed: %s (received %llu bytes, %ds)",c->addr,c->why,(unsigned long long)c->got,(int)((now-c->t_open)/1000));
    memset(c,0,sizeof *c);c->fd=-1;
}
static int conn_slot(void){for(int k=0;k<conn_cap&&k<MAX_CONN;k++)if(conns[k].state==PS_FREE)return k;return -1;}
static void conn_init(conn_t*c,int fd,uint32_t ip,uint16_t port,int cand,int state,int64_t now){
    memset(c,0,sizeof *c);c->fd=fd;c->ip=ip;c->port=port;c->cand=cand;c->state=state;addr_str(c->addr,ip,port);
    c->t_open=c->last_recv=c->last_send=now;c->rcap=65536;c->rbuf=malloc(c->rcap);if(!c->rbuf)conn_kill(c,K_FAIL,"out of memory");
}
static void note_connect_error(conn_t*c,int e){
    if(e==ECONNREFUSED)stats.refused++;else if(e==ETIMEDOUT)stats.timeout++;else if(e==ENETUNREACH||e==EHOSTUNREACH)stats.unreach++;else stats.other++;
    conn_kill(c,K_FAIL,"connect: " EFMT,EARG(e));
}
/* 1 started, 0 capacity busy (stop scheduling this turn), -1 target deferred. */
static int conn_start(int ci,int64_t now){
    int k=conn_slot();if(k<0)return 0;
    pthread_mutex_lock(&st_lock);uint32_t ip=cands[ci].ip;uint16_t port=cands[ci].port;cands[ci].state=C_ACTIVE;pthread_mutex_unlock(&st_lock);
    int fd=nb_socket(AF_INET,SOCK_STREAM);
    if(fd<0){int e=errno,limited=e==EMFILE||e==ENOBUFS||e==ENFILE;if(limited){int nc=0;for(int j=0;j<MAX_CONN;j++)if(conns[j].state)nc++;conn_cap=nc>8?nc:8;LOG("Socket limit reached (" EFMT "); limiting to %d connections",EARG(e),conn_cap);}
        pthread_mutex_lock(&st_lock);cands[ci].state=C_IDLE;cands[ci].retry_at=now+15000;pthread_mutex_unlock(&st_lock);return limited?0:-1;}
    no_sigpipe(fd);
    connect_job*j=NULL;
    if(socket_compat){
        j=connect_job_start(fd,ip,port);
        if(!j){
            int e=errno,busy=e==EAGAIN;
            if(busy){session_current()->ctx_connect_retry_at=now+500;if(now-session_current()->ctx_connect_limit_log>=5000){session_current()->ctx_connect_limit_log=now;LOG("Connector capacity busy: %d active; scheduling waits 500ms instead of discarding candidates",__atomic_load_n(&connect_workers,__ATOMIC_ACQUIRE));}}
            else if(e!=EALREADY)LOG("Cannot start peer connector: " EFMT,EARG(e));
            pthread_mutex_lock(&st_lock);cands[ci].state=C_IDLE;cands[ci].retry_at=now+(busy?500:e==EALREADY?2000:15000);pthread_mutex_unlock(&st_lock);return busy?0:-1;
        }
    }
    stats.attempts++;conn_t*c=&conns[k];conn_init(c,j?-1:fd,ip,port,ci,PS_CONNECTING,now);c->connector=j;
    if(j){PLOG("Peer %s: compatibility connect worker started",c->addr);return 1;}
    struct sockaddr_in sa;memset(&sa,0,sizeof sa);portalAddress4(&sa,ip,port);
    PLOG("Peer %s: connect started",c->addr);
    if(connect(fd,(struct sockaddr*)&sa,sizeof sa)==0){stats.ok++;c->state=PS_HANDSHAKE;send_handshake(c);}
    else if(errno!=EINPROGRESS&&errno!=EINTR&&errno!=EALREADY&&errno!=EWOULDBLOCK&&errno!=EAGAIN)note_connect_error(c,errno);
    return 1;
}
static void conn_connect_finish(conn_t*c,int64_t now){
    if(!c->connector)return;
    int fd,e;int64_t elapsed;
    if(!connect_job_take(c->connector,&fd,&e,&elapsed))return;
    c->connector=NULL;c->fd=fd;
    PLOG("Peer %s: compatibility connect completed in %lldms: " EFMT,c->addr,(long long)elapsed,EARG(e));
    if(e){note_connect_error(c,e);return;}
    stats.ok++;c->state=PS_HANDSHAKE;c->last_recv=now;send_handshake(c);conn_flush(c,now);
}
static void tracker_summary(char*out,size_t cap){
    out[0]=0;size_t n=0;pthread_mutex_lock(&st_lock);
    if(!ntrk)n+=snprintf(out,cap,"none");
    for(size_t i=0;i<ntrk&&n+20<cap;i++){int w=snprintf(out+n,cap-n,"%s%s: %s",i?"; ":"",trk[i].host,trk[i].status[0]?trk[i].status:"pending");if(w<0)break;n+=(size_t)w;if(n>=cap){out[cap-1]=0;break;}}
    pthread_mutex_unlock(&st_lock);
}
#include "torrent_checkpoint.h"
static void make_listener(int*lfd){
    if(socket_compat){*lfd=-1;LOG("Incoming listener disabled in compatibility mode; outgoing peer connections remain enabled");return;}
    *lfd=-1;for(int p=6881;p<=6890;p++){
        int fd=nb_socket(AF_INET,SOCK_STREAM);if(fd<0){int e=errno;LOG("Listener socket failed: " EFMT,EARG(e));return;}
        no_sigpipe(fd);int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);struct sockaddr_in sa;memset(&sa,0,sizeof sa);
#ifdef PORTAL_HOST
        struct in_addr lo;inet_pton(AF_INET,"127.0.0.1",&lo);portalAddress4(&sa,lo.s_addr,p==6890?0:p);
#else
        portalAddress4(&sa,htonl(INADDR_ANY),p==6890?0:p);
#endif
        if(bind(fd,(struct sockaddr*)&sa,sizeof sa)==0&&listen(fd,16)==0){socklen_t sl=sizeof sa;getsockname(fd,(struct sockaddr*)&sa,&sl);
            pthread_mutex_lock(&st_lock);listen_port=ntohs(sa.sin_port);pthread_mutex_unlock(&st_lock);*lfd=fd;LOG("Listening for incoming peers on TCP port %u",ntohs(sa.sin_port));return;}
        close(fd);
    }
    LOG("Cannot listen for incoming peers; continuing with outgoing connections only");
}
static void tracker_host(const char*url,char out[64]){const char*h=strstr(url,"://");h=h?h+3:url;size_t n=0;while(h[n]&&h[n]!=':'&&h[n]!='/'&&h[n]!='?'&&n<63)n++;memcpy(out,h,n);out[n]=0;}
static int setup_trackers(torrent_t*t,int priv){
    static const char*defaults[]={"udp://tracker.opentrackr.org:1337/announce","udp://open.stealth.si:80/announce","udp://tracker.torrent.eu.org:451/announce","udp://exodus.desync.com:6969/announce","udp://open.demonii.com:1337/announce"};
    size_t nd=sizeof defaults/sizeof defaults[0];
#ifdef PORTAL_HOST
    nd=0;
#endif
    if(priv)nd=0;
    trk=calloc(t->ntrackers+nd+1,sizeof *trk);if(!trk)return -1;ntrk=0;
    for(size_t i=0;i<t->ntrackers;i++){
        char host[64];tracker_host(t->trackers[i],host);size_t hn=strlen(host);
        if(hn>=6&&!strcmp(host+hn-6,".local")){LOG("Skipping provider-local tracker %s",host);continue;}
        trk[ntrk].url=t->trackers[i];ntrk++;
    }
    for(size_t i=0;i<nd;i++){int dup=0;for(size_t j=0;j<t->ntrackers;j++)if(!strcmp(t->trackers[j],defaults[i]))dup=1;if(!dup){trk[ntrk].url=defaults[i];ntrk++;}}
    for(size_t i=0;i<ntrk;i++){trk[i].udp=!strncmp(trk[i].url,"udp://",6);tracker_host(trk[i].url,trk[i].host);trk[i].interval=1800;trk[i].seeders=trk[i].leechers=-1;}
    return 0;
}
static int swarm(torrent_t*t){
    B*pf=bget(bget(t->root,"info"),"private");int priv=pf&&pf->t=='i'&&pf->i==1;
    g_t=t;__atomic_store_n(&g_quit,0,__ATOMIC_RELEASE);g_halt=g_stopped=0;g_private=priv;g_last_block=0;g_up_bytes=0;memset(&stats,0,sizeof stats);
    conns=calloc(MAX_CONN,sizeof *conns);if(!conns){fail("Out of memory allocating peer slots");return -1;}
    memset(conns,0,MAX_CONN*sizeof *conns);
    for(int k=0;k<MAX_CONN;k++)conns[k].fd=-1;conn_cap=MAX_CONN;peer_log_budget=60;
    size_t bl=(t->npieces+7)/8;g_maxmsg=(uint32_t)((1+bl>9+131072?1+bl:9+131072)+16);
    act_cap=(int)(16u*1024*1024/t->plen);if(act_cap<1)act_cap=1;if(act_cap>64)act_cap=64;
    pthread_mutex_lock(&st_lock);cands=calloc(MAX_CAND,sizeof *cands);ncands=0;memset(src_count,0,sizeof src_count);need_peers=1;listen_port=0;
    st_left=t->total-g_have_bytes;st_down=0;st_up=0;tracker_key=(session_random()<<16)^session_random();snprintf(dht_status,sizeof dht_status,priv?"DHT off (private torrent)":"DHT starting");pthread_mutex_unlock(&st_lock);
    act=calloc((size_t)act_cap,sizeof *act);act_flag=calloc(t->npieces,1);avail=calloc(t->npieces,sizeof *avail);nact=0;
    int rc=-1,lfd=-1,resume_ready=0;int64_t shutdown_start=0;pthread_t th[3];int th_ok[3]={0,0,0};
    if(!cands||!act||!act_flag||!avail||setup_trackers(t,priv)){fail("Out of memory starting torrent");goto cleanup;}
    if(checkpoint_restore(t)){if(stopped())g_stopped=1;goto cleanup;}resume_ready=1;checkpoint_save(t,1);
    if(init_socket_transport()){fail("Socket transport probe failed; see torrent.log");goto cleanup;}
    make_listener(&lfd);
    LOG("Swarm start: %u trackers (%s), DHT %s, max %d connections, %d active pieces",(unsigned)ntrk,priv?"private":"public",priv?"off":"on",MAX_CONN,act_cap);
    for(size_t i=0;i<ntrk;i++)LOG("  tracker %u: %s",(unsigned)i+1,trk[i].url);
    {pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256*1024);
     int hh=0,hu=0;for(size_t i=0;i<ntrk;i++){if(trk[i].udp)hu=1;else hh=1;}
     void*(*fn[3])(void*)={tracker_http_thread,tracker_udp_thread,dht_thread};int want[3]={hh,hu,!priv};const char*nm[3]={"HTTP tracker","UDP tracker","DHT"};
     for(int i=0;i<3;i++)if(want[i]){int e=pthread_create(&th[i],&attr,fn[i],session_current());if(e)LOG("Cannot start %s thread: " EFMT,nm[i],EARG(e));else th_ok[i]=1;}
     pthread_attr_destroy(&attr);}
    int64_t start=now_ms(),last_stop=0,last_report=0,last_status=start,last_choke=0,last_opt=0,last_rate=start,last_eval=0,last_budget=start;
    unsigned eligible=0;size_t known=0;uint64_t rate_mark=0;double g_rate=0;int cursor=0;
    struct pollfd pfd[MAX_CONN+1];int pmap[MAX_CONN+1];
    while(!g_halt&&!io_failed){
        int64_t now=now_ms();
        if(now-last_stop>=50){last_stop=now;if(stopped()){g_stopped=1;LOG("Stop request observed; closing active peer and tracker workers");break;}}
        checkpoint_save(t,0);
        if(now-last_budget>=60000){last_budget=now;peer_log_budget+=20;if(peer_log_budget>60)peer_log_budget=60;}
        if(now-last_eval>=1000){last_eval=now;unsigned useful=0;for(int k=0;k<MAX_CONN;k++)if(conns[k].state==PS_ACTIVE&&conns[k].wanted)useful++;
            pthread_mutex_lock(&st_lock);eligible=0;known=ncands;for(size_t i=0;i<ncands;i++)if(cands[i].state==C_IDLE&&cands[i].retry_at<=now)eligible++;
            need_peers=useful<MIN_USEFUL_PEERS&&eligible<MIN_ELIGIBLE_PEERS;st_left=t->total-g_have_bytes;st_up=g_up_bytes;pthread_mutex_unlock(&st_lock);}
        /* timeouts */
        for(int k=0;k<MAX_CONN;k++){conn_t*c=&conns[k];if(!c->state||c->dead)continue;
            conn_connect_finish(c,now);if(c->dead)continue;
            if(c->state==PS_CONNECTING&&now-c->t_open>10000){stats.timeout++;conn_kill(c,K_FAIL,"connect timed out after 10s");continue;}
            if(c->state==PS_HANDSHAKE&&now-c->last_recv>10000){stats.hs_fail++;conn_kill(c,K_FAIL,"handshake timed out after 10s without data");continue;}
            if(c->state!=PS_ACTIVE)continue;
            if(now-c->last_recv>180000){conn_kill(c,K_FAIL,"peer silent for 180s");continue;}
            if(c->am_interested||c->peer_interested)c->last_interest=now;
            if(c->peer_choking&&c->am_interested&&now-c->since_choked>150000&&eligible){stats.choked_out++;conn_kill(c,K_CHOKED,"kept us choked for 150s");continue;}
            if(!c->am_interested&&!c->peer_interested&&now-c->last_interest>120000&&eligible){stats.idle_out++;conn_kill(c,K_IDLE,"has no pieces we need");continue;}
            for(int i=c->nrq-1;i>=0;i--)if(now-c->rq[i].sent>60000){if(!c->snubbed)PLOG("Peer %s: request timed out after 60s; reducing its queue",c->addr);c->snubbed=1;req_drop(c,i,1);}
            if(now-c->last_send>90000){uint8_t z[4]={0,0,0,0};wq(c,z,4);}
        }
        /* new outgoing connections */
        {int open=0,half=0;for(int k=0;k<MAX_CONN;k++)if(conns[k].state){open++;if(conns[k].state!=PS_ACTIVE&&!conns[k].incoming)half++;}
         int max_half=socket_compat?12:MAX_HALF;
         for(int attempted=0;now>=session_current()->ctx_connect_retry_at&&open<conn_cap&&half<max_half&&attempted<8;attempted++){
            int ci=-1;pthread_mutex_lock(&st_lock);
            for(int pass=0;pass<3&&ci<0;pass++)for(size_t s=0;s<ncands;s++){size_t i=(cursor+s)%ncands;cand_t*cd=&cands[i];if(cd->state!=C_IDLE||cd->retry_at>now)continue;if(pass==0&&!cd->worked)continue;if(pass==1&&cd->fails)continue;ci=(int)i;cursor=(int)((i+1)%ncands);break;}
            pthread_mutex_unlock(&st_lock);
            if(ci<0)break;int began=conn_start(ci,now);if(!began)break;if(began>0){open++;half++;}}}
        /* choking: reciprocate with the 4 best peers plus one optimistic slot */
        if(now-last_choke>=10000){last_choke=now;int order[MAX_CONN],n=0;
            for(int k=0;k<MAX_CONN;k++)if(conns[k].state==PS_ACTIVE&&!conns[k].dead&&conns[k].peer_interested)order[n++]=k;
            for(int i=0;i<n;i++)for(int j=i+1;j<n;j++)if(conns[order[j]].rate>conns[order[i]].rate){int x=order[i];order[i]=order[j];order[j]=x;}
            if(now-last_opt>=30000&&n>4){last_opt=now;for(int k=0;k<MAX_CONN;k++)conns[k].optimistic=0;conns[order[4+session_random()%(n-4)]].optimistic=1;}
            for(int i=0;i<n;i++){conn_t*c=&conns[order[i]];int want=i<4||c->optimistic;if(want&&c->am_choking){c->am_choking=0;msg(c,1,NULL,0);}else if(!want&&!c->am_choking){c->am_choking=1;c->nup=0;msg(c,0,NULL,0);}}
            for(int k=0;k<MAX_CONN;k++){conn_t*c=&conns[k];if(c->state==PS_ACTIVE&&!c->peer_interested&&!c->am_choking){c->am_choking=1;c->nup=0;msg(c,0,NULL,0);}}}
        if(now-last_rate>=2000){double dt=(double)(now-last_rate);last_rate=now;for(int k=0;k<MAX_CONN;k++){conn_t*c=&conns[k];if(!c->state)continue;double inst=(double)(c->got-c->rate_mark)*1000.0/dt;c->rate=c->rate*0.6+inst*0.4;c->rate_mark=c->got;}
            double inst=(double)(stats.payload_bytes-rate_mark)*1000.0/dt;g_rate=g_rate*0.6+inst*0.4;rate_mark=stats.payload_bytes;}
        for(int k=0;k<MAX_CONN;k++){conn_t*c=&conns[k];if(c->state&&!c->dead){fill_requests(t,c,now);if(c->state!=PS_CONNECTING)conn_flush(c,now);}}
        /* poll */
        int np=0;if(lfd>=0){pfd[np].fd=lfd;pfd[np].events=POLLIN;pfd[np].revents=0;pmap[np++]=-1;}
        for(int k=0;k<MAX_CONN;k++){conn_t*c=&conns[k];if(!c->state||c->dead||c->connector)continue;pfd[np].fd=c->fd;pfd[np].events=c->state==PS_CONNECTING?POLLOUT:(short)(POLLIN|(c->wlen?POLLOUT:0));pfd[np].revents=0;pmap[np++]=k;}
        int pr=poll(pfd,(nfds_t)np,100);
        if(pr<0&&errno!=EINTR){int e=errno;LOG("poll failed: " EFMT,EARG(e));sleep_ms(100);}
        now=now_ms();
        for(int i=0;pr>0&&i<np&&!g_halt;i++){if(!pfd[i].revents)continue;
            if(pmap[i]<0){for(int a=0;a<16;a++){struct sockaddr_in from;socklen_t fl=sizeof from;int fd=nb_accept(lfd,(struct sockaddr*)&from,&fl);if(fd<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){int e=errno;LOG("Peer accept failed: " EFMT,EARG(e));}break;}
                    int k=conn_slot();if(k<0){close(fd);continue;}no_sigpipe(fd);stats.incoming++;conn_init(&conns[k],fd,from.sin_addr.s_addr,ntohs(from.sin_port),-1,PS_HANDSHAKE,now);conns[k].incoming=1;}
                continue;}
            conn_t*c=&conns[pmap[i]];if(!c->state||c->dead)continue;
            if(c->state==PS_CONNECTING){int e=0;socklen_t el=sizeof e;if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&e,&el)<0)e=errno;
                if(e){note_connect_error(c,e);continue;}if(!(pfd[i].revents&POLLOUT))continue;stats.ok++;c->state=PS_HANDSHAKE;c->last_recv=now;send_handshake(c);conn_flush(c,now);continue;}
            if(pfd[i].revents&(POLLIN|POLLHUP|POLLERR))conn_read(t,c,now);
            if(g_halt)break;
            if(!c->dead){fill_requests(t,c,now);
                while(c->nup&&c->wlen<128*1024&&!c->dead&&!c->am_choking){preq_t r=c->up[0];memmove(c->up,c->up+1,(size_t)(--c->nup)*sizeof r);uint8_t*blk=malloc(13+r.len);if(!blk)break;
                    put32(blk,9+r.len);blk[4]=7;put32(blk+5,r.piece);put32(blk+9,r.off);if(io_range(t,(uint64_t)r.piece*t->plen+r.off,blk+13,r.len,0)){free(blk);break;}wq(c,blk,13+r.len);free(blk);c->sent+=r.len;g_up_bytes+=r.len;}
                conn_flush(c,now);}
        }
        if(g_halt)break;
        for(int k=0;k<MAX_CONN;k++)if(conns[k].state&&conns[k].dead)conn_close(t,&conns[k],now);
        if(g_nhave==t->npieces)break;
        /* progress */
        if(now-last_report>=500){last_report=now;unsigned peers=0,choking=0;for(int k=0;k<MAX_CONN;k++)if(conns[k].state==PS_ACTIVE){peers++;if(conns[k].peer_choking)choking++;}
            char ph[200];
            if(g_last_block&&now-g_last_block<10000)snprintf(ph,sizeof ph,"Downloading");
            else if(!known){unsigned ok=0,bad=0;char ds[120];pthread_mutex_lock(&st_lock);for(size_t i=0;i<ntrk;i++){if(trk[i].last_success)ok++;else if(trk[i].fails)bad++;}snprintf(ds,sizeof ds,"%s",dht_status);pthread_mutex_unlock(&st_lock);
                snprintf(ph,sizeof ph,"Finding peers (trackers: %u ok, %u failing; %s)",ok,bad,ds);}
            else if(!peers)snprintf(ph,sizeof ph,"Connecting to peers: 0 connected, %u known",(unsigned)known);
            else snprintf(ph,sizeof ph,"Waiting for peers to send data: %u connected, %u choking us",peers,choking);
            report_now(ph,peers);}
        if(now-last_status>=(now-start<60000?5000:30000)){last_status=now;unsigned peers=0,half=0,unchoked=0;for(int k=0;k<MAX_CONN;k++){if(conns[k].state==PS_ACTIVE){peers++;if(!conns[k].peer_choking)unchoked++;}else if(conns[k].state)half++;}
            char ts[400],ds[120];tracker_summary(ts,sizeof ts);pthread_mutex_lock(&st_lock);snprintf(ds,sizeof ds,"%s",dht_status);unsigned s0=src_count[0],s1=src_count[1],s2=src_count[2],s3=src_count[3];pthread_mutex_unlock(&st_lock);
            LOG("Status: %u/%u pieces, %.1f KB/s, %u peers (%u unchoked us, %u connecting), %u known (tracker %u, DHT %u, PEX %u, saved %u), %u eligible; connects %u ok/%u tried (refused %u, timeout %u, unreachable %u, other %u); handshake fails %u, choked-out %u, incoming %u, hash fails %u",
                g_nhave,t->npieces,g_rate/1024,peers,unchoked,half,(unsigned)known,s0,s1,s2,s3,eligible,stats.ok,stats.attempts,stats.refused,stats.timeout,stats.unreach,stats.other,stats.hs_fail,stats.choked_out,stats.incoming,stats.hashfail);
            LOG("Transport: %s; %u completed handshakes, %llu payload blocks / %llu bytes, %d connector workers (process total)",socket_compat?"compatibility":"descriptor nonblocking",stats.handshakes,(unsigned long long)stats.blocks,(unsigned long long)stats.payload_bytes,__atomic_load_n(&connect_workers,__ATOMIC_ACQUIRE));
            LOG("Trackers: %s | %s",ts,ds);}
        int64_t quiet=now-(g_last_block>start?g_last_block:start);
        if(!known&&now-start>600000){char ts[200],ds[120];tracker_summary(ts,sizeof ts);pthread_mutex_lock(&st_lock);snprintf(ds,sizeof ds,"%s",dht_status);pthread_mutex_unlock(&st_lock);
            snprintf(last_error,sizeof last_error,"No peers found after 10 min. Trackers: %s. %s.%s",ts,ds,LOG_HINT);LOG("FAILED: %s",last_error);break;}
        if(known&&quiet>1800000){snprintf(last_error,sizeof last_error,"Found %u peers but none sent data for 30 min (%u refused, %u timed out, %u handshake failed, %u kept us choked).%s",(unsigned)known,stats.refused,stats.timeout,stats.hs_fail,stats.choked_out,LOG_HINT);LOG("FAILED: %s",last_error);break;}
    }
    rc=g_nhave==t->npieces?0:-1;
cleanup:
    shutdown_start=now_ms();
    __atomic_store_n(&g_quit,1,__ATOMIC_RELEASE);
#ifndef PORTAL_HOST
    for(int i=0;i<3;i++)if(rids[i]>=0)sceNetResolverAbort(rids[i],0);
#endif
    {int64_t now=now_ms();for(int k=0;k<MAX_CONN;k++)if(conns[k].state){conn_kill(&conns[k],K_QUIT,"shutdown");conn_close(t,&conns[k],now);}}
    if(lfd>=0)close(lfd);
    for(int i=0;i<3;i++)if(th_ok[i])pthread_join(th[i],NULL);
    if(resume_ready)checkpoint_save(t,1);
    if(socket_compat)LOG("Compatibility shutdown: %d connector workers still completing kernel calls (no torrent state retained)",__atomic_load_n(&connect_workers,__ATOMIC_ACQUIRE));
    if(g_stopped)LOG("Swarm stop completed after %lld ms",(long long)(now_ms()-shutdown_start));
    LOG("Swarm end: %u/%u pieces, %llu blocks received, %llu bytes uploaded, %u peers known, %u handshakes, %u hash fails%s",g_nhave,t->npieces,(unsigned long long)stats.blocks,(unsigned long long)g_up_bytes,(unsigned)ncands,stats.handshakes,stats.hashfail,g_stopped?" (stopped)":"");
    if(act)for(int a=nact-1;a>=0;a--)act_free(a);
    free(act);act=NULL;free(act_flag);act_flag=NULL;free(avail);avail=NULL;free(conns);conns=NULL;
    pthread_mutex_lock(&st_lock);free(cands);cands=NULL;ncands=0;free(trk);trk=NULL;ntrk=0;pthread_mutex_unlock(&st_lock);
    return rc;
}

static int download(torrent_t*t) {
    LOG("Torrent: %s, %llu bytes, %u pieces of %u bytes, %u files, %u trackers",t->name,(unsigned long long)t->total,t->npieces,t->plen,(unsigned)t->nfiles,(unsigned)t->ntrackers);
    if(recheck(t))return -1;
    g_nhave=count_have(t);g_have_bytes=have_bytes(t);
    LOG("Existing data: %u/%u pieces verified",g_nhave,t->npieces);
    uint64_t left=t->total-g_have_bytes;struct statvfs fs;
    if(left){
        errno=0;
        if(statvfs(output_dir,&fs)==0){
            uint64_t unit=fs.f_frsize?fs.f_frsize:fs.f_bsize;
            uint64_t available=(uint64_t)fs.f_bavail*unit;
            LOG("STORAGE path=%s needed=%llu available=%llu free_blocks=%llu blocks=%llu block_size=%llu",output_dir,(unsigned long long)left,(unsigned long long)available,(unsigned long long)fs.f_bavail,(unsigned long long)fs.f_blocks,(unsigned long long)unit);
            if(left>available){char message[256];snprintf(message,sizeof message,"Not enough free space at download location: need %llu MB, available %llu MB",(unsigned long long)(left/(1024*1024)),(unsigned long long)(available/(1024*1024)));fail(message);return -1;}
        }else{
            int storageError=errno;LOG("STORAGE statvfs failed path=%s errno=%d (%s)",output_dir,storageError,strerror(storageError));
        }
    }
    if(g_nhave<t->npieces){int r=swarm(t);if(stopped()||g_stopped)return 1;if(r||g_nhave!=t->npieces){if(!io_failed&&!last_error[0])fail("Torrent download did not complete." LOG_HINT);return -1;}}
    for(size_t i=0;i<t->nfiles;i++){if(ftruncate(t->files[i].fd,t->files[i].len)||fsync(t->files[i].fd)){fail("Could not flush completed files to disk");return -1;}}
    checkpoint_save(t,1);LOG("Download complete");report(t,"Complete",0);return 0;
}
int torrent_run(const char*metainfo,const char*destination,const torrent_callbacks*cb) {
    torrent_session s;memset(&s,0,sizeof s);
    s.ctx_log_id=__atomic_add_fetch(&next_log_id,1,__ATOMIC_RELAXED);
    torrent_session*previous=session_current();session_bind(&s);init_locks();log_t0=now_ms();for(int i=0;i<3;i++)rids[i]=-1;
    callbacks=cb;output_dir=destination;last_error[0]=0;io_failed=0;
    s.ctx_random_state=(unsigned)time(NULL)^(unsigned)(uintptr_t)cb^(unsigned)(uintptr_t)&s;if(!s.ctx_random_state)s.ctx_random_state=0x4e554c4c;
    if(!cb||!destination||strlen(destination)>700){pthread_mutex_destroy(&st_lock);pthread_mutex_destroy(&time_lock);session_bind(previous);return -1;}
    dns_cache=calloc(32,sizeof(dns_entry));if(!dns_cache){fail("Out of memory allocating DNS cache");goto early;}
    LOG("==== NULL_store 1.0 torrent_run pid=%d: %s -> %s",getpid(),metainfo?metainfo:"(null)",destination);
    FILE*f=fopen(metainfo,"rb");if(!f){fail("Cannot read torrent file");goto early;}
    if(fseek(f,0,SEEK_END)){fclose(f);goto early;}long n=ftell(f);rewind(f);
    if(n<=0||n>MAX_META){fclose(f);fail("Torrent metadata exceeds size limit");goto early;}
    uint8_t*d=malloc(n);if(!d){fclose(f);goto early;}size_t got=fread(d,1,n,f);fclose(f);
    if(got!=(size_t)n){free(d);goto early;}
#ifndef PORTAL_HOST
    int pool=sceNetPoolCreate("NULL_store Torrent",256*1024,0);if(pool<0){free(d);fail("Cannot allocate torrent network pool");goto early;}
    for(int i=0;i<3;i++){rids[i]=sceNetResolverCreate("NULL_store Torrent",pool,0);if(rids[i]<0){LOG("sceNetResolverCreate failed: 0x%08x",(unsigned)rids[i]);for(int j=0;j<i;j++)sceNetResolverDestroy(rids[j]);for(int j=0;j<3;j++)rids[j]=-1;sceNetPoolDestroy(pool);free(d);fail("Cannot start torrent DNS resolver");goto early;}}
#endif
    memcpy(peer_id,"-PT0101-",8);for(int i=8;i<20;i++)peer_id[i]='a'+session_random()%26;
    torrent_t t;int r=load_torrent(&t,d,n);if(r)LOG("Cannot load torrent: %s",last_error[0]?last_error:"invalid metadata");if(!r){resume_init(&t,metainfo);r=download(&t);}free_torrent(&t);free(d);
#ifndef PORTAL_HOST
    for(int i=0;i<3;i++){if(rids[i]>=0)sceNetResolverDestroy(rids[i]);rids[i]=-1;}sceNetPoolDestroy(pool);
#endif
    if(stopped()||r==1){LOG("Torrent stopped by request");log_close();r=1;goto finish;}
    if(r<0){LOG("Torrent failed: %s",*last_error?last_error:"Torrent failed");if(cb->error)cb->error(cb->user,*last_error?last_error:"Torrent failed");}
    log_close();goto finish;
early:LOG("Cannot load torrent: %s",*last_error?last_error:"unknown error");log_close();if(cb&&cb->error)cb->error(cb->user,*last_error?last_error:"Cannot load torrent");r=-1;
finish:free(dns_cache);pthread_mutex_destroy(&st_lock);pthread_mutex_destroy(&time_lock);session_bind(previous);return r;
}
