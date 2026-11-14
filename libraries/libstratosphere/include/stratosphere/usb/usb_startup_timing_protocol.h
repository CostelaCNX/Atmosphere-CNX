/* SPDX-License-Identifier: MIT */
#ifndef NEXUS_STARTUP_TIMING_PROTOCOL_H
#define NEXUS_STARTUP_TIMING_PROTOCOL_H
#include <stdint.h>
#define NEXUS_STARTUP_SUBMIT 84u
#define NEXUS_STARTUP_QUERY 85u
#define NEXUS_STARTUP_MAGIC 0x3854504eu
#define NEXUS_STARTUP_VERSION 1u
#define NEXUS_STARTUP_SLOT_COUNT 8u
#define NEXUS_STARTUP_STAGE_COUNT 8u
#define NEXUS_STARTUP_OP_COUNT 8u
#define NEXUS_STARTUP_BUCKET_COUNT 12u
#define NEXUS_STARTUP_EVENT_COUNT 8u
#define NEXUS_STARTUP_WINDOW_NS UINT64_C(60000000000)
#define NEXUS_STARTUP_BUCKET_NS UINT64_C(5000000000)
#define NEXUS_STARTUP_SLOW_NS UINT64_C(10000000)
#define NEXUS_STARTUP_SOURCE_SD 1u
#define NEXUS_STARTUP_SOURCE_USB 2u
#define NEXUS_STARTUP_HANDOFF 1u
#define NEXUS_STARTUP_HASH_PERFORMED 2u
#define NEXUS_STARTUP_HASH_MATCHED 4u
enum NexusStartupStage {
    NexusStartup_Open, NexusStartup_Header, NexusStartup_Code,
    NexusStartup_Hash, NexusStartup_Activate, NexusStartup_Map,
    NexusStartup_Permissions, NexusStartup_Total
};
enum NexusStartupOperation {
    NexusStartup_Read, NexusStartup_Write, NexusStartup_OpenFile,
    NexusStartup_GetSize, NexusStartup_Metadata, NexusStartup_ReadDirectory,
    NexusStartup_OpenDirectory, NexusStartup_Flush
};
enum NexusStartupState {
    NexusStartup_Empty, NexusStartup_Capturing, NexusStartup_Expired,
    NexusStartup_LoaderFailed, NexusStartup_Superseded
};
/* Submit: PID placeholder u64 + send_pid; this exact record in an InBuffer;
 * returns u64 capture sequence. Kernel PID is used, never a caller-supplied PID.
 * HANDOFF starts the fixed 60-second filesystem observation window. Only an
 * explicitly selected diagnostic forwarder submits. No file payload is kept.
 * Hash time and bytes are zero when HASH_PERFORMED is clear (normal SD path).
 * Total includes the other stages: do not add it to them. Durations are ns. */
typedef struct NexusStartupLoader {
    uint32_t magic, version, source, flags, result, reserved;
    uint64_t nro_bytes;
    uint64_t duration_ns[NEXUS_STARTUP_STAGE_COUNT];
    uint8_t executable_sha256[32];
    char path[256];
} NexusStartupLoader;
typedef struct NexusStartupStats {
    uint64_t calls, requested_bytes, completed_bytes, total_ns, max_ns, errors;
} NexusStartupStats;
/* All recorded operations count in calls/io_ns; read_bytes is successful read
 * bytes only. Operations are assigned to the bucket in which they STARTED.
 * io_ns is summed call duration, not wall time (calls may overlap). */
typedef struct NexusStartupBucket {
    uint64_t read_bytes, io_ns, calls;
} NexusStartupBucket;
typedef struct NexusStartupEvent {
    uint64_t relative_start_ns, duration_ns, offset, requested_bytes, completed_bytes;
    uint32_t operation, result;
    char path[96]; /* NUL-terminated, possibly truncated; no file contents. */
} NexusStartupEvent;
/* Query: u32 physical slot index [0,8), OutBuffer of exactly this size.
 * Empty slots still return magic/version/sample time with sequence zero.
 * Eight captures retained until overwritten or reboot. Events are the latest
 * eight >=10ms calls, oldest first; event_total includes overwritten events.
 * Read size bands: <=4KiB, <=64KiB, <=1MiB, >1MiB (requested size).
 * Calls starting before deadline may finish after it and are included in full.
 * A query while calls are in flight is a partial snapshot. Expired means the
 * observation window elapsed, NOT that the game finished loading or exited.
 * Only filesystems opened after submission are covered. CPU/decode/render,
 * system saves and other filesystem services are outside this observation. */
typedef struct NexusStartupSnapshot {
    uint32_t magic, version;
    uint64_t sequence, pid, started_ns, deadline_ns, sampled_ns;
    uint32_t state, event_count;
    uint64_t event_total;
    NexusStartupLoader loader;
    uint64_t read_size_calls[4];
    NexusStartupStats operations[NEXUS_STARTUP_OP_COUNT];
    NexusStartupBucket buckets[NEXUS_STARTUP_BUCKET_COUNT];
    NexusStartupEvent events[NEXUS_STARTUP_EVENT_COUNT];
} NexusStartupSnapshot;
#if defined(__cplusplus)
static_assert(sizeof(NexusStartupLoader)==384, "startup loader ABI");
static_assert(sizeof(NexusStartupStats)==48, "startup stats ABI");
static_assert(sizeof(NexusStartupEvent)==144, "startup event ABI");
static_assert(sizeof(NexusStartupSnapshot)==2304, "startup snapshot ABI");
#else
_Static_assert(sizeof(NexusStartupLoader)==384, "startup loader ABI");
_Static_assert(sizeof(NexusStartupSnapshot)==2304, "startup snapshot ABI");
#endif
#endif
