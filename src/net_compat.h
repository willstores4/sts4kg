#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#ifndef PORTAL_HOST
/* PS4 uses the FreeBSD message ABI. OpenOrbis's generic header has the
   Linux MSG_DONTWAIT value (0x40); FreeBSD 9.3 uses 0x80. */
#undef MSG_DONTWAIT
#define MSG_DONTWAIT 0x80
/* PlayStation's socket option (Sony WebKit setNonBlock); fcntl/ioctl can
   be denied for packaged applications. ABI value from Orbis net types. */
#define PORTAL_SO_NBIO 0x1200
#undef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x20000
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0x0800
#endif
#endif
static inline void portalAddress4(struct sockaddr_in*sa,uint32_t address,uint16_t port){memset(sa,0,sizeof *sa);
#if !defined(PORTAL_HOST) || defined(__APPLE__)
 sa->sin_len=sizeof *sa;
#endif
 sa->sin_family=AF_INET;sa->sin_addr.s_addr=address;sa->sin_port=htons(port);
}
