#ifndef _PAYLOAD_STDDEF_H_
#define _PAYLOAD_STDDEF_H_

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef _SIZE_T_DECLARED
#define _SIZE_T_DECLARED
typedef unsigned long size_t;
typedef long ssize_t;
#endif

typedef long ptrdiff_t;

#endif
