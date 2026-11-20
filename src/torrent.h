#ifndef PORTAL_TORRENT_H
#define PORTAL_TORRENT_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Concurrent calls keep independent torrent/network state. HTTP results must be malloc-owned. */
typedef struct {
    void *user;
    unsigned char *(*http)(void *, const char *, size_t *);
    int (*stopped)(void *);
    void (*progress)(void *, const char *, uint64_t, uint64_t, unsigned);
    void (*error)(void *, const char *);
    /* Optional cumulative payload bytes for live throughput before piece verification. */
    void (*traffic)(void *, uint64_t);
} torrent_callbacks;
/* 0 completed, 1 stopped with files retained, -1 failed. Rechecks existing pieces. */
int torrent_identity(const unsigned char*,size_t,char out[41]);
int torrent_run(const char *metainfo, const char *destination, const torrent_callbacks *callbacks);
#ifdef __cplusplus
}
#endif
#endif
