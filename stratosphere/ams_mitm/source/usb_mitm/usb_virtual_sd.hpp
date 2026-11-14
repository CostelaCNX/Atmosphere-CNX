// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stratosphere.hpp>
#include "usb_virtual_sd_policy.hpp"
#include "nexus_vsd_protocol.h"
namespace ams::mitm::usb_storage {
namespace fs = ::ams::fs;
struct VirtualSdLease {
    nexus_vsd::Profile profile{};
    u64 pid{},serial{},generation{};
};
bool GetVirtualSdLease(VirtualSdLease *out,u64 pid);
Result CheckVirtualSdLease(const VirtualSdLease &lease);
// Starts once before ams:usb is exposed. No storage, FS or SM dependency.
void InitializeVirtualSdReaper();
struct VirtualSdDiagnostics {
    u64 polls{}, retained{}, detached{}, closed{}, last_pid{}, last_ms{};
    u32 watched{}, query_errors{}, last_query_error{}, init_result{};
    bool running{};
};
// Best effort: never hold the VSD lock while formatting or writing a log.
bool GetVirtualSdDiagnostics(VirtualSdDiagnostics *out);
Result CreateVirtualSdFileSystem(std::unique_ptr<fs::fsa::IFileSystem> *out,
    const VirtualSdLease &lease,std::shared_ptr<fs::fsa::IFileSystem> anchor);
}
