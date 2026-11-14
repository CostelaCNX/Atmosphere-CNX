/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere/usb/usb_reader_device_reset.hpp>

namespace ams::usb::reader_direct_start {
    // One trial, only AFTER C15's existing exact-reader reset RPC succeeded.
    // A new Initialize has acquired/validated fresh handles. Never treat the
    // reset itself, or skipping GET_MAX_LUN, as a successful storage connection.
    enum { Attempts, Bus, Device, Generation, Stage, Ready, Capacity, Initialized, Count };
    inline std::atomic<uint32_t> sequence{}, fields[Count]{};
    inline bool claimed = false; // Single startup writer; never re-armed.
    template<class DeviceInfo, class Info>
    bool Claim(const DeviceInfo &d, const Info &acquired, uint32_t lun, bool reset_before,
               bool reselect, bool live, uint64_t known_blocks, uint32_t generation) {
        namespace r = reader_device_reset;
        if (claimed || lun != 0 || reset_before || reselect || !live || known_blocks ||
            r::ever_initialized.load() || !r::claimed.load() || r::fields[r::Calls].load() != 1 ||
            r::fields[r::Stage].load() != 3 || r::fields[r::Result].load() != 0 ||
            d.busID != r::fields[r::Bus].load() || !r::Matches(d) ||
            !endpoint_profile::Matches(d.device_desc, acquired) ||
            !interface_recovery::SameShape(d.inf, acquired)) return false;
        claimed = true;
        ++sequence;
        fields[Attempts]=1; fields[Bus]=d.busID; fields[Device]=d.deviceID;
        fields[Generation]=generation; fields[Stage]=1;
        fields[Ready]=fields[Capacity]=UINT32_MAX; fields[Initialized]=0;
        ++sequence;
        return true;
    }
    inline void Finish(bool success, uint32_t ready, uint32_t capacity) {
        ++sequence;
        fields[Ready]=ready; fields[Capacity]=capacity; fields[Initialized]=success;
        fields[Stage]=success ? 4 : ready != 0 ? 2 : 3;
        ++sequence;
    }
    inline int Format(char *out, size_t size) {
        uint32_t f[Count]{}; bool coherent=false;
        for(unsigned attempt=0;attempt<3;++attempt) {
            const auto before=sequence.load(); if(before&1)continue;
            for(unsigned i=0;i<Count;++i)f[i]=fields[i].load();
            if(before==sequence.load()){coherent=true;break;}
        }
        if(!coherent)return std::snprintf(out,size,"usb_reader_direct=C17\nusb_reader_direct_coherent=0\n");
        return std::snprintf(out,size,
            "usb_reader_direct=C17\nusb_reader_direct_coherent=1\n"
            "usb_reader_direct_scope=exact_reader_same_bus_after_reset_RPC_fresh_handles_once_per_boot_LUN0\n"
            "usb_reader_direct_attempts=%u\nusb_reader_direct_bus_device=%u,%u\n"
            "usb_reader_direct_generation=%u\nusb_reader_direct_stage=%u\n"
            "usb_reader_direct_ready=0x%08X\nusb_reader_direct_capacity=0x%08X\n"
            "usb_reader_direct_initialized=%u\n"
            "usb_reader_direct_stages=1:claimed_2:readiness_failed_3:capacity_failed_4:initialized_not_mount\n"
            "usb_reader_direct_note=GET_MAX_LUN_skipped_only_this_trial_no_assumption_other_slots_absent\n"
            "usb_reader_halt=C17_disabled_C16_status_query_timed_out\n",
            f[Attempts],f[Bus],f[Device],f[Generation],f[Stage],f[Ready],f[Capacity],f[Initialized]);
    }
}
