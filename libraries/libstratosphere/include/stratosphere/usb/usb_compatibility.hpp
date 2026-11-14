/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere/usb/usb_device_map.hpp>
#include <stratosphere/usb/usb_reader_endpoint_profile.hpp>
#include <stratosphere/usb/usb_reader_startup_profile.hpp>

namespace ams::usb::compatibility {
    constexpr size_t ReportBytes = 24 * 1024;
    constexpr uint32_t DefaultUrbs = 4, DefaultBytes = 0x400000;
    enum Profile : uint32_t { StandardBot, StandardRbc, SerializedUas, ReaderStartupC8,
                              Reader05E30764, Unsupported,
                              ReaderAllocationC11 = Reader05E30764 };
    // Stable numeric profile 4 retains the C11 endpoint allocation and the
    // C19 startup request, which the user confirmed mounts their reader.
    // Matching still requires the tested revision/descriptor shape in BOTH
    // inventories. VID/PID alone is not evidence for a different revision.
    constexpr uint32_t DefaultStartupSenseBytes = 64, Reader05E30764SenseBytes = 18;
    inline const char *Name(uint32_t p) {
        switch (p) {
            case StandardBot: return "standard-bot";
            case StandardRbc: return "standard-rbc-bot";
            case SerializedUas: return "uas-serialized-existing-driver";
            case ReaderStartupC8: return "reader-2e4d-2372-startup";
            case Reader05E30764: return "reader-05e3-0764-fixed-sense18";
            default: return "unsupported-interface";
        }
    }
    struct Decision {
        Profile profile{Unsupported};
        uint32_t urbs{}, in_bytes{}, out_bytes{}, in_packet{}, out_packet{};
        uint32_t bulk_in{}, bulk_out{}, ss_companions{}, descriptor_warnings{};
    };
    constexpr uint32_t StartupSenseBytes(const Decision &r) {
        // A BOT/SCSI readiness policy, NOT a reader identity guess. Linux
        // usb-storage also starts with small autosense (US_SENSE_SIZE).
        // Allocation, speed, reset quirks and mounted I/O are independent.
        const bool bot = r.profile == StandardBot || r.profile == StandardRbc ||
                         r.profile == ReaderStartupC8 || r.profile == Reader05E30764;
        return bot && r.descriptor_warnings == 0
            ? Reader05E30764SenseBytes : DefaultStartupSenseBytes;
    }
    template<class Interface>
    bool SameStartupInventory(const Interface &a, const Interface &b) {
        const auto &x = a.interface_desc; const auto &y = b.interface_desc;
        if (x.bInterfaceClass != y.bInterfaceClass || x.bInterfaceSubClass != y.bInterfaceSubClass ||
            x.bInterfaceProtocol != y.bInterfaceProtocol || x.bInterfaceNumber != y.bInterfaceNumber ||
            x.bAlternateSetting != y.bAlternateSetting || x.bNumEndpoints != y.bNumEndpoints) return false;
        // Match endpoint addresses, not array positions. Describe() separately
        // validates each inventory, including counts and duplicate addresses.
        for (unsigned direction = 0; direction < 2; ++direction) {
            const auto &ae = direction ? a.output_endpoint_descs : a.input_endpoint_descs;
            const auto &be = direction ? b.output_endpoint_descs : b.input_endpoint_descs;
            const auto &ac = direction ? a.output_ss_endpoint_companion_descs : a.input_ss_endpoint_companion_descs;
            const auto &bc = direction ? b.output_ss_endpoint_companion_descs : b.input_ss_endpoint_companion_descs;
            for (size_t n = 0; n < sizeof(ae) / sizeof(ae[0]); ++n) {
                if (!ae[n].bDescriptorType) continue;
                bool matched = false;
                for (size_t k = 0; k < sizeof(be) / sizeof(be[0]); ++k) {
                    if (!be[k].bDescriptorType || ae[n].bEndpointAddress != be[k].bEndpointAddress) continue;
                    matched = ae[n].bDescriptorType == be[k].bDescriptorType &&
                        ae[n].bmAttributes == be[k].bmAttributes && ae[n].wMaxPacketSize == be[k].wMaxPacketSize &&
                        ac[n].bDescriptorType == bc[k].bDescriptorType;
                    if (matched && ac[n].bDescriptorType)
                        matched = ac[n].bLength == bc[k].bLength && ac[n].bMaxBurst == bc[k].bMaxBurst &&
                            ac[n].bmAttributes == bc[k].bmAttributes && ac[n].wBytesPerInterval == bc[k].wBytesPerInterval;
                    break;
                }
                if (!matched) return false;
            }
        }
        return true;
    }
    // Inspection only. Never use USB version, RMB or a brand name to decide
    // that an unknown HDD/SSD is a reader. Unknown models retain the fast path.
    template<class Device, class Interface>
    Decision Describe(const Device &d, const Interface &i) {
        Decision r{};
        const auto &v = i.interface_desc;
        if (v.bInterfaceClass != 8) return r;
        if (v.bInterfaceProtocol == 0x50 && (v.bInterfaceSubClass == 6 || v.bInterfaceSubClass == 1))
            r.profile = v.bInterfaceSubClass == 1 ? StandardRbc : StandardBot;
        else if (v.bInterfaceProtocol == 0x62 && v.bInterfaceSubClass == 6) r.profile = SerializedUas;
        else return r;
        r.urbs = r.profile == SerializedUas ? 1 : DefaultUrbs;
        r.in_bytes = r.out_bytes = DefaultBytes;
        for (unsigned direction = 0; direction < 2; ++direction) {
            const auto &endpoints = direction ? i.output_endpoint_descs : i.input_endpoint_descs;
            const auto &companions = direction ? i.output_ss_endpoint_companion_descs : i.input_ss_endpoint_companion_descs;
            uint32_t count = 0, packet = 0;
            uint32_t addresses = 0;
            for (size_t n = 0; n < sizeof(endpoints) / sizeof(endpoints[0]); ++n) {
                const auto &e = endpoints[n];
                if (!e.bDescriptorType) continue;
                if (e.bDescriptorType != 5 || e.bmAttributes != 2 ||
                    (e.bEndpointAddress & 0x70) || !(e.bEndpointAddress & 15) ||
                    bool(e.bEndpointAddress & 0x80) == bool(direction)) { r.descriptor_warnings |= 1; continue; }
                if (addresses & (1u << (e.bEndpointAddress & 15))) r.descriptor_warnings |= 2;
                addresses |= 1u << (e.bEndpointAddress & 15);
                ++count;
                if (!packet) packet = e.wMaxPacketSize;
                const auto p = e.wMaxPacketSize;
                if (p != 8 && p != 16 && p != 32 && p != 64 && p != 512 && p != 1024) r.descriptor_warnings |= 4;
                const auto &c = companions[n];
                if (c.bDescriptorType) {
                    if (c.bDescriptorType == 0x30 && c.bLength == 6 && c.bMaxBurst <= 15 && p == 1024 &&
                        !(c.bmAttributes & 0xE0) && (c.bmAttributes & 31) <= 16 && !c.wBytesPerInterval) ++r.ss_companions;
                    else r.descriptor_warnings |= 8;
                }
            }
            if (direction) { r.bulk_out = count; r.out_packet = packet; }
            else { r.bulk_in = count; r.in_packet = packet; }
        }
        const unsigned expected = r.profile == SerializedUas ? 2 : 1;
        if (r.bulk_in != expected || r.bulk_out != expected || v.bNumEndpoints != expected * 2) r.descriptor_warnings |= 16;
        if (endpoint_profile::Matches(d, i)) {
            r.profile = Reader05E30764; r.urbs = 1; r.in_bytes = r.out_bytes = 1024;
        } else if (startup_profile::Matches(d, i)) r.profile = ReaderStartupC8;
        return r;
    }
    template<class Device, class Interface>
    Decision Select(const Device &d, const Interface &candidate, const Interface &acquired) {
        auto r = Describe(d, acquired);
        // Allocation quirk requires both inventories to agree. Discovery-only
        // descriptors never authorize a lower-level command or a cached mount.
        if (r.profile == Reader05E30764 && !endpoint_profile::Matches(d, candidate)) {
            r.profile = StandardBot; r.urbs = DefaultUrbs; r.in_bytes = r.out_bytes = DefaultBytes;
            r.descriptor_warnings |= 32;
        }
        if (r.profile == ReaderStartupC8 && !startup_profile::Matches(d, candidate)) {
            r.profile = StandardBot; r.descriptor_warnings |= 32;
        }
        if (StartupSenseBytes(r) == Reader05E30764SenseBytes &&
            (Describe(d, candidate).descriptor_warnings || !SameStartupInventory(candidate, acquired)))
            r.descriptor_warnings |= 32;
        return r;
    }
    // One initializer writer, bounded non-waiting monitor reader. No strings,
    // serial numbers, commands, SD I/O or dynamic allocation on the USB path.
    enum Field { Identity, Version, Lun, Stage, Selected, Urbs, InBytes, OutBytes,
                 InPacket, OutPacket, Warnings, BlockCount, BlockSize, StartedNs, FieldCount };
    struct State { uint32_t revision{}; uint64_t value[FieldCount]{}; };
    inline std::atomic<uint32_t> revision{};
    inline std::atomic<uint64_t> values[FieldCount]{};
    template<class Device> void Begin(const Device &d, uint32_t lun, uint64_t now) {
        revision.fetch_add(1);
        for (auto &v : values) v.store(0);
        values[Identity].store((uint32_t(d.idVendor) << 16) | d.idProduct);
        values[Version].store((uint32_t(d.bcdUSB) << 16) | d.bcdDevice);
        values[Lun].store(lun); values[Selected].store(Unsupported);
        values[Stage].store(1); values[StartedNs].store(now);
        revision.fetch_add(1);
    }
    inline void Applied(const Decision &r) {
        revision.fetch_add(1);
        values[Selected].store(r.profile); values[Urbs].store(r.urbs);
        values[InBytes].store(r.in_bytes); values[OutBytes].store(r.out_bytes);
        values[InPacket].store(r.in_packet); values[OutPacket].store(r.out_packet);
        values[Warnings].store(r.descriptor_warnings); values[Stage].store(2);
        revision.fetch_add(1);
    }
    inline void Capacity(uint64_t blocks, uint32_t size) {
        revision.fetch_add(1); values[BlockCount].store(blocks); values[BlockSize].store(size);
        values[Stage].store(3); revision.fetch_add(1);
    }
    inline bool Snapshot(State &s) {
        s.revision = revision.load(); if (s.revision & 1) return false;
        for (unsigned i = 0; i < FieldCount; ++i) s.value[i] = values[i].load();
        return s.revision == revision.load();
    }
    // Decode the already-captured descriptor map, not a second USB probe.
    struct RawDevice { uint32_t idVendor{}, idProduct{}, bcdUSB{}, bcdDevice{}; };
    struct RawEndpoint { uint32_t bDescriptorType{}, bEndpointAddress{}, bmAttributes{}, wMaxPacketSize{}; };
    struct RawCompanion { uint32_t bLength{}, bDescriptorType{}, bMaxBurst{}, bmAttributes{}, wBytesPerInterval{}; };
    struct RawInterface {
        struct { uint32_t bInterfaceClass{}, bInterfaceSubClass{}, bInterfaceProtocol{}, bInterfaceNumber{}, bAlternateSetting{}, bNumEndpoints{}; } interface_desc;
        RawEndpoint input_endpoint_descs[15]{}, output_endpoint_descs[15]{};
        RawCompanion input_ss_endpoint_companion_descs[15]{}, output_ss_endpoint_companion_descs[15]{};
    };
    inline Decision DescribeMap(const mapping::Descriptor &d) {
        const auto *w = d.words;
        RawDevice device{w[0] >> 16, w[0] & 65535, w[1] >> 16, w[1] & 65535};
        RawInterface i{};
        i.interface_desc = {w[8] & 255, (w[8] >> 8) & 255, (w[8] >> 16) & 255, w[8] >> 24, (w[9] >> 16) & 255, w[9] >> 24};
        for (unsigned n = 0; n < mapping::EndpointSlots; ++n) {
            const unsigned p = mapping::HeaderWords + n * 4;
            auto &e = n < 15 ? i.input_endpoint_descs[n] : i.output_endpoint_descs[n-15];
            auto &c = n < 15 ? i.input_ss_endpoint_companion_descs[n] : i.output_ss_endpoint_companion_descs[n-15];
            e = {(w[p] >> 8) & 255, (w[p] >> 16) & 255, w[p] >> 24, w[p+1] & 65535};
            c = {w[p+2] & 255, (w[p+2] >> 8) & 255, (w[p+2] >> 16) & 255, w[p+2] >> 24, w[p+3] & 65535};
        }
        return Describe(device, i);
    }
    inline int Format(char *out, size_t size, const mapping::SnapshotData &m, const State &s) {
        mapping::Writer w{out, size};
        w.Add("usb_compatibility_version=C12\npolicy=standards_first_exact_reader_quirks\n"
              "usb3_policy=preserve_host_settings_no_global_downgrade\nnegotiated_speed=unknown_not_exposed_by_driver\n"
              "device_kind=unknown_unless_exact_reader_profile\nwrite_protection=unknown_no_extra_probe\n"
              "uas_limit=existing_serialized_transport_no_stream_id_API\n"
              "physical_port=unknown_bus_ids_are_opaque\nprofile_cache=atmosphere/config/usb_slot_preferences.bin\n"
              "cache_policy=successful_mount_LUN_hint_revalidate_every_connection\n"
              "reader_profile_version=C20\nreader_05e3_0764_status=mount_user_confirmed_C19_tested_variant\n"
              "startup_policy_version=C21\nstartup_policy=validated_BOT_SCSI_RBC_small_sense18\n"
              "startup_policy_scope=readiness_only_including_BOT_disks_not_reader_classification\n"
              "startup_policy_hardware_validation=pending_other_models\n"
              "reader_05e3_0764_match=VID05E3_PID0764_revision2961_USB0320_exact_C11_descriptors\n"
              "reader_05e3_0764_policy=automatic_startup_sense18_existing_allocation_no_speed_change\n"
              "descriptor_warning_bits=1:shape_2:duplicate_4:packet_8:companion_16:count_32:inventory_changed\n"
              "extra_USB_commands=0\ndestructive_tests=0\n"
              "map_revision=%u\nquery_result=0x%08X\ncurrent_interfaces=%u\nsaved_interfaces=%u\nlimited=%u\n"
              "saved_scope=historical_if_current_empty_or_query_failed\nsample_ns=%llu\nsaved_ns=%llu\n",
              m.revision, m.query_result, m.current_count, m.saved_count, m.limited,
              (unsigned long long)m.sample_ns, (unsigned long long)m.saved_ns);
        for (unsigned n = 0; n < m.saved_count && n < mapping::Limit; ++n) {
            const auto &d = m.interfaces[n]; const auto r = DescribeMap(d); const auto *a = d.words;
            w.Add("interface.%u=VID:%04X PID:%04X revision:%04X usb_bcd:%04X interface:%u alternate:%u\n"
                  "interface.%u.candidate=%s\ninterface.%u.bulk_in=%u bulk_out=%u packet_in=%u packet_out=%u ss_companions=%u warnings=0x%X\n"
                  "interface.%u.endpoint_allocation=urbs:%u in_bytes:%u out_bytes:%u\n"
                  "interface.%u.candidate_startup_sense_bytes=%u\n",
                  n,a[0]>>16,a[0]&65535,a[1]&65535,a[1]>>16,a[8]>>24,(a[9]>>16)&255,
                  n,Name(r.profile),n,r.bulk_in,r.bulk_out,r.in_packet,r.out_packet,r.ss_companions,r.descriptor_warnings,
                  n,r.urbs,r.in_bytes,r.out_bytes,n,StartupSenseBytes(r));
        }
        const auto *v = s.value;
        w.Add("selection_scope=last_initialize_attempt_not_presence\nselection_revision=%u\n"
              "selection_vid_pid=%08X\nselection_version=%08X\nselection_lun=%u\nselection_started_ns=%llu\n"
              "selection_stage=%u\nselection_stage_legend=0:none_1:discovered_2:allocation_selected_3:capacity_read\n"
              "selection_profile=%s\nselection_urbs=%u\nselection_in_bytes=%u\nselection_out_bytes=%u\n"
              "selection_in_packet=%u\nselection_out_packet=%u\nselection_warnings=0x%X\n"
              "capacity_blocks=%llu\ncapacity_block_size=%u\ncapacity_scope=last_successful_READ_CAPACITY_this_attempt_not_mount_proof\n",
              s.revision,uint32_t(v[Identity]),uint32_t(v[Version]),uint32_t(v[Lun]),(unsigned long long)v[StartedNs],
              uint32_t(v[Stage]),v[Stage] < 2 ? "not-selected" : Name(uint32_t(v[Selected])),uint32_t(v[Urbs]),uint32_t(v[InBytes]),uint32_t(v[OutBytes]),
              uint32_t(v[InPacket]),uint32_t(v[OutPacket]),uint32_t(v[Warnings]),(unsigned long long)v[BlockCount],uint32_t(v[BlockSize]));
        Decision selected{};
        selected.profile = static_cast<Profile>(v[Selected]);
        selected.descriptor_warnings = uint32_t(v[Warnings]);
        w.Add("selection_startup_sense_bytes=%u\nselection_startup_sense_scope=readiness_only_not_live_IO_or_mount_proof\n",
              v[Stage] < 2 ? 0u : StartupSenseBytes(selected));
        w.Add("probe_scope=separate_last_worker_attempt_not_atomic_with_selection\nprobe_valid=%u\nprobe_ns=%llu\nprobe_lun=%u\n"
              "probe_lun_count=%u\nprobe_tried_mask=0x%X\nprobe_empty_mask=0x%X\nprobe_result=0x%08X\n"
              "mounted_ready_and_inquiry=see_usb_storage_status.txt\nall_raw_endpoints_and_bursts=see_usb_device_map.txt\n"
              "end=C12\n", m.attempt_valid,(unsigned long long)m.attempt_ns,m.attempt_lun,m.lun_count,m.tried_mask,m.empty_mask,m.result);
        return w.ok ? int(w.length) : -1;
    }
}
