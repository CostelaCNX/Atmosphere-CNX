/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere/usb/usb_reader_endpoint_profile.hpp>
#include <stratosphere/usb/usb_interface_recovery.hpp>
#include <stratosphere/usb/usb_probe_trace.hpp>

namespace ams::usb::reader_query_trial {
    // Experimental, not a general single-LUN quirk. Only C13's one fresh-session
    // retry after this exact reader's GET_MAX_LUN completion failed may enter.
    template<class Device> bool Eligible(const Device &d, bool reselect) {
        return reselect && interface_recovery::Eligible(d) &&
            endpoint_profile::Matches(d.device_desc, d.inf) &&
            !probe_trace::active.load() && probe_trace::control.phase.load() == 5 &&
            probe_trace::control.result.load() == 0x0003228C &&
            probe_trace::control.request.load() == 0xFE;
    }
    inline uint16_t Le16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
    inline bool DescriptorValid(const uint8_t *p, uint32_t n) {
        // Fixed public device descriptor only; never request serial/string data.
        return p && n == 18 && p[0] == 18 && p[1] == 1 && Le16(p+2) == 0x0320 &&
            p[7] == 9 && Le16(p+8) == 0x05E3 && Le16(p+10) == 0x0764 &&
            Le16(p+12) == 0x2961 && p[17] == 1;
    }
    enum Field { Attempts, Stage, Bus, DeviceId, PriorGeneration, DescriptorResult,
                 DescriptorBytes, DescriptorMatch, SkipMaxLun, ReadyResult, CapacityResult,
                 ControlPhase, ControlResult, ControlExpected, ControlActual,
                 BulkPhase, BulkResult, BulkExpected, BulkActual, Count };
    inline std::atomic<uint32_t> revision{}, fields[Count]{};
    template<class Device> void Begin(const Device &d) {
        revision.fetch_add(1);
        const auto attempts = fields[Attempts].load() + 1;
        for (auto &v : fields) v.store(0);
        fields[Attempts] = attempts; fields[Stage] = 1;
        fields[Bus] = d.busID; fields[DeviceId] = d.deviceID;
        fields[PriorGeneration] = probe_trace::generation.load();
        fields[DescriptorResult] = fields[ReadyResult] = fields[CapacityResult] = UINT32_MAX;
        revision.fetch_add(1);
    }
    inline void Set(Field f, uint32_t value) { revision.fetch_add(1); fields[f] = value; revision.fetch_add(1); }
    inline void Finish(bool success) {
        revision.fetch_add(1);
        // Keep the first-error evidence from THIS trial even when normal
        // background probing later replaces C13's latest-initializer record.
        fields[ControlPhase] = probe_trace::control.phase.load(); fields[ControlResult] = probe_trace::control.result.load();
        fields[ControlExpected] = probe_trace::control.expected.load(); fields[ControlActual] = probe_trace::control.actual.load();
        fields[BulkPhase] = probe_trace::bulk.phase.load(); fields[BulkResult] = probe_trace::bulk.result.load();
        fields[BulkExpected] = probe_trace::bulk.expected.load(); fields[BulkActual] = probe_trace::bulk.actual.load();
        if (success) fields[Stage] = 8;
        else if (fields[Stage].load() != 3 && fields[Stage].load() != 4) fields[Stage] = 7;
        revision.fetch_add(1);
    }
    inline int Format(char *out, size_t n) {
        uint32_t v[Count]{}; bool coherent = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto before = revision.load(); if (before & 1) continue;
            for (unsigned i = 0; i < Count; ++i) v[i] = fields[i].load();
            if (before == revision.load()) { coherent = true; break; }
        }
        if (!coherent) return std::snprintf(out,n,"usb_reader_trial=C14\nusb_reader_trial_coherent=0\n");
        return std::snprintf(out,n,
            "usb_reader_trial=C14\nusb_reader_trial_coherent=1\n"
            "usb_reader_trial_scope=last_05E3_0764_2961_0320_recovery_not_presence\n"
            "usb_reader_trial_stages=0:none_1:reacquire_2:descriptor_3:descriptor_error_4:descriptor_mismatch_5:slot0_7:failed_8:initialized_not_mount\n"
            "usb_reader_trial_attempts=%u\nusb_reader_trial_stage=%u\n"
            "usb_reader_trial_bus_device=%08X,%08X\nusb_reader_trial_prior_generation=%u\n"
            "usb_reader_trial_descriptor=0x%08X,%u,%u\n"
            "usb_reader_trial_descriptor_fields=result,reported_bytes,validated\n"
            "usb_reader_trial_max_lun_skipped=%u\nusb_reader_trial_ready=0x%08X\nusb_reader_trial_capacity=0x%08X\n"
            "usb_reader_trial_control=%u,0x%08X,%u,%u\nusb_reader_trial_bulk=%u,0x%08X,%u,%u\n"
            "usb_reader_trial_transfer_fields=phase,result,expected,reported_actual\n"
            "usb_reader_trial_note=one_GET_DESCRIPTOR_before_endpoints_no_native_completion_code\n",
            v[Attempts],v[Stage],v[Bus],v[DeviceId],v[PriorGeneration],
            v[DescriptorResult],v[DescriptorBytes],v[DescriptorMatch],v[SkipMaxLun],v[ReadyResult],v[CapacityResult],
            v[ControlPhase],v[ControlResult],v[ControlExpected],v[ControlActual],v[BulkPhase],v[BulkResult],v[BulkExpected],v[BulkActual]);
    }
}
