/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>
#include <stratosphere/os/os_tick.hpp>
#include "usb_write_cache_protocol.h"

namespace ams::usb::write_cache_diagnostics {
    inline os::SdkMutex mutex{};
    inline NexusUsbWriteCacheSnapshot counters{};
    inline uint64_t Add(uint64_t a,uint64_t b) { return b>UINT64_MAX-a?UINT64_MAX:a+b; }
    // The lock covers scalar telemetry only. Never hold it during USB I/O or
    // while touching the clean-sector byte cache.
    inline void LogicalWrite(uint32_t sectors) {
        std::scoped_lock lock(mutex);
        counters.logical_write_calls=Add(counters.logical_write_calls,1);
        counters.logical_write_sectors=Add(counters.logical_write_sectors,sectors);
    }
    inline void Elided(uint32_t bytes) {
        std::scoped_lock lock(mutex);
        counters.elided_write_calls=Add(counters.elided_write_calls,1);
        counters.elided_write_bytes=Add(counters.elided_write_bytes,bytes);
    }
    inline void State(uint32_t sector_size,uint32_t cached_sectors) {
        std::scoped_lock lock(mutex);
        counters.sector_size=sector_size;counters.cached_sectors=cached_sectors;
    }
    inline void Clear(uint32_t sector_size) {
        std::scoped_lock lock(mutex);
        counters.cache_invalidations=Add(counters.cache_invalidations,1);
        counters.sector_size=sector_size;counters.cached_sectors=0;
    }
    inline bool Snapshot(NexusUsbWriteCacheSnapshot *out,uint64_t generation) {
        if (!out) return false;
        std::scoped_lock lock(mutex);
        *out=counters;out->magic=NEXUS_USB_WRITE_CACHE_MAGIC;out->version=NEXUS_USB_WRITE_CACHE_VERSION;
        out->size=sizeof(*out);out->capacity=NEXUS_USB_WRITE_CACHE_CAPACITY;
        out->media_generation=generation;
        out->sampled_ns=static_cast<uint64_t>(os::GetSystemTick().ToTimeSpan().GetNanoSeconds());
        return true;
    }
}
