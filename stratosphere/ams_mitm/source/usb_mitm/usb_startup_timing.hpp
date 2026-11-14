/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_startup_timing_protocol.h>
namespace ams::mitm::usb_storage {
u64 GetStartupTimingCapture(u64 pid,u32 source);
void RetireStartupTimingCapture(u64 pid);
void PollStartupTimingAutosave();
int FormatStartupTimingAutosaveStatus(char *out,size_t capacity);
std::unique_ptr<fs::fsa::IFileSystem> WrapStartupTimingFileSystem(
    std::shared_ptr<fs::fsa::IFileSystem> base,u64 capture);
}
