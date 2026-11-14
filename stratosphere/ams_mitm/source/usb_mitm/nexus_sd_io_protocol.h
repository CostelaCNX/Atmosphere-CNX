/* SPDX-License-Identifier: MIT */
#ifndef NEXUS_SD_IO_PROTOCOL_H
#define NEXUS_SD_IO_PROTOCOL_H
#include <stdint.h>
#define NEXUS_SD_IO_QUERY 80
#define NEXUS_SD_IO_MAGIC 0x31494453u
#define NEXUS_SD_IO_VERSION 1u
#define NEXUS_SD_IO_BUCKETS 5u
/* Buckets are disjoint. These are native SD filesystem calls in ams_mitm,
 * not physical sector transfers or whole-system traffic. No paths/PIDs/data. */
typedef struct NexusSdIoBucket {
    uint64_t read_calls, read_bytes, read_ns, read_errors;
    uint64_t write_calls, write_bytes, write_ns, write_errors;
    uint64_t metadata_calls, metadata_ns, metadata_errors;
    uint64_t flush_calls, flush_ns, flush_errors, in_flight;
} NexusSdIoBucket;
typedef struct NexusSdIoSnapshot {
    uint32_t magic, version, size, bucket_count;
    uint64_t coverage, epoch_ns, sampled_ns, sequence;
    uint64_t registry_drops, unknown_read_calls, unknown_write_calls;
    uint64_t usb_requests, usb_requests_active, media_generation;
    NexusSdIoBucket buckets[NEXUS_SD_IO_BUCKETS];
} NexusSdIoSnapshot;
/* coverage=31: native handles, request scopes, support scopes, logs, content.
 * 0=other observed SD; 1=ams:usb request; 2=explicit USB SD support;
 * 3=diagnostic log; 4=native SD content filesystem (not necessarily USB).
 * Successful bytes and errors are separate. Calls count attempts. A snapshot
 * is consistent under a short lock; in-flight requests have no final bytes yet.
 * Unknown calls are NOT assumed to be SD. Drops mean incomplete coverage. */
#ifdef __cplusplus
static_assert(sizeof(NexusSdIoBucket)==120);
static_assert(sizeof(NexusSdIoSnapshot)==696);
#endif
#endif
