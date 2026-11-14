/* Nexus S2 read-only test protocol. GPL-2.0-or-later. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define NX_S2_VERSION 2
#define NX_S2_ENTRIES 16
#define NX_S2_READ_MAX 65536
typedef struct {
    uint32_t version, state, result, io_result;
    uint64_t generation, block_count;
    uint32_t block_size, filesystem, lun, probe_stage, probe_result, interfaces;
    char vendor[16], product[24];
} NxS2Status;
typedef struct {
    char path[512];
    uint64_t generation, offset;
    uint32_t count, reserved;
} NxS2Request;
typedef struct {
    char name[256];
    uint64_t size;
    uint32_t directory, reserved;
} NxS2Entry;
/* Require a canonical absolute path. No truncation or alternate device names. */
static inline int nxS2ValidPath(const char *p, size_t capacity) {
    const char *end=(const char*)memchr(p,0,capacity);
    if (!end || p[0]!='/' || end==p) return 0;
    if (end==p+1) return 1;
    const char *part=p+1;
    for (const char *s=part;;++s) {
        if (*s=='\\' || *s==':') return 0;
        if (*s=='/' || !*s) {
            size_t n=(size_t)(s-part);
            if (!n || n>255 || (n==1 && part[0]=='.') || (n==2 && part[0]=='.' && part[1]=='.')) return 0;
            if (!*s) return 1;
            part=s+1;
        }
    }
}
#if defined(__cplusplus)
static_assert(sizeof(NxS2Status)==96 && sizeof(NxS2Request)==536 && sizeof(NxS2Entry)==272);
#else
_Static_assert(sizeof(NxS2Status)==96 && sizeof(NxS2Request)==536 && sizeof(NxS2Entry)==272,"S2 ABI");
#endif
