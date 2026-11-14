/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>
#include "usb_hang_capture_model.hpp"

namespace ams::mitm::usb_storage::hang_capture {
    void RegisterCurrentThread(Role role);
    void RegisterFsThread();
    void StartOneShot();
    void NotifySelection(ncm::ProgramId client, ncm::ProgramId title);
    void NotifyStart(ncm::ProgramId client, ncm::ProgramId title, Result start_result);
    void Cancel(ncm::ProgramId title);
    void BeatMonitor(u32 phase);
    void BeatPublisher();
    u32 GetState();
    u32 GetSetupResult();
    Progress GetProgress();
}
