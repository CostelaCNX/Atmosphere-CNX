/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEXUS_USB_KEEP_ALIVE_TEST_PROTOCOL_H
#define NEXUS_USB_KEEP_ALIVE_TEST_PROTOCOL_H
#include <stdint.h>

#define NEXUS_USB_KEEP_ALIVE_CONTROL 83
#define NEXUS_USB_KEEP_ALIVE_MAGIC UINT32_C(0x374b504e)
#define NEXUS_USB_KEEP_ALIVE_VERSION 1
#define NEXUS_USB_KEEP_ALIVE_QUERY 0
#define NEXUS_USB_KEEP_ALIVE_SUSPEND 1
#define NEXUS_USB_KEEP_ALIVE_RESTORE 2
#define NEXUS_USB_KEEP_ALIVE_MAX_MS 30000
#define NEXUS_USB_KEEP_ALIVE_ACTIVE 1
#define NEXUS_USB_KEEP_ALIVE_OWNED 2

/* Command 83 also takes the kernel-supplied ClientProcessId. Suspend requires
 * a current generation, token=0 and duration_ms in 1..30000. Query and Restore
 * require duration_ms=0. Restore requires the owner's token while active and
 * is idempotent after expiry/invalidation. Query never arms a lease.
 * Use explicit restore on every exit; expiry is the crash-safety fallback. */
typedef struct NexusUsbKeepAliveRequest {
    uint64_t generation;
    uint64_t token;
    uint32_t operation;
    uint32_t duration_ms;
} NexusUsbKeepAliveRequest;

/* State is RAM-only. Successful suspension is serialized with the volume's
 * command stream, so any already-running keep-alive finishes before receipt.
 * Suppression is checked using monotonic time; no persisted power policy is
 * changed. Status counts are boot cumulative. OWNED is relative to caller PID.
 * Query can observe an expired lease being cleared. It does not issue USB I/O. */
typedef struct NexusUsbKeepAliveStatus {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint64_t sampled_ns;
    uint64_t media_generation;
    uint64_t owner_pid;
    uint64_t token;
    uint64_t deadline_ns;
    uint64_t skipped_polls;
    uint64_t acquisitions;
    uint64_t expirations;
    uint64_t restores;
    uint64_t generation_cancels;
} NexusUsbKeepAliveStatus;

#if defined(__cplusplus)
static_assert(sizeof(NexusUsbKeepAliveRequest) == 24, "Keep-alive request ABI");
static_assert(sizeof(NexusUsbKeepAliveStatus) == 96, "Keep-alive status ABI");
#else
_Static_assert(sizeof(NexusUsbKeepAliveRequest) == 24, "Keep-alive request ABI");
_Static_assert(sizeof(NexusUsbKeepAliveStatus) == 96, "Keep-alive status ABI");
#endif
#endif
