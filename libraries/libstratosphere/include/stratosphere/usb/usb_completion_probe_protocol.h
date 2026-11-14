/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEXUS_USB_COMPLETION_PROBE_PROTOCOL_H
#define NEXUS_USB_COMPLETION_PROBE_PROTOCOL_H
#include <stdint.h>
#define NEXUS_USB_COMPLETION_PROBE_CONTROL 90
#define NEXUS_USB_COMPLETION_PROBE_MAGIC UINT32_C(0x3150434e)
#define NEXUS_USB_COMPLETION_PROBE_VERSION 1
#define NEXUS_USB_COMPLETION_PROBE_QUERY 0
#define NEXUS_USB_COMPLETION_PROBE_ACQUIRE 1
#define NEXUS_USB_COMPLETION_PROBE_RESTORE 2
#define NEXUS_USB_COMPLETION_PROBE_EVENT_ONLY 0
#define NEXUS_USB_COMPLETION_PROBE_ASSISTED 1
#define NEXUS_USB_COMPLETION_PROBE_INTERVAL_MS 100
#define NEXUS_USB_COMPLETION_PROBE_MAX_MS 120000
#define NEXUS_USB_COMPLETION_PROBE_ACTIVE 1
#define NEXUS_USB_COMPLETION_PROBE_OWNED 2

/* Command90 receives kernel ClientProcessId. ACQUIRE: current nonzero
 * generation, token=0, mode EVENT_ONLY/ASSISTED, duration_ms=1..120000.
 * QUERY/RESTORE: mode=duration_ms=0; QUERY also token=0. reserved always0.
 * Active restore requires owner PID/token/generation; inactive restore is
 * idempotent. No renewal. ACQUIRE90 and ACQUIRE89 mutually exclude one another
 * so this experiment retains the ordinary256KiB disk-write transfer cap. */
typedef struct NexusUsbCompletionProbeRequest {
    uint64_t generation, token;
    uint32_t operation, mode, duration_ms, reserved;
} NexusUsbCompletionProbeRequest;

/* RAM-only boot-cumulative counters. Inactive owner/token/deadline/mode=0;
 * default_mode always EVENT_ONLY. OWNED is relative to the caller PID.
 * Policy applies to all modern USB endpoint transfers, not only caller I/O;
 * each PostBuffer snapshots its mode once. Expiry takes effect at the next
 * transfer boundary, never aborting in-flight I/O. Attach/detach, generation
 * changes and failed disk I/O cancel the lease. Acquire/restore serialize
 * with volume I/O; query never waits on that lock. Explicitly restore on all
 * exits; expiry is crash fallback. Control transfers/HOS1.x are unchanged.
 *
 * ASSISTED uses <=100ms waits within the ORIGINAL absolute deadline. An
 * intermediate timeout rechecks the event without blocking before querying
 * the SAME pending transfer. No re-submit, overlap, reset or write replay.
 * slice_timeouts: intermediate waits that timed out before final deadline.
 * event_races: nonblocking recheck found a signaled event after such timeout.
 * probe_queries: GetReport calls made without observing a signaled event.
 * probe_empty: successful such queries returning zero reports.
 * probe_ready: matching completion (including native failed result) found.
 * probe_errors: query failures or malformed identity/count/success lengths.
 * ready does NOT prove a lost wake: completion can race with the query.
 * Expected slice/recheck timeouts appear in existing per-call HostWait
 * diagnostics; compare TOTAL wait and command time, not max single wait.
 */
typedef struct NexusUsbCompletionProbeStatus {
    uint32_t magic, version, size, flags;
    uint64_t sampled_ns, media_generation, owner_pid, token, deadline_ns;
    uint32_t mode, default_mode;
    uint64_t acquisitions, expirations, restores, cancellations;
    uint64_t slice_timeouts, event_races, probe_queries, probe_empty, probe_ready, probe_errors;
} NexusUsbCompletionProbeStatus;
#if defined(__cplusplus)
static_assert(sizeof(NexusUsbCompletionProbeRequest)==32,"Completion-probe request ABI");
static_assert(sizeof(NexusUsbCompletionProbeStatus)==144,"Completion-probe status ABI");
#else
_Static_assert(sizeof(NexusUsbCompletionProbeRequest)==32,"Completion-probe request ABI");
_Static_assert(sizeof(NexusUsbCompletionProbeStatus)==144,"Completion-probe status ABI");
#endif
#endif
