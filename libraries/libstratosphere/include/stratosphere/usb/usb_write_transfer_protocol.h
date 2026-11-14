/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEXUS_USB_WRITE_TRANSFER_PROTOCOL_H
#define NEXUS_USB_WRITE_TRANSFER_PROTOCOL_H
#include <stdint.h>
#define NEXUS_USB_WRITE_TRANSFER_CONTROL 89
#define NEXUS_USB_WRITE_TRANSFER_MAGIC UINT32_C(0x3154574e)
#define NEXUS_USB_WRITE_TRANSFER_VERSION 1
#define NEXUS_USB_WRITE_TRANSFER_QUERY 0
#define NEXUS_USB_WRITE_TRANSFER_ACQUIRE 1
#define NEXUS_USB_WRITE_TRANSFER_RESTORE 2
#define NEXUS_USB_WRITE_TRANSFER_DEFAULT 262144
#define NEXUS_USB_WRITE_TRANSFER_SMALL 65536
#define NEXUS_USB_WRITE_TRANSFER_MAX_MS 120000
#define NEXUS_USB_WRITE_TRANSFER_ACTIVE 1
#define NEXUS_USB_WRITE_TRANSFER_OWNED 2

/* Command 89 also receives the kernel ClientProcessId. ACQUIRE requires a
 * current nonzero generation, token=0, one supported transfer_bytes value and
 * duration_ms in 1..120000. QUERY/RESTORE require transfer_bytes=duration_ms=0;
 * QUERY also requires token=0. reserved must always be zero. RESTORE requires
 * the owner and token while active; after expiry/invalidation it is idempotent.
 * No renewal: explicitly restore then acquire a new lease. */
typedef struct NexusUsbWriteTransferRequest {
    uint64_t generation, token;
    uint32_t operation, transfer_bytes, duration_ms, reserved;
} NexusUsbWriteTransferRequest;

/* RAM-only status; counters are boot cumulative. Inactive transfer_bytes is
 * DEFAULT, owner/token/deadline are zero. OWNED is relative to caller PID.
 * All USB disk writes share this temporary policy, not only the caller's.
 * Each disk_write snapshots its limit once; expiry takes effect at the next
 * request boundary, never aborting or changing an in-flight request. Reads,
 * sector elision, command deadlines, retry policy and f_sync are unchanged.
 * Acquire/restore wait for current volume I/O; query never takes that lock.
 * Detach, attach, media-generation change or failed disk I/O cancels a lease.
 * Explicit restore is required on every exit; expiry is crash fallback. */
typedef struct NexusUsbWriteTransferStatus {
    uint32_t magic, version, size, flags;
    uint64_t sampled_ns, media_generation, owner_pid, token, deadline_ns;
    uint32_t transfer_bytes, default_bytes;
    uint64_t acquisitions, expirations, restores, cancellations;
} NexusUsbWriteTransferStatus;
#if defined(__cplusplus)
static_assert(sizeof(NexusUsbWriteTransferRequest)==32,"Write-transfer request ABI");
static_assert(sizeof(NexusUsbWriteTransferStatus)==96,"Write-transfer status ABI");
#else
_Static_assert(sizeof(NexusUsbWriteTransferRequest)==32,"Write-transfer request ABI");
_Static_assert(sizeof(NexusUsbWriteTransferStatus)==96,"Write-transfer status ABI");
#endif
#endif
