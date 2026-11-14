/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stratosphere.hpp>

namespace ams::mitm::ns {

    /* NCM IPC is isolated on a dedicated client thread. Forward observers run
     * with qlaunch's response in TLS and must never issue nested IPC there. */
    Result StartNcmLaunchTraceWorker();
    Result BeginNcmLaunchTrace(ncm::ProgramId application_id);
    Result EndNcmLaunchTrace(ncm::ProgramId application_id, Result launch_result);

}
