/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::ldr {

    enum class NsPrePglPatchApplyStatus : u32 {
        NotObserved          = 0,
        ExactPatchApplied    = 1,
        InvalidEntryCount    = 2,
        EntryOutOfBounds     = 3,
        OriginalByteMismatch = 4,
        UnknownNsMainModule  = 5,
    };

    enum class NsPrePglPatchFinalStatus : u32 {
        NotObserved          = 0,
        ReplacementsVerified = 1,
        OriginalsPresent     = 2,
        MixedOrOtherBytes    = 3,
        EntryOutOfBounds     = 4,
        UnknownNsMainModule  = 5,
    };

    struct NsPrePglPatchDiagnostic {
        NsPrePglPatchApplyStatus apply_status;
        NsPrePglPatchFinalStatus final_status;
        u32 pre_patch_words[2];
        u32 final_words[2];
        u32 module_id_words[2];
        u64 mapped_size;
    };

    /* Apply IPS patches. */
    void LocateAndApplyIpsPatchesToModule(const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size);

    /* Apply embedded patches. */
    void ApplyEmbeddedPatchesToModule(const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size);
    void FinalizeEmbeddedPatchDiagnostics(ncm::ProgramId program_id, bool is_main_nso, const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size);
    NsPrePglPatchDiagnostic GetNsPrePglPatchDiagnostic();

}
