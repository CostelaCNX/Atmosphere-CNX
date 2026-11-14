/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>
#include <stratosphere/os/os_tick.hpp>
#include "usb_completion_probe_core.hpp"
namespace ams::usb::completion_probe {
inline os::SdkMutex mutex{};
inline Lease lease{};
inline uint64_t Now(){return static_cast<uint64_t>(os::GetSystemTick().ToTimeSpan().GetNanoSeconds());}
inline Outcome Control(uint64_t generation,uint64_t caller,const NexusUsbCompletionProbeRequest &r,NexusUsbCompletionProbeStatus *out){
    std::scoped_lock lock(mutex);return lease.Control(Now(),generation,caller,r,out);
}
inline uint32_t SelectedMode(){std::scoped_lock lock(mutex);return lease.Mode(lease.Active()?Now():0);}
inline bool IsActive(){std::scoped_lock lock(mutex);lease.Mode(lease.Active()?Now():0);return lease.Active();}
inline void Invalidate(){std::scoped_lock lock(mutex);lease.Invalidate();}
inline void Count(Counter counter){std::scoped_lock lock(mutex);lease.Count(counter);}
}
