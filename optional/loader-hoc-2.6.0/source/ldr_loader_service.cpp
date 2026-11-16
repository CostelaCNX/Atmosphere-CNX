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
#include <stratosphere.hpp>
#include "ldr_argument_store.hpp"
#include "ldr_content_management.hpp"
#include "ldr_development_manager.hpp"
#include "ldr_process_creation.hpp"
#include "ldr_launch_record.hpp"
#include "ldr_loader_service.hpp"

namespace ams::ldr {

    namespace {

        constinit ArgumentStore g_argument_store;

        Result TryResolveUsbProgramAlias(ncm::ProgramLocation *out, const ncm::ProgramLocation &loc) {
            R_UNLESS(out != nullptr, ncm::ResultInvalidOperation());
            *out = loc;

            if constexpr (usb::content::UseFileSystemContentRoute) {
                /* ProgramLocation stays canonical in R97; there is no opaque
                 * Host alias to repair in Loader. */
                R_THROW(ncm::ResultContentNotFound());
            }

            /* This is deliberately a failure-only Loader fallback. PGL no
             * longer asks ams:usb about ordinary SD launches, and Loader calls
             * this helper only after stock path/meta loading has already
             * failed. This mirrors the replacement-Loader boundary observed
             * in SX OS without placing USB IPC in every homebrew launch. */
            const auto storage_id = static_cast<ncm::StorageId>(loc.storage_id);
            R_UNLESS((storage_id == ncm::StorageId::SdCard || storage_id == ncm::StorageId::Host)
                     && !ncm::IsSystemProgramId(loc.program_id)
                     && !ncm::IsApplicationId(loc.program_id),
                     ncm::ResultContentNotFound());

            ncm::ProgramId resolved_program_id{};
            /* Prefer the durable SD map. The previous implementation asked
             * ams:usb first, but that command only searched its in-memory
             * alias cache; after a reboot the cache could be empty even when
             * title_map.bin contained the validated association. This direct
             * lookup runs only after the stock Loader attempt has completed,
             * so it does not put USB IPC into the normal launch path. */
            Result alias_result = ncm::ResolveUsbTitleAlias(std::addressof(resolved_program_id), loc.program_id);
            if (R_FAILED(alias_result)) {
                /* Keep the runtime cache as a compatibility fallback for an
                 * alias learned during the current media generation but not
                 * persisted yet. */
                os::NativeHandle service_handle;
                R_TRY(sm::GetServiceHandle(std::addressof(service_handle), sm::ServiceName::Encode(usb::content::ServiceName)));

                Service usb_service{};
                serviceCreate(std::addressof(usb_service), service_handle);
                ON_SCOPE_EXIT { serviceClose(std::addressof(usb_service)); };

                alias_result = serviceDispatchInOut(std::addressof(usb_service), usb::content::CommandId_ResolveProgramIdAlias, loc.program_id, resolved_program_id);
            }
            R_TRY(alias_result);
            R_UNLESS(resolved_program_id != loc.program_id && !ncm::IsSystemProgramId(resolved_program_id), ncm::ResultContentNotFound());

            /* Match the PGL handoff: Host is the USB byte provider, while the
             * installed title's authoritative content-meta database remains
             * on the redirected SD storage. */
            const ncm::StorageId resolved_storage_id = storage_id == ncm::StorageId::Host
                ? ncm::StorageId::SdCard
                : storage_id;
            *out = ncm::ProgramLocation::Make(resolved_program_id, resolved_storage_id);
            R_SUCCEED();
        }

        bool IsValidProgramAttributes(const ldr::ProgramAttributes &attrs) {
            /* Check that the attributes are valid; on NX, this means Platform = NX + no content attrs. */
            if (attrs.platform != ncm::ContentMetaPlatform::Nx) {
                return false;
            }
            if (attrs.content_attributes != fs::ContentAttributes_None) {
                return false;
            }

            return true;
        }

    }


    Result LoaderService::CreateProcess(os::NativeHandle *out, PinId pin_id, u32 flags, os::NativeHandle resource_limit, const ProgramAttributes &attrs) {
        /* Check that the program attributes are valid. */
        R_UNLESS(IsValidProgramAttributes(attrs), ldr::ResultInvalidProgramAttributes());

        /* Get the location and override status. */
        ncm::ProgramLocation loc;
        cfg::OverrideStatus override_status;
        R_TRY(ldr::GetProgramLocationAndOverrideStatusFromPinId(std::addressof(loc), std::addressof(override_status), pin_id));

        /* Get the program path. */
        char path[fs::EntryNameLengthMax];
        RecordUsbLoaderTraceForDiagnostics(usb::content::LoaderProbeStage::LoaderCreateProcessAttempt, loc);
        const Result path_result = GetProgramPath(path, sizeof(path), loc, attrs);
        RecordUsbLoaderTraceForDiagnostics(usb::content::LoaderProbeStage::LoaderCreateProcessPathResult, loc, path_result);
        R_TRY(path_result);
        path[sizeof(path) - 1] = '\x00';

        /* Create the process. */
        R_RETURN(ldr::CreateProcess(out, pin_id, loc, override_status, path, g_argument_store.Get(loc.program_id), flags, resource_limit, attrs));
    }

    Result LoaderService::GetProgramInfo(ProgramInfo *out, cfg::OverrideStatus *out_status, const ncm::ProgramLocation &loc, const ProgramAttributes &attrs) {
        /* Check that the program attributes are valid. */
        R_UNLESS(IsValidProgramAttributes(attrs), ldr::ResultInvalidProgramAttributes());

        /* The diagnostics and USB resolver perform nested service calls. Copy
         * the request out of loader's IPC message before any of those calls
         * can reuse the thread-local message buffer. */
        const ncm::ProgramLocation requested_location = loc;

        char path[fs::EntryNameLengthMax];
        cfg::OverrideStatus status;

        auto load_program_info = [&](const ncm::ProgramLocation &candidate_location) -> Result {
            std::memset(out, 0, sizeof(*out));
            std::memset(path, 0, sizeof(path));

            RecordUsbLoaderTraceForDiagnostics(usb::content::LoaderProbeStage::LoaderGetProgramInfoAttempt, candidate_location);
            const Result path_result = GetProgramPath(path, sizeof(path), candidate_location, attrs);
            RecordUsbLoaderTraceForDiagnostics(usb::content::LoaderProbeStage::LoaderGetProgramInfoPathResult, candidate_location, path_result);
            R_TRY(path_result);
            path[sizeof(path) - 1] = '\x00';

            R_RETURN(ldr::GetProgramInfo(out, std::addressof(status), candidate_location, path, attrs));
        };

        ncm::ProgramLocation effective_location = requested_location;
        Result program_info_result = load_program_info(effective_location);
        bool used_usb_alias_fallback = false;

        if (R_FAILED(program_info_result)) {
            /* The stock request has completed and released its FS call before
             * this IPC is made, avoiding an FS -> ams:usb -> Loader cycle. An
             * exact alias exists only after a validated USB Control NCA was
             * opened for the current media generation. */
            RecordUsbLoaderTraceForDiagnostics(usb::content::LoaderProbeStage::LoaderAliasFallbackAttempt, requested_location, program_info_result);

            ncm::ProgramLocation resolved_location = requested_location;
            const Result alias_result = TryResolveUsbProgramAlias(std::addressof(resolved_location), requested_location);
            RecordUsbLoaderTraceForDiagnostics(usb::content::LoaderProbeStage::LoaderAliasFallbackResult,
                                               R_SUCCEEDED(alias_result) ? resolved_location : requested_location,
                                               alias_result);
            if (R_FAILED(alias_result)) {
                R_RETURN(program_info_result);
            }

            effective_location = resolved_location;
            used_usb_alias_fallback = true;
            program_info_result = load_program_info(effective_location);
        }
        R_TRY(program_info_result);

        if (!used_usb_alias_fallback && requested_location.program_id != out->program_id) {
            /* Redirect the program path. */
            const ncm::ProgramLocation new_loc = ncm::ProgramLocation::Make(out->program_id, static_cast<ncm::StorageId>(requested_location.storage_id));
            R_TRY(RedirectProgramPath(path, sizeof(path), new_loc));
        }

        /* Update arguments when NPDM or the USB alias recovered a different
         * real ProgramId for the process that PM will pin. */
        if (requested_location.program_id != out->program_id) {
            if (const auto *entry = g_argument_store.Get(requested_location.program_id); entry != nullptr) {
                R_TRY(g_argument_store.Set(out->program_id, entry->argument, entry->argument_size));
            }
        }

        /* If we should, set the output status. */
        if (out_status != nullptr) {
            *out_status = status;
        }

        R_SUCCEED();
    }

    Result LoaderService::PinProgram(PinId *out, const ncm::ProgramLocation &loc, const cfg::OverrideStatus &status) {
        *out = {};
        const ncm::ProgramLocation requested_location = loc;
        R_RETURN(ldr::PinProgram(out, requested_location, status));
    }

    Result LoaderService::UnpinProgram(PinId id) {
        R_RETURN(ldr::UnpinProgram(id));
    }

    Result LoaderService::SetProgramArgument(ncm::ProgramId program_id, const void *argument, size_t size) {
        R_RETURN(g_argument_store.Set(program_id, argument, size));
    }

    Result LoaderService::FlushArguments() {
        R_RETURN(g_argument_store.Flush());
    }

    Result LoaderService::GetProcessModuleInfo(u32 *out_count, ModuleInfo *out, size_t max_out_count, os::ProcessId process_id) {
        *out_count = 0;
        std::memset(out, 0, max_out_count * sizeof(*out));
        R_RETURN(ldr::GetProcessModuleInfo(out_count, out, max_out_count, process_id));
    }

    Result LoaderService::RegisterExternalCode(os::NativeHandle *out, ncm::ProgramId program_id) {
        R_RETURN(fssystem::CreateExternalCode(out, program_id));
    }

    void LoaderService::UnregisterExternalCode(ncm::ProgramId program_id) {
        fssystem::DestroyExternalCode(program_id);
        ClearUsbExternalCodeVerification(program_id);
    }

    void LoaderService::HasLaunchedBootProgram(bool *out, ncm::ProgramId program_id) {
        *out = ldr::HasLaunchedBootProgram(program_id);
    }

    Result LoaderService::SetEnabledProgramVerification(bool enabled) {
        ldr::SetEnabledProgramVerification(enabled);
        R_SUCCEED();
    }

}
