/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>
namespace ams::mitm::usb_storage {
    struct UsbLaunchSnapshot;
    using LaunchPin = std::shared_ptr<const UsbLaunchSnapshot>;
    Result OpenPinnedProcessRomFs(std::shared_ptr<::ams::fs::IStorage> *out, LaunchPin *pin,
                                  u64 process_id, ncm::ProgramId program_id);
    void QueueWakeComparison(const LaunchPin &pin, u32 open_epoch, u32 read_epoch,
                             s64 offset, size_t size, Result original);
}
