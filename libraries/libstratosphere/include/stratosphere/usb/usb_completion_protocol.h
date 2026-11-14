/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEXUS_USB_COMPLETION_PROTOCOL_H
#define NEXUS_USB_COMPLETION_PROTOCOL_H
#include <stdint.h>

#define NEXUS_USB_COMPLETION_QUERY 82
#define NEXUS_USB_COMPLETION_MAGIC UINT32_C(0x3643504e)
#define NEXUS_USB_COMPLETION_VERSION 2
#define NEXUS_USB_COMPLETION_RING 24
#define NEXUS_USB_COMPLETION_SLOW_NS UINT64_C(100000000)
#define NEXUS_USB_COMPLETION_IDLE_NS UINT64_C(250000000)
#define NEXUS_USB_COMPLETION_UNKNOWN_RESULT UINT32_MAX
#define NEXUS_USB_COMPLETION_SOURCE_COMMAND 0
#define NEXUS_USB_COMPLETION_SOURCE_KEEP_ALIVE 1
#define NEXUS_USB_COMPLETION_SOURCE_SYNC 2
#define NEXUS_USB_COMPLETION_IDLE_VALID 1
#define NEXUS_USB_COMPLETION_LBA_VALID 2
#define NEXUS_USB_COMPLETION_CONTEXT_VALID 4
#define NEXUS_USB_COMPLETION_PREVIOUS_VALID 8
#define NEXUS_USB_COMPLETION_WRITE_HISTORY_VALID 16

/* One logical SCSI command, including any existing recovery/sense transfers.
 * idle_ns is sampled BEFORE the existing last-command clock is updated.
 * submit/wait/report are sums of the endpoint spans on this command's thread.
 * max_wait identifies the single longest completion-event wait. It does not
 * supply a hardware completion timestamp or distinguish scheduling latency.
 * All keepalives, commands >=100ms and first commands after >=250ms are kept.
 * Fast active commands still advance sequence and cumulative counts. */
typedef struct NexusUsbCompletionEvent {
    uint64_t sequence;
    uint64_t start_ns;
    uint64_t duration_ns;
    uint64_t idle_ns;
    uint64_t bytes;
    uint64_t submit_ns;
    uint64_t wait_ns;
    uint64_t report_ns;
    uint64_t max_wait_ns;
    uint64_t max_wait_start_ns;
    uint32_t host_calls;
    uint32_t wait_calls;
    uint32_t report_calls;
    uint32_t opcode;
    uint32_t source;
    uint32_t result;
    uint32_t max_wait_bytes;
    uint32_t max_wait_endpoint;
    uint32_t max_wait_xfer_id;
    uint32_t max_wait_result;
    uint32_t flags;
    uint32_t reserved;
    /* Version 2: requested starting LBA for READ/WRITE(10/16), plus context
     * from previously accepted logical command records. write_bytes_before
     * counts requested bytes of successful WRITE(10/16) commands, not physical
     * media writes or durable bytes. Drops limit completeness. The context
     * validity flags distinguish an unavailable sample from zero activity. */
    uint64_t lba;
    uint64_t write_bytes_before;
    uint64_t prior_write_end_ns;
    uint32_t previous_opcode;
    uint32_t previous_bytes;
} NexusUsbCompletionEvent;

/* RAM only. Snapshot does not start USB or issue any drive operation.
 * Boot cumulative; media_generation belongs to sampling time. Use timestamps
 * and snapshot deltas to separate runs/generations. retained_total reveals
 * ring overwrite; dropped counts recorder contention. host_unattributed counts
 * endpoint submissions outside the instrumented logical-command scopes. */
typedef struct NexusUsbCompletionSnapshot {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t event_count;
    uint64_t sampled_ns;
    uint64_t media_generation;
    uint64_t sequence;
    uint64_t dropped;
    uint64_t command_count;
    uint64_t keep_alive_count;
    uint64_t host_transfer_count;
    uint64_t host_unattributed;
    uint64_t last_command_end_ns;
    uint64_t slow_threshold_ns;
    uint64_t idle_threshold_ns;
    uint64_t retained_total;
    NexusUsbCompletionEvent events[NEXUS_USB_COMPLETION_RING];
} NexusUsbCompletionSnapshot;

#if defined(__cplusplus)
static_assert(sizeof(NexusUsbCompletionEvent) == 160, "USB completion event ABI");
static_assert(sizeof(NexusUsbCompletionSnapshot) == 3952, "USB completion snapshot ABI");
#else
_Static_assert(sizeof(NexusUsbCompletionEvent) == 160, "USB completion event ABI");
_Static_assert(sizeof(NexusUsbCompletionSnapshot) == 3952, "USB completion snapshot ABI");
#endif
#endif
