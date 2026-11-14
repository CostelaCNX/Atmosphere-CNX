/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ams::usb::reader {
    constexpr unsigned InventoryLimit = 8;
    struct Entry { uint32_t vid_pid{}, descriptor{}, location{}; };
    struct Inventory {
        uint64_t sampled_ns{};
        uint32_t all_result{}, available_result{}, all_count{}, available_count{};
        uint32_t storage_count{}, supported_count{}, available_supported{}, limited{};
        Entry entries[InventoryLimit]{};
    };
    struct AtomicEntry { std::atomic<uint32_t> vid_pid{}, descriptor{}, location{}; };
    inline std::atomic<uint32_t> revision{}, all_result{}, available_result{}, all_count{}, available_count{};
    inline std::atomic<uint32_t> storage_count{}, supported_count{}, available_supported{}, limited{};
    inline std::atomic<uint64_t> sampled_ns{}, fallback_queries{}, fallback_matches{};
    inline std::atomic<uint32_t> fallback_result{};
    inline AtomicEntry entries[InventoryLimit];

    // One worker writer, nonblocking monitor reader. All payload fields are
    // atomic: no undefined data races, no volume/USB locks in status publishing.
    inline void Publish(const Inventory &v) {
        revision.fetch_add(1);
        sampled_ns.store(v.sampled_ns); all_result.store(v.all_result); available_result.store(v.available_result);
        all_count.store(v.all_count); available_count.store(v.available_count);
        storage_count.store(v.storage_count); supported_count.store(v.supported_count);
        available_supported.store(v.available_supported); limited.store(v.limited);
        for (unsigned i=0;i<InventoryLimit;++i) {
            entries[i].vid_pid.store(v.entries[i].vid_pid);
            entries[i].descriptor.store(v.entries[i].descriptor);
            entries[i].location.store(v.entries[i].location);
        }
        revision.fetch_add(1);
    }
    inline bool Snapshot(Inventory &v, uint32_t &rev) {
        rev=revision.load(); if (rev&1) return false;
        v.sampled_ns=sampled_ns.load(); v.all_result=all_result.load(); v.available_result=available_result.load();
        v.all_count=all_count.load(); v.available_count=available_count.load();
        v.storage_count=storage_count.load(); v.supported_count=supported_count.load();
        v.available_supported=available_supported.load(); v.limited=limited.load();
        for (unsigned i=0;i<InventoryLimit;++i) {
            v.entries[i]={entries[i].vid_pid.load(),entries[i].descriptor.load(),entries[i].location.load()};
        }
        return rev==revision.load();
    }

    // Appended to the normal SD status log. A sample is historical when mounted;
    // it is never used as acquisition authority or an indication of corruption.
    inline int FormatInventory(char *out, size_t size) {
        Inventory v{};uint32_t rev{};
        if (!Snapshot(v,rev)) return util::SNPrintf(out,size,"usb_discovery_snapshot=busy\n");
        int n=util::SNPrintf(out,size,
            "usb_discovery_revision=%u\nusb_discovery_sample_ns=%llu\n"
            "usb_discovery_all_result=0x%08X\nusb_discovery_available_result=0x%08X\n"
            "usb_discovery_all_count=%u\nusb_discovery_available_count=%u\n"
            "usb_discovery_storage_count=%u\nusb_discovery_supported_count=%u\n"
            "usb_discovery_available_supported=%u\nusb_discovery_limited=%u\n"
            "usb_discovery_fallback_queries=%llu\nusb_discovery_fallback_matches=%llu\n"
            "usb_discovery_fallback_result=0x%08X\n",
            rev,static_cast<unsigned long long>(v.sampled_ns),v.all_result,v.available_result,
            v.all_count,v.available_count,v.storage_count,v.supported_count,v.available_supported,v.limited,
            static_cast<unsigned long long>(fallback_queries.load()),static_cast<unsigned long long>(fallback_matches.load()),fallback_result.load());
        if (n<=0 || static_cast<size_t>(n)>=size) return -1;
        for (unsigned i=0;i<v.all_count && i<InventoryLimit;++i) {
            const auto &e=v.entries[i];
            int part=util::SNPrintf(out+n,size-n,"usb_discovery_interface_%u=%08X/%08X/%08X\n",i,e.vid_pid,e.descriptor,e.location);
            if (part<=0 || static_cast<size_t>(part)>=size-n) return -1;
            n+=part;
        }
        return n;
    }
}
