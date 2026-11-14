/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include "../amsmitm_module.hpp"
#include "ncm_launch_trace.hpp"

namespace ams::mitm::usb_storage {

    void RecordLoaderTraceForDiagnostics(const ams::usb::content::LoaderTraceInfo &info);
    void StoreNcmLaunchTraceForDiagnostics(const ncm_trace::LookupSnapshot &snapshot,
        ncm::ProgramId application_id, u64 session, Result begin_result,
        Result end_result, Result launch_result);

    /* True only after the post-boot monitor has explicitly armed the USB
     * content route. FS/NS MITM hooks use this local gate to avoid crossing
     * into NCM during Horizon's early boot phase. */
    bool IsUsbContentLaunchPathEnabled();

    /* The clean filesystem route deliberately keeps the retired Host/NCM
     * compatibility path above disabled. NS and appletAE still need a narrow
     * runtime gate for an exact title recorded in the persistent USB maps.
     * This check is atomic-only: it never starts USB, calls NCM or performs
     * filesystem I/O. */
    bool IsUsbMappedTitleLaunchEnabled();

    /* R97 filesystem route. These accessors are deliberately non-starting:
     * command 110 may be opened during Horizon boot, so merely constructing
     * the overlay must never acquire usb:hs or wait for a device. */
    bool IsUsbContentFileSystemRouteReady(u64 *out_media_generation, bool require_writable);
    ams::usb::storage::Service *GetUsbContentStorageService();
    Result CreatePinnedUsbContentFileSystem(std::unique_ptr<ams::fs::fsa::IFileSystem> *out, u64 generation);
    Result CreateUsbContentOverlayFileSystem(
        std::unique_ptr<::ams::fs::fsa::IFileSystem> *out,
        std::shared_ptr<::ams::fs::fsa::IFileSystem> stock_file_system);

    bool EnsureUsbProgramPath(ncm::ProgramId program_id);
    bool HasUsbProgramPath(ncm::ProgramId program_id);
    bool HasCachedUsbProgramPath(ncm::ProgramId program_id);
    /* NS/applet boundary helper. Unlike HasCachedUsbProgramPath(), this may
     * read the small persistent SD-side title maps, but it never calls NCM or
     * touches USB. Do not call it from an active FS request. */
    bool PrimeUsbProgramPathFromPersistentMap(ncm::ProgramId program_id);
    ncm::ProgramId GetLastCachedUsbProgramIdForDiagnostics();
    void MarkUsbLaunchActiveForDiagnostics(ncm::ProgramId program_id);
    void ClearUsbLaunchActiveForDiagnostics(ncm::ProgramId program_id);
    ncm::ProgramId GetActiveUsbLaunchProgramIdForDiagnostics();
    /* Passive launch-flight-recorder selector. This is deliberately separate
     * from the USB-active selector above: arming an SD reference launch must
     * never enable a USB route or alter any launch decision. */
    void MarkLaunchTraceActiveForDiagnostics(ncm::ProgramId program_id);
    void ClearLaunchTraceActiveForDiagnostics(ncm::ProgramId program_id);
    ncm::ProgramId GetActiveLaunchTraceProgramIdForDiagnostics();
    Result OpenUsbProgramRomFs(std::shared_ptr<::ams::fs::IStorage> *out, ncm::ProgramId program_id);
    bool HasUsbProcessContent(u64 process_id, ncm::ProgramId program_id);
    Result OpenUsbProcessRomFs(std::shared_ptr<::ams::fs::IStorage> *out, u64 process_id, ncm::ProgramId program_id);
    bool HasUsbAddOnContent(ncm::ProgramId client, ncm::DataId data_id, u8 storage_id, u64 process_id);
    Result OpenUsbAddOnRomFs(std::shared_ptr<::ams::fs::IStorage> *out, ncm::ProgramId client, ncm::DataId data_id, u8 storage_id, u64 process_id);
    Result OpenUsbControlFileSystem(std::shared_ptr<::ams::fs::fsa::IFileSystem> *out, ncm::ProgramId *out_resolved_program_id, const char *content_path, ncm::ProgramId requested_program_id);
    struct UsbNcaHeaderInfo {
        ncm::ProgramId program_id;
        ::ams::fs::RightsId rights_id;
        u8 key_generation;
    };
    /* Exact mapped archives only; reads original headers without requesting
     * a title key or changing tickets. Unmapped paths forward unchanged. */
    Result ReadMappedUsbNcaHeader(UsbNcaHeaderInfo *out, const char *content_path);
    /* R97 exact-path equivalents. They accept only a canonical SD registered
     * NCA path and never consult title_map.bin or scan the drive. */
    Result ReadUsbNcaHeaderByPath(UsbNcaHeaderInfo *out, const char *content_path);
    Result OpenUsbControlFileSystemByPath(
        std::shared_ptr<::ams::fs::fsa::IFileSystem> *out,
        ncm::ProgramId *out_resolved_program_id,
        const char *content_path,
        ncm::ProgramId requested_program_id);
    Result OpenUsbVerificationFileSystem(std::shared_ptr<::ams::fs::fsa::IFileSystem> *out,
        const char *path, size_t path_bound, ncm::ProgramId requested, ncm::ProgramId title, u32 type);
    Result GetUsbApplicationContentPath(char *out_path, size_t out_size, ncm::ProgramId application_id, ncm::ContentType content_type);

    Result CacheExternalKey(const ::ams::fs::RightsId &rights_id, const spl::AccessKey &access_key);
    Result RemoveExternalKey(const ::ams::fs::RightsId &rights_id);
    Result RemoveAllExternalKeys();

    DEFINE_MITM_MODULE_CLASS(0x8000, AMS_GET_SYSTEM_THREAD_PRIORITY(fs, WorkerThreadPool) - 2);

}
