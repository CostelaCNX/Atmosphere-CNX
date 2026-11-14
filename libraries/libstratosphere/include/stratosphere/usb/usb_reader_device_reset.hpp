/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere/usb/usb_reader_endpoint_profile.hpp>
#include <stratosphere/usb/usb_interface_recovery.hpp>
#include <stratosphere/usb/usb_probe_trace.hpp>

namespace ams::usb::reader_device_reset {
    // Single startup writer; never reset a device after ANY successful storage
    // initialization in this process. Device-ID changes do not refill the budget.
    inline std::atomic<bool> claimed{}, ever_initialized{};
    enum Field { Attempts, Stage, Calls, Bus, DeviceId, PriorGeneration, Result,
        ElapsedMs, PostAttempts, PostInitialized, PostBus, PostDeviceId, Ready,
        Capacity, ControlPhase, ControlResult, ControlExpected, ControlActual,
        BulkPhase, BulkResult, BulkExpected, BulkActual, Count };
    inline std::atomic<uint32_t> revision{}, fields[Count]{};

    template<class Device> bool Matches(const Device &d) {
        return interface_recovery::Eligible(d) && endpoint_profile::Matches(d.device_desc, d.inf);
    }
    template<class Device> bool Eligible(const Device &d, bool reselect) {
        // Require BOTH observed startup channel failures, not just a timeout,
        // an empty slot, a malformed response or an unrelated device failure.
        return reselect && Matches(d) && !ever_initialized.load() && !claimed.load() &&
            !probe_trace::active.load() && probe_trace::control.phase.load() == 5 &&
            probe_trace::control.result.load() == 0x3228C && probe_trace::control.request.load() == 0xFE &&
            probe_trace::control.expected.load() == 1 && probe_trace::bulk.phase.load() == 5 &&
            probe_trace::bulk.result.load() == 0x3228C && probe_trace::bulk.request.load() == 2 &&
            probe_trace::bulk.expected.load() == 31 && probe_trace::bulk.actual.load() == 0;
    }
    template<class Device> bool Begin(const Device &d, bool reselect) {
        if (!Eligible(d, reselect)) return false;
        bool expected = false;
        if (!claimed.compare_exchange_strong(expected, true)) return false;
        revision.fetch_add(1);
        for (auto &v : fields) v = 0;
        fields[Attempts] = 1; fields[Stage] = 1;
        fields[Bus] = d.busID; fields[DeviceId] = d.deviceID;
        fields[PriorGeneration] = probe_trace::generation.load();
        fields[Result] = fields[Ready] = fields[Capacity] = UINT32_MAX;
        revision.fetch_add(1);
        return true;
    }
    inline void Calling() {
        revision.fetch_add(1); fields[Stage] = 2; fields[Calls] = 1; revision.fetch_add(1);
    }
    inline void Complete(uint32_t result, uint64_t elapsed_ns) {
        revision.fetch_add(1);
        fields[Result] = result;
        const auto ms = elapsed_ns / 1000000;
        fields[ElapsedMs] = ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
        fields[Stage] = result ? 4 : 3;
        revision.fetch_add(1);
    }
    template<class Device> void Finish(const Device &d, bool success, bool reset_attempt,
                                        uint32_t ready_result, uint32_t capacity_result) {
        if (success) ever_initialized = true;
        if (reset_attempt) {
            if (fields[Stage].load() == 1) {
                revision.fetch_add(1); fields[Stage] = 5; revision.fetch_add(1);
            }
            return;
        }
        if (!fields[Calls].load() || !Matches(d)) return;
        revision.fetch_add(1);
        ++fields[PostAttempts]; fields[PostInitialized] = success;
        fields[PostBus] = d.busID; fields[PostDeviceId] = d.deviceID;
        fields[Ready] = ready_result; fields[Capacity] = capacity_result;
        fields[ControlPhase] = probe_trace::control.phase.load(); fields[ControlResult] = probe_trace::control.result.load();
        fields[ControlExpected] = probe_trace::control.expected.load(); fields[ControlActual] = probe_trace::control.actual.load();
        fields[BulkPhase] = probe_trace::bulk.phase.load(); fields[BulkResult] = probe_trace::bulk.result.load();
        fields[BulkExpected] = probe_trace::bulk.expected.load(); fields[BulkActual] = probe_trace::bulk.actual.load();
        revision.fetch_add(1);
    }
    inline int Format(char *out, size_t n) {
        uint32_t v[Count]{}; bool coherent = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto before = revision.load(); if (before & 1) continue;
            for (unsigned i = 0; i < Count; ++i) v[i] = fields[i].load();
            if (before == revision.load()) { coherent = true; break; }
        }
        if (!coherent) return std::snprintf(out,n,"usb_reader_reset=C15\nusb_reader_reset_coherent=0\n");
        return std::snprintf(out,n,
            "usb_reader_reset=C15\nusb_reader_reset_coherent=1\n"
            "usb_reader_reset_policy=05E3_0764_2961_0320_once_per_boot_before_any_storage_initialized\n"
            "usb_reader_reset_stages=0:none_1:acquire_2:reset_IPC_3:RPC_ok_reprobe_required_4:RPC_error_5:preflight_refused\n"
            "usb_reader_reset_attempts=%u\nusb_reader_reset_stage=%u\nusb_reader_reset_calls=%u\n"
            "usb_reader_reset_bus_device=%08X,%08X\nusb_reader_reset_prior_generation=%u\n"
            "usb_reader_reset_result=0x%08X\nusb_reader_reset_elapsed_ms=%u\n"
            "usb_reader_reset_post_scope=last_exact_model_probe_not_physical_identity_or_mount_proof\n"
            "usb_reader_reset_post=%u,%u,%08X,%08X\n"
            "usb_reader_reset_post_fields=attempts,initialized,bus,device\n"
            "usb_reader_reset_ready=0x%08X\nusb_reader_reset_capacity=0x%08X\n"
            "usb_reader_reset_control=%u,0x%08X,%u,%u\nusb_reader_reset_bulk=%u,0x%08X,%u,%u\n"
            "usb_reader_reset_transfer_fields=phase,result,expected,reported_actual\n"
            "usb_reader_reset_note=close_all_old_handles_fresh_discovery_no_speed_change_no_native_completion_code\n",
            v[Attempts],v[Stage],v[Calls],v[Bus],v[DeviceId],v[PriorGeneration],v[Result],v[ElapsedMs],
            v[PostAttempts],v[PostInitialized],v[PostBus],v[PostDeviceId],v[Ready],v[Capacity],
            v[ControlPhase],v[ControlResult],v[ControlExpected],v[ControlActual],v[BulkPhase],v[BulkResult],v[BulkExpected],v[BulkActual]);
    }
}
