/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEXUS_USB_HOST_WRITE_PROTOCOL_H
#define NEXUS_USB_HOST_WRITE_PROTOCOL_H
#include <stdint.h>
#define NEXUS_USB_HOST_WRITE_CONTROL 91
#define NEXUS_USB_HOST_WRITE_MAGIC UINT32_C(0x3157484e)
#define NEXUS_USB_HOST_WRITE_VERSION 1
#define NEXUS_USB_HOST_WRITE_QUERY 0
#define NEXUS_USB_HOST_WRITE_ACQUIRE 1
#define NEXUS_USB_HOST_WRITE_RESTORE 2
#define NEXUS_USB_HOST_WRITE_DIRECT 0
#define NEXUS_USB_HOST_WRITE_SEGMENTED 1
#define NEXUS_USB_HOST_WRITE_SEGMENT_BYTES 65536
#define NEXUS_USB_HOST_WRITE_MAX_MS 120000
#define NEXUS_USB_HOST_WRITE_ACTIVE 1
#define NEXUS_USB_HOST_WRITE_OWNED 2

/* Command91 uses kernel ClientProcessId. QUERY/ACQUIRE/RESTORE and ownership,
 * generation, token, expiration semantics match command90. Acquisitions are
 * mutually exclusive with commands89/90. DIRECT is the unchanged default.
 * SEGMENTED sends one BOT WRITE(10/16) DATA-OUT stage in <=64KiB host requests;
 * the SCSI CDB/CBW, total length, ordering and single CSW are unchanged.
 * Policy is snapshotted once per data stage. An expiry never changes an
 * in-flight stage. Each stage uses one <=15s deadline shared by all chunks.
 * No concurrent transfers, zero-length packets, timeout retry or write replay.
 */
typedef struct NexusUsbHostWriteRequest {
    uint64_t generation, token;
    uint32_t operation, mode, duration_ms, reserved;
} NexusUsbHostWriteRequest;

/* RAM-only boot-cumulative counters, collected only for eligible BOT WRITE
 * data stages: HOS2+, aligned buffer, known power-of-two endpoint maxpacket
 * <=1024 dividing64KiB. Others retain the original direct path and are NOT
 * counted. Compare requested_bytes against the known workload to establish
 * coverage; active lease alone does not establish applicability.
 * eligible_commands counts both policies, including writes<=64KiB.
 * segmented_commands counts stages actually requiring >1 host submission.
 * data_submissions counts attempted DATA-OUT calls, excluding CBW/CSW.
 * requested_bytes is each stage's original length, counted once.
 * completed_bytes sums only successful, validated transfer lengths.
 * short_transfers counts successful short chunks (including zero); errors
 * counts failed data stages, including deadline/length failures. A short ends
 * data immediately; the existing CSW/residue checks decide command success.
 * No counters are physical media durability or controller completion times.
 */
typedef struct NexusUsbHostWriteStatus {
    uint32_t magic, version, size, flags;
    uint64_t sampled_ns, media_generation, owner_pid, token, deadline_ns;
    uint32_t mode, default_mode, segment_bytes, reserved;
    uint64_t acquisitions, expirations, restores, cancellations;
    uint64_t eligible_commands, segmented_commands, data_submissions;
    uint64_t requested_bytes, completed_bytes, short_transfers, errors;
} NexusUsbHostWriteStatus;
#if defined(__cplusplus)
static_assert(sizeof(NexusUsbHostWriteRequest)==32,"Host-write request ABI");
static_assert(sizeof(NexusUsbHostWriteStatus)==160,"Host-write status ABI");
#else
_Static_assert(sizeof(NexusUsbHostWriteRequest)==32,"Host-write request ABI");
_Static_assert(sizeof(NexusUsbHostWriteStatus)==160,"Host-write status ABI");
#endif
#endif
