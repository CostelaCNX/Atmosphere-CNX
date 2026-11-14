// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stratosphere.hpp>
#include <atomic>
#include "../usb_mitm/usb_launch_trace.hpp"
namespace ams::mitm::ns::handoff {
inline std::atomic<u64> serial{};
inline u64 Next() { return serial.fetch_add(1) + 1; }
// In-memory publication only: no filesystem, USB, NS, NCM, waits or resets.
// A begin without its matching call/end identifies the wait boundary, not
// proof of a bad device. Native request bytes and results remain untouched.
inline void Record(u64 call, ncm::ProgramId client, u32 command, u32 object,
        const char* phase, Result rc = ResultSuccess()) {
    char text[0x80]{};
    util::SNPrintf(text, sizeof(text), "S1-ns call=%llu ms=%lld c=%u o=%u phase=%s",
        static_cast<unsigned long long>(call),
        static_cast<long long>(os::GetSystemTick().ToTimeSpan().GetMilliSeconds()), command, object, phase);
    usb_storage::RecordLaunchBoundaryTrace(static_cast<usb::content::LoaderProbeStage>(162), client, 0, rc, text);
}
}
