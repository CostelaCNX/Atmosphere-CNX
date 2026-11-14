/* SPDX-License-Identifier: MIT */
#ifndef NEXUS_USB_WRITE_CACHE_PROTOCOL_H
#define NEXUS_USB_WRITE_CACHE_PROTOCOL_H
#include <stdint.h>
#define NEXUS_USB_WRITE_CACHE_QUERY 88u
#define NEXUS_USB_WRITE_CACHE_MAGIC UINT32_C(0x3143574e)
#define NEXUS_USB_WRITE_CACHE_VERSION 1u
#define NEXUS_USB_WRITE_CACHE_CAPACITY 64u
/* Boot-cumulative counters, sampled from RAM without starting USB. A clean
 * cache hit only skips a byte-identical single-sector write. Changed writes
 * remain synchronous; reads still reach USB. No dirty data is kept here.
 * logical counters count validated disk_write requests (including failures).
 * elided bytes are requested bytes avoided, not flash wear or durable bytes.
 * invalidations count explicit whole-cache clears, including attach/detach
 * and failed reads/writes. Compare deltas only within one boot and generation.
 * sector_size/cached_sectors describe the current attached volume; zero means
 * detached. The snapshot cannot establish power-loss durability. */
typedef struct NexusUsbWriteCacheSnapshot {
    uint32_t magic, version, size, capacity;
    uint64_t sampled_ns, media_generation;
    uint64_t logical_write_calls, logical_write_sectors;
    uint64_t elided_write_calls, elided_write_bytes, cache_invalidations;
    uint32_t sector_size, cached_sectors;
} NexusUsbWriteCacheSnapshot;
#if defined(__cplusplus)
static_assert(sizeof(NexusUsbWriteCacheSnapshot)==80,"USB write-cache snapshot ABI");
#else
_Static_assert(sizeof(NexusUsbWriteCacheSnapshot)==80,"USB write-cache snapshot ABI");
#endif
#endif
