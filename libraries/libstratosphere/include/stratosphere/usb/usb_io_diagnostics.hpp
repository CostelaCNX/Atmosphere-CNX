/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once
#include <atomic>
#include <cstdint>
#include <stratosphere/os/os_tick.hpp>

namespace ams::usb::io_diagnostics {
    /* U2 startup only. Failure evidence survives later empty-interface polls.
     * Stages: 1=TUR, 2=START, 3=SENSE, 4=ready, 5=capacity, 6=initialized.
     * Sense uses 0x01kkAAqq (key/ASC/ASCQ); zero means absent/invalid. */
    inline std::atomic<uint64_t> startup_attempts{0}, startup_failures{0}, startup_last_ns{0};
    inline std::atomic<uint32_t> startup_stage{0}, startup_polls{0}, startup_starts{0};
    inline std::atomic<uint32_t> startup_result{0}, startup_sense{0};
    inline std::atomic<uint32_t> startup_failure_stage{0}, startup_failure_opcode{0};
    inline std::atomic<uint32_t> startup_failure_result{0}, startup_failure_sense{0};
    // Last attempted interface, retained on failed acquisition/cleanup.
    inline std::atomic<uint32_t> startup_vid{0}, startup_pid{0}, startup_protocol{0}, startup_lun{0};
    inline std::atomic<uint32_t> startup_endpoints{0}, startup_packets{0};
    inline std::atomic<uint32_t> startup_recoveries{0}, startup_recovery_stage{0}, startup_recovery_result{0};
    inline std::atomic<uint64_t> probe_failed_cycles{0};
    inline std::atomic<uint32_t> probe_cooldown_ms{0};
    // C1: last probed reader inventory and this cycle's confirmed empty slots.
    // These are diagnostics, never authority for slot selection or I/O.
    inline std::atomic<uint32_t> reader_lun_count{0}, reader_empty_lun_mask{0};
    inline std::atomic<uint32_t> reader_max_lun_result{0}, reader_insertion_retries{0};
    // Last pre-mount BOT failure: 1=CBW submit, 2=CBW length, 3=data,
    // 4=CSW submit, 5=length, 6=signature, 7=tag, 8=residue,
    // 9=command failed, 10=phase/status, 11=actual/residue mismatch.
    inline std::atomic<uint32_t> startup_xfer_stage{0}, startup_xfer_opcode{0}, startup_xfer_result{0};
    inline std::atomic<uint32_t> startup_xfer_expected{0}, startup_xfer_actual{0}, startup_xfer_tag{0};
    inline std::atomic<uint32_t> startup_csw_signature{0}, startup_csw_tag{0}, startup_csw_residue{0}, startup_csw_status{0};
    // C4: most recent pre-mount TUR status-recovery attempt, retained across
    // later LUN probes. UINT32_MAX result/status means that step was not run.
    // stage: 1=query, 2=not-halted/malformed, 3=clear, 4=retry, 5=validated,
    // 6=valid-command-failed, 7=query-failed, 8=clear-failed, 9=retry-failed,
    // 10=invalid-CSW, 11=deadline/session-lost. Counters are boot-cumulative.
    inline std::atomic<uint32_t> csw_recovery_attempts{0}, csw_recovery_validated{0}, csw_recovery_stage{0};
    inline std::atomic<uint32_t> csw_recovery_lun{0}, csw_recovery_tag{0}, csw_recovery_endpoint{0};
    inline std::atomic<uint32_t> csw_recovery_vid{0}, csw_recovery_pid{0}, csw_recovery_interface_id{0};
    inline std::atomic<uint64_t> csw_recovery_time_ns{0};
    inline std::atomic<uint32_t> csw_recovery_original{0}, csw_recovery_query_result{UINT32_MAX};
    inline std::atomic<uint32_t> csw_recovery_query_bytes{0}, csw_recovery_halt_status{UINT32_MAX};
    inline std::atomic<uint32_t> csw_recovery_clear_result{UINT32_MAX}, csw_recovery_retry_result{UINT32_MAX};
    inline std::atomic<uint32_t> csw_recovery_retry_bytes{0}, csw_recovery_final_result{UINT32_MAX};
    /* Boot-cumulative counters. Sampling never takes the USB volume lock. */
    // C5 latest failed-slot handoff. 1=scheduled, 2=refresh, 3=reset,
    // 4=inventory/probe, 5=initialized (not mounted), 6=refresh failure,
    // 7=reset failure, 8=initialization failure. No fields grant I/O authority.
    inline std::atomic<uint32_t> slot_recovery_attempts{0}, slot_recovery_stage{0};
    inline std::atomic<uint32_t> slot_recovery_from_lun{0}, slot_recovery_to_lun{0};
    inline std::atomic<uint32_t> slot_recovery_vid{0}, slot_recovery_pid{0}, slot_recovery_device_id{0};
    inline std::atomic<uint64_t> slot_recovery_time_ns{0};
    inline std::atomic<uint32_t> slot_recovery_result{UINT32_MAX}, slot_recovery_reset_result{UINT32_MAX};
    // End C5 diagnostics.
    inline std::atomic<uint64_t> file_opens{0};
    inline std::atomic<uint64_t> read_hits{0};
    inline std::atomic<uint64_t> write_hits{0};
    inline std::atomic<uint64_t> write_syncs{0};
    // SD9: positive metadata-cache lookups; invalidations count nonempty clears.
    inline std::atomic<uint64_t> metadata_cache_hits{0}, metadata_cache_misses{0};
    inline std::atomic<uint64_t> metadata_cache_invalidations{0}, metadata_stat_calls{0};
    inline std::atomic<uint64_t> virtual_sd_append_writes{0};
    inline std::atomic<uint64_t> read_bytes{0};
    inline std::atomic<uint64_t> written_bytes{0};
    inline std::atomic<uint64_t> read_calls{0}, write_calls{0};
    inline std::atomic<uint64_t> read_us{0}, write_us{0}, sync_us{0};
    inline std::atomic<uint64_t> disk_read_calls{0}, disk_write_calls{0};
    inline std::atomic<uint64_t> disk_read_bytes{0}, disk_write_bytes{0};
    inline std::atomic<uint64_t> disk_read_us{0}, disk_write_us{0};
    /* Nested elapsed times overlap: never sum file, sync and disk timings.
     * Only atomic counters here: no allocation, logs or filesystem recursion. */
    class ScopedTimer {
        std::atomic<uint64_t> &m_total;
        const os::Tick m_start;
        public:
            explicit ScopedTimer(std::atomic<uint64_t> &total)
                : m_total(total), m_start(os::GetSystemTick()) { }
            ~ScopedTimer() {
                const auto elapsed = (os::GetSystemTick() - m_start).ToTimeSpan().GetMicroSeconds();
                if (elapsed > 0) m_total.fetch_add(static_cast<uint64_t>(elapsed), std::memory_order_relaxed);
            }
            ScopedTimer(const ScopedTimer &) = delete;
            ScopedTimer &operator=(const ScopedTimer &) = delete;
    };
    /* Active interface descriptor values, not a throughput measurement. */
    inline std::atomic<uint32_t> protocol{0};
    inline std::atomic<uint32_t> bulk_in_packet{0};
    inline std::atomic<uint32_t> bulk_out_packet{0};
    /* Last writable-mount attempt, retained when ordinary probe status moves
     * on. Stage: 1=validation, 2=attach, 3=FatFs mount, 4=ready. UINT32_MAX
     * means f_mount was not reached. No change to the public service ABI. */
    inline std::atomic<uint32_t> mount_stage{0}, mount_fatfs_result{UINT32_MAX};
    inline std::atomic<uint32_t> mount_sector_size{0};
    inline std::atomic<uint64_t> mount_first_lba{0}, mount_sector_count{0};
    /* K1: physical commands, not cached reads/status queries. Retain failure
     * evidence across reconnects; do not log data, keys, or file contents. */
    inline std::atomic<uint64_t> last_command_ns{0}, transport_failures{0};
    inline std::atomic<uint32_t> last_failed_opcode{0}, last_failed_result{0};
    inline std::atomic<uint64_t> last_failure_ns{0};
    inline std::atomic<uint64_t> keep_alive_attempts{0}, keep_alive_successes{0}, keep_alive_failures{0};
    inline std::atomic<uint64_t> keep_alive_busy_skips{0}, keep_alive_last_ns{0};
    inline std::atomic<uint32_t> keep_alive_last_result{0};
    inline std::atomic<uint64_t> keep_alive_last_duration_ns{0}, keep_alive_max_duration_ns{0};
    inline std::atomic<uint64_t> keep_alive_slow_successes{0};
    inline std::atomic<uint64_t> link_query_failures{0}, link_missing_events{0}, reconnect_requests{0};
}
