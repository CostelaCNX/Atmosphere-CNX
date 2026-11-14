/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>

namespace ams::ncm {
    Result GetUsbAwareRightsId(RightsId *out, const Path &path, fs::ContentAttributes attr);
    Result GetUsbAwareProgramId(ProgramId *out, const Path &path, fs::ContentAttributes attr);
}
