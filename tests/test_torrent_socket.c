/* A controlled blocked connect validates detached-worker ownership under ASan.
   The caller cancels and releases its state before the syscall returns. */
#define _DARWIN_C_SOURCE
#define PORTAL_HOST
#define connect controlled_connect
#include "../src/torrent.c"
#undef connect
#include <assert.h>

static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed=PTHREAD_COND_INITIALIZER;
static int entered,released,connect_error;
int controlled_connect(int fd,const struct sockaddr*sa,socklen_t length){
    (void)fd;(void)sa;(void)length;
    pthread_mutex_lock(&gate_lock);entered=1;pthread_cond_broadcast(&gate_changed);
    while(!released)pthread_cond_wait(&gate_changed,&gate_lock);
    int e=connect_error;pthread_mutex_unlock(&gate_lock);errno=e;return e?-1:0;
}
static void wait_entered(void){pthread_mutex_lock(&gate_lock);while(!entered)pthread_cond_wait(&gate_changed,&gate_lock);pthread_mutex_unlock(&gate_lock);}
static void release_connect(void){pthread_mutex_lock(&gate_lock);released=1;pthread_cond_broadcast(&gate_changed);pthread_mutex_unlock(&gate_lock);}
static void reset_gate(int e){entered=released=0;connect_error=e;}
static void wait_workers(void){int64_t end=connect_clock()+2000;while(__atomic_load_n(&connect_workers,__ATOMIC_ACQUIRE)&&connect_clock()<end){struct timespec t={0,1000000};nanosleep(&t,NULL);}assert(!__atomic_load_n(&connect_workers,__ATOMIC_ACQUIRE));}
int main(void){
    for(int attempt=0;attempt<100;attempt++){
        reset_gate(ECONNRESET);int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
        connect_job*j=connect_job_start(fd,htonl(INADDR_LOOPBACK),1234);assert(j);wait_entered();
        int duplicate=socket(AF_INET,SOCK_STREAM,0);assert(!connect_job_start(duplicate,htonl(INADDR_LOOPBACK),1234)&&errno==EALREADY);
        int64_t began=connect_clock();connect_job_cancel(j);assert(connect_clock()-began<100);
        assert(__atomic_load_n(&connect_workers,__ATOMIC_ACQUIRE)==1);
        duplicate=socket(AF_INET,SOCK_STREAM,0);assert(!connect_job_start(duplicate,htonl(INADDR_LOOPBACK),1234)&&errno==EALREADY);
        release_connect();wait_workers();
    }
    reset_gate(ECONNRESET);connect_job*pending[MAX_CONNECT_WORKERS];
    for(int i=0;i<MAX_CONNECT_WORKERS;i++){int fd=socket(AF_INET,SOCK_STREAM,0);pending[i]=connect_job_start(fd,htonl(INADDR_LOOPBACK),2000+i);assert(pending[i]);}
    int overflow=socket(AF_INET,SOCK_STREAM,0);assert(!connect_job_start(overflow,htonl(INADDR_LOOPBACK),9999)&&errno==EAGAIN);
    assert(__atomic_load_n(&connect_workers,__ATOMIC_ACQUIRE)==MAX_CONNECT_WORKERS);
    for(int i=0;i<MAX_CONNECT_WORKERS;i++)connect_job_cancel(pending[i]);release_connect();wait_workers();
    for(int attempt=0;attempt<2;attempt++){
        reset_gate(attempt?ECONNREFUSED:0);int original=socket(AF_INET,SOCK_STREAM,0);assert(original>=0);
        connect_job*j=connect_job_start(original,htonl(INADDR_LOOPBACK),1234);assert(j);wait_entered();release_connect();wait_workers();
        int fd,e;int64_t elapsed;assert(connect_job_take(j,&fd,&e,&elapsed));assert(fd==original);assert(e==(attempt?ECONNREFUSED:0));assert(fcntl(fd,F_GETFD)>=0);close(fd);
    }
    puts("Connector cancellation, duplicate suppression, capacity bound and ownership passed (ASan/UBSan).");
}
