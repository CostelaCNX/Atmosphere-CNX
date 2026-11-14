/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>
#include <stratosphere/os/os_tick.hpp>
#include "usb_write_transfer_core.hpp"
namespace ams::usb::write_transfer {
inline os::SdkMutex mutex{};
inline Lease lease{};
inline uint64_t Now(){return static_cast<uint64_t>(os::GetSystemTick().ToTimeSpan().GetNanoSeconds());}
// Never held around USB I/O. Service mutations first own the volume/state locks.
inline Outcome Control(uint64_t generation,uint64_t caller,const NexusUsbWriteTransferRequest &r,NexusUsbWriteTransferStatus *out){
    std::scoped_lock lock(mutex);return lease.Control(Now(),generation,caller,r,out);
}
inline uint32_t SelectedBytes(){
    std::scoped_lock lock(mutex);return lease.Limit(lease.Active()?Now():0);
}
inline bool IsActive(){std::scoped_lock lock(mutex);lease.Limit(lease.Active()?Now():0);return lease.Active();}
inline void Invalidate(){std::scoped_lock lock(mutex);lease.Invalidate();}
}
