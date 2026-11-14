/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>

namespace ams::usb::content {
    /* Single append-only registry shared by every ams:usb client and server. */
    enum CommandId : unsigned int {
        CommandId_OpenNativeSdContentFileSystem = 28,
        CommandId_OpenPinnedUsbContentFileSystem = 29,
        CommandId_GetLegacyStatus = 0,
        CommandId_GetNroInfo      = 1,
        CommandId_ReadNro         = 2,
        CommandId_ReadDirectoryLegacy = 3,
        CommandId_Start           = 4,
        CommandId_GetStorageStatus = 5,
        CommandId_GetFileInfo      = 6,
        CommandId_ReadFile         = 7,
        CommandId_ReadDirectory    = 8,
        CommandId_GetSpaceSize     = 9,
        CommandId_CreateFile       = 10,
        CommandId_ResizeFile       = 11,
        CommandId_DeleteFile       = 12,
        CommandId_RenameFile       = 13,
        CommandId_WriteFile        = 14,
        CommandId_CreateDirectory  = 15,
        CommandId_Flush            = 16,
        CommandId_OpenCodeFileSystem = 17,
        CommandId_RecordLoaderProbe = 18,
        CommandId_RecordLoaderTrace = 19,
        CommandId_GetNcaRightsId = 20,
        /* Keep this append-only. PGL uses it to bind an installed title to
         * its USB content path without changing the stock ProgramLocation or
         * moving Horizon's metadata/ticket databases. */
        CommandId_GetApplicationContentPath = 21,
        /* Resolve only an exact, generation-bound opaque ProgramId that was
         * observed while opening a validated USB Control NCA. */
        CommandId_ResolveProgramIdAlias = 22,
        /* Read the ProgramId directly from a validated USB NCA header.  NCM
         * cannot send the private usbcontent: provider path to stock fsp-srv,
         * because the provider is owned by ams_mitm rather than Horizon FS. */
        CommandId_GetNcaProgramId = 23,
        /* Existing SD bridge commands; never renumber these. */
        CommandId_OpenSdContentFileSystem = 24,
        CommandId_GetSdContentHeader = 25,
        /* R156: distinct from the pre-existing SD bridge command 24. */
        CommandId_OpenLaunchCodeFileSystem = 26,
        CommandId_SetProcessContent = 27,
        CommandId_QuerySdIoCounters = 80, // SD1: read-only native SD attribution snapshot
    };

    template<std::size_t N>
    constexpr bool AreCommandIdsUnique(const unsigned int (&ids)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t j = i + 1; j < N; ++j) {
                if (ids[i] == ids[j]) { return false; }
            }
        }
        return true;
    }
}
