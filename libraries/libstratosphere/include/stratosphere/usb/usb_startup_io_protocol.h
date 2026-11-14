/* SPDX-License-Identifier: MIT */
#ifndef NEXUS_STARTUP_IO_PROTOCOL_H
#define NEXUS_STARTUP_IO_PROTOCOL_H
#include "usb_startup_timing_protocol.h"
#define NEXUS_STARTUP_IO_QUERY 86
#define NEXUS_STARTUP_IO_MAGIC UINT32_C(0x314f494e)
#define NEXUS_STARTUP_IO_VERSION 1
#define NEXUS_STARTUP_IO_STAGES 8
#define NEXUS_STARTUP_IO_PRIMARY_STAGES 5
#define NEXUS_STARTUP_IO_EVENTS 8

enum NexusStartupIoStage {
    NexusStartupIo_LockWait = 0,
    NexusStartupIo_Metadata = 1,
    NexusStartupIo_Prepare = 2,
    NexusStartupIo_DataWrite = 3,
    NexusStartupIo_Sync = 4,
    NexusStartupIo_HostSubmit = 5,
    NexusStartupIo_HostWait = 6,
    NexusStartupIo_HostReport = 7
};

/* Primary stages 0..4 are disjoint elapsed time within the outer filesystem
 * operation. Metadata and preparation may themselves issue USB I/O. Sync
 * includes explicit f_sync and explicit flush/close work; implicit sync in
 * opening/evicting cached handles remains preparation. Host stages 5..7 are
 * nested USB endpoint timings and MUST NOT be added to primary stages.
 * calls==0 means no instrumented span was observed, not an instantaneous I/O. */
typedef struct NexusStartupIoSample {
    uint64_t calls[NEXUS_STARTUP_IO_STAGES];
    uint64_t duration_ns[NEXUS_STARTUP_IO_STAGES];
} NexusStartupIoSample;
typedef struct NexusStartupIoPhase {
    uint64_t calls;
    uint64_t total_ns;
    uint64_t max_ns; /* maximum accumulated phase time in one outer operation */
} NexusStartupIoPhase;
typedef struct NexusStartupIoOperation {
    uint64_t calls;
    uint64_t instrumented_calls;
    uint64_t total_ns;
    NexusStartupIoPhase phases[NEXUS_STARTUP_IO_STAGES];
} NexusStartupIoOperation;
typedef struct NexusStartupIoEvent {
    uint64_t relative_start_ns;
    uint64_t duration_ns;
    uint64_t offset;
    uint64_t requested_bytes;
    uint64_t completed_bytes;
    uint32_t operation;
    uint32_t result;
    NexusStartupIoSample sample;
    char path[256];
} NexusStartupIoEvent;
typedef struct NexusStartupIoDetails {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t stage_count;
    uint64_t sequence;
    uint64_t sampled_ns;
    uint64_t started_ns;
    uint64_t deadline_ns;
    uint64_t slow_event_total;
    uint64_t invalid_samples;
    uint32_t event_count;
    uint32_t supported; /* USB capture=1; native SD internals unavailable=0 */
    NexusStartupIoOperation operations[NEXUS_STARTUP_OP_COUNT];
    /* Slowest eight instrumented outer operations >= startup slow threshold;
     * sorted by descending duration, oldest start first for equal durations.
     * Totals cover every completed admitted call; events are a bounded subset. */
    NexusStartupIoEvent events[NEXUS_STARTUP_IO_EVENTS];
} NexusStartupIoDetails;

/* Command86 takes the same u32 slot index as85. Both parts are sampled under
 * one recorder lock. Existing commands84/85 and Manager/forwarder ABI stay
 * unchanged. This RAM-only query never starts USB or touches any filesystem. */
typedef struct NexusStartupIoSnapshot {
    NexusStartupSnapshot startup;
    NexusStartupIoDetails io;
} NexusStartupIoSnapshot;
#ifdef __cplusplus
static_assert(sizeof(NexusStartupIoSample)==128,"startup I/O sample ABI");
static_assert(sizeof(NexusStartupIoPhase)==24,"startup I/O phase ABI");
static_assert(sizeof(NexusStartupIoOperation)==216,"startup I/O operation ABI");
static_assert(sizeof(NexusStartupIoEvent)==432,"startup I/O event ABI");
static_assert(sizeof(NexusStartupIoDetails)==5256,"startup I/O detail ABI");
static_assert(sizeof(NexusStartupIoSnapshot)==sizeof(NexusStartupSnapshot)+5256,"startup I/O combined ABI");
#endif
#endif
