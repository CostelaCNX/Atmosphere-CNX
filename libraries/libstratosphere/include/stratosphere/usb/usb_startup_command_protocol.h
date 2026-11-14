/* SPDX-License-Identifier: MIT */
#ifndef NEXUS_STARTUP_COMMAND_PROTOCOL_H
#define NEXUS_STARTUP_COMMAND_PROTOCOL_H
#include "usb_startup_io_protocol.h"
#include "usb_completion_protocol.h"
#define NEXUS_STARTUP_COMMAND_QUERY 87
#define NEXUS_STARTUP_COMMAND_MAGIC UINT32_C(0x3143534e)
#define NEXUS_STARTUP_COMMAND_VERSION 1
#define NEXUS_STARTUP_COMMAND_SEGMENTS 7
#define NEXUS_STARTUP_COMMAND_UNKNOWN UINT32_MAX
#define NEXUS_STARTUP_COMMAND_ATTEMPT_VALID 1
#define NEXUS_STARTUP_COMMAND_LBA_VALID 2
enum NexusStartupCommandSegment {
    NexusStartupCommand_Unknown = 0,
    NexusStartupCommand_BotCommand = 1,
    NexusStartupCommand_BotData = 2,
    NexusStartupCommand_BotStatus = 3,
    NexusStartupCommand_UasCommand = 4,
    NexusStartupCommand_UasData = 5,
    NexusStartupCommand_UasStatus = 6
};
enum NexusStartupCommandState {
    NexusStartupCommand_None = 0,
    NexusStartupCommand_Captured = 1,
    NexusStartupCommand_Invalid = 2
};
/* A logical SCSI request, including existing retries/sense/recovery. Copied
 * before insertion into the older boot-global ring: command.sequence is zero
 * and is NOT an identifier. The longest wait's actual CDB opcode/LBA/tag are
 * separate so a REQUEST SENSE wait is never mislabeled as the original write.
 * Segment spans describe explicit transport buffers, are disjoint, and are
 * nested inside command.duration_ns. Host timings are nested, not additive.
 * filesystem_stage is the primary phase at logical-command admission, or
 * UNKNOWN. No media generation is inferred from USB probe counters. */
typedef struct NexusStartupUsbCommand {
    NexusUsbCompletionEvent command;
    uint64_t segment_calls[NEXUS_STARTUP_COMMAND_SEGMENTS];
    uint64_t segment_ns[NEXUS_STARTUP_COMMAND_SEGMENTS];
    uint64_t max_wait_lba;
    uint32_t filesystem_stage;
    uint32_t max_wait_segment;
    uint32_t max_wait_opcode;
    uint32_t max_wait_tag;
    uint32_t max_wait_flags;
    uint32_t max_wait_transport; /* 0 unknown, 1 BOT, 2 UAS */
    uint32_t max_wait_lun;
    uint32_t max_wait_sector_size;
} NexusStartupUsbCommand;
typedef struct NexusStartupCommandSample {
    uint64_t command_count;
    uint32_t state;
    uint32_t reserved;
    NexusStartupUsbCommand slowest;
} NexusStartupCommandSample;
typedef struct NexusStartupCommandEvent {
    uint64_t relative_start_ns;
    uint64_t duration_ns;
    uint32_t operation;
    uint32_t reserved;
    NexusStartupCommandSample sample;
} NexusStartupCommandEvent;
typedef struct NexusStartupCommandDetails {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t event_count;
    uint64_t sequence;
    uint64_t sampled_ns;
    uint32_t supported;
    uint32_t reserved;
    uint64_t command_count; /* all valid commands observed in the capture */
    uint64_t invalid_samples;
    /* Exactly aligned with the existing SD10 slowest_events, including events
     * that observed no command (for example a cached query waiting on a lock).
     * The sample retains the longest logical command from that same thread and
     * operation; ties choose the earlier start. This is not the lock holder's
     * command and does not depend on global-ring retention or snapshot deltas. */
    NexusStartupCommandEvent events[NEXUS_STARTUP_IO_EVENTS];
} NexusStartupCommandDetails;
/* Query87 takes u32 slot index; all layers sampled atomically under the same
 * lock. Commands84/85/86 retain their exact original sizes and behavior. */
typedef struct NexusStartupCommandSnapshot {
    NexusStartupIoSnapshot io;
    NexusStartupCommandDetails commands;
} NexusStartupCommandSnapshot;
#ifdef __cplusplus
static_assert(sizeof(NexusStartupUsbCommand)==312,"startup USB command ABI");
static_assert(sizeof(NexusStartupCommandSample)==328,"startup command sample ABI");
static_assert(sizeof(NexusStartupCommandEvent)==352,"startup command event ABI");
static_assert(sizeof(NexusStartupCommandDetails)==2872,"startup command details ABI");
static_assert(sizeof(NexusStartupCommandSnapshot)==10432,"startup command snapshot ABI");
#endif
#endif
