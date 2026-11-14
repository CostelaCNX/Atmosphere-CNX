/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEXUS_USB_PAUSE_PROTOCOL_H
#define NEXUS_USB_PAUSE_PROTOCOL_H

#include <stdint.h>

#define NEXUS_USB_PAUSE_QUERY 81
#define NEXUS_USB_PAUSE_MAGIC UINT32_C(0x3453504e)
#define NEXUS_USB_PAUSE_VERSION 2
#define NEXUS_USB_PAUSE_STAGES 21
#define NEXUS_USB_PAUSE_RING 64
#define NEXUS_USB_PAUSE_THRESHOLD_NS UINT64_C(100000000)
#define NEXUS_USB_PAUSE_UNKNOWN_RESULT UINT32_MAX

typedef struct NexusUsbPauseStage {
    uint64_t calls;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t slow_calls;
} NexusUsbPauseStage;

typedef struct NexusUsbPauseEvent {
    uint64_t sequence;
    uint64_t start_ns;
    uint64_t duration_ns;
    uint64_t bytes;
    uint32_t stage;
    uint32_t opcode;
    uint32_t result;
    uint32_t reserved;
} NexusUsbPauseEvent;

/* Boot-cumulative counters. Events retain the newest slow records, ordered
 * oldest to newest. Sequence advances on every accepted record, including
 * fast operations. Dropped counts contention losses, not ring overwrites.
 * A busy snapshot returns an error instead of waiting. Version 2 appends
 * host submission, completion-event wait and report retrieval as stages
 * 18..20. Earlier stage IDs and the event layout are unchanged. */
typedef struct NexusUsbPauseSnapshot {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t stage_count;
    uint64_t sampled_ns;
    uint64_t media_generation;
    uint64_t sequence;
    uint64_t dropped;
    uint64_t threshold_ns;
    uint32_t event_count;
    uint32_t reserved;
    NexusUsbPauseStage stages[NEXUS_USB_PAUSE_STAGES];
    NexusUsbPauseEvent events[NEXUS_USB_PAUSE_RING];
} NexusUsbPauseSnapshot;

#if defined(__cplusplus)
static_assert(sizeof(NexusUsbPauseStage) == 32, "USB pause stage ABI");
static_assert(sizeof(NexusUsbPauseEvent) == 48, "USB pause event ABI");
static_assert(sizeof(NexusUsbPauseSnapshot) == 3808, "USB pause snapshot ABI");
#else
_Static_assert(sizeof(NexusUsbPauseStage) == 32, "USB pause stage ABI");
_Static_assert(sizeof(NexusUsbPauseEvent) == 48, "USB pause event ABI");
_Static_assert(sizeof(NexusUsbPauseSnapshot) == 3808, "USB pause snapshot ABI");
#endif

#endif
