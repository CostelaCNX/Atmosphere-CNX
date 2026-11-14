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
#include <stratosphere/ncm/ncm_usb_title_map.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include "ldr_content_management.hpp"
#include "ldr_patcher.hpp"
#include "ldr_usb_launch_selection.hpp"

namespace ams::ldr {

    namespace {

        constexpr inline size_t MaxUsbExternalCodeVerificationEntries = 0x10;

        os::SdkMutex g_usb_external_code_verification_mutex;
        util::BoundedMap<ncm::ProgramId, fs::CodeVerificationData, MaxUsbExternalCodeVerificationEntries> g_usb_external_code_verification;

        bool IsUsbContentPath(const char *path) {
            constexpr const char UsbContentPrefix[] = "usbcontent:";
            return path != nullptr && std::strncmp(path, UsbContentPrefix, sizeof(UsbContentPrefix) - 1) == 0;
        }

        bool IsUsbUserProgramLocation(const ncm::ProgramLocation &loc) {
            const auto storage_id = static_cast<ncm::StorageId>(loc.storage_id);
            return !ncm::IsSystemProgramId(loc.program_id)
                && (storage_id == ncm::StorageId::SdCard
                    || storage_id == ncm::StorageId::BuiltInUser
                    || storage_id == ncm::StorageId::Host);
        }

        bool TryGetUsbContentServiceHandle(os::NativeHandle *out) {
            AMS_ASSERT(out != nullptr);

            const auto service_name = sm::ServiceName::Encode(usb::content::ServiceName);
            bool has_service = false;
            if (R_FAILED(sm::HasService(std::addressof(has_service), service_name)) || !has_service) {
                return false;
            }

            return R_SUCCEEDED(sm::GetServiceHandle(out, service_name));
        }

        Result RememberUsbExternalCodeVerification(ncm::ProgramId program_id, const fs::CodeVerificationData &verification) {
            std::scoped_lock lk(g_usb_external_code_verification_mutex);
            g_usb_external_code_verification.Remove(program_id);
            R_UNLESS(g_usb_external_code_verification.Emplace(program_id, verification), fs::ResultAllocationMemoryFailedInCodeA());
            R_SUCCEED();
        }

        Result MountBoundUsbExternalCode(fs::CodeVerificationData *out_verification, const char *mount_name, ncm::ProgramId program_id) {
            R_UNLESS(out_verification != nullptr && mount_name != nullptr, fs::ResultInvalidArgument());

            auto *external_code = fssystem::GetExternalCodeFileSystem(program_id);
            R_UNLESS(external_code != nullptr, fs::ResultPathNotFound());

            {
                std::scoped_lock lk(g_usb_external_code_verification_mutex);
                const auto *verification = g_usb_external_code_verification.Find(program_id);
                R_UNLESS(verification != nullptr, fs::ResultPathNotFound());
                *out_verification = *verification;
            }

            /* The external-code map owns the remote filesystem until process
             * creation completes. Register a non-owning root view for each of
             * Loader's base mount names instead of reopening ams:usb. */
            auto view = std::make_unique<fssystem::SubDirectoryFileSystem>(external_code);
            R_UNLESS(view != nullptr, fs::ResultAllocationMemoryFailedInCodeA());
            constexpr const fs::Path RootPath = fs::MakeConstantPath("/");
            R_TRY(view->Initialize(RootPath));
            R_RETURN(fs::fsa::Register(mount_name, std::move(view)));
        }

        void RecordUsbLoaderProbe(usb::content::LoaderProbeStage stage, const ncm::ProgramLocation &loc, Result result = ResultSuccess(), ncm::StorageId metadata_storage = ncm::StorageId::None) {
            os::NativeHandle service_handle;
            if (!TryGetUsbContentServiceHandle(std::addressof(service_handle))) {
                return;
            }

            Service usb_service{};
            serviceCreate(std::addressof(usb_service), service_handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(usb_service)); };

            const usb::content::LoaderProbeInfo info{
                .program_id      = loc.program_id.value,
                .stage           = static_cast<u32>(stage),
                .result          = result.GetValue(),
                .location_storage = loc.storage_id,
                .metadata_storage = static_cast<u8>(metadata_storage),
                .reserved        = {},
            };
            const Result record_result = serviceDispatchIn(std::addressof(usb_service), usb::content::CommandId_RecordLoaderProbe, info);
            AMS_UNUSED(record_result);
        }

        void RecordUsbLoaderTrace(usb::content::LoaderProbeStage stage, const ncm::ProgramLocation &loc, Result result = ResultSuccess(), ncm::StorageId metadata_storage = ncm::StorageId::None, ncm::StorageId content_storage = ncm::StorageId::None, const ncm::ContentId *content_id = nullptr, const char *path = nullptr) {
            os::NativeHandle service_handle;
            if (!TryGetUsbContentServiceHandle(std::addressof(service_handle))) {
                return;
            }

            Service usb_service{};
            serviceCreate(std::addressof(usb_service), service_handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(usb_service)); };

            usb::content::LoaderTraceInfo info{};
            info.program_id = loc.program_id.value;
            info.stage = static_cast<u32>(stage);
            info.result = result.GetValue();
            info.location_storage = loc.storage_id;
            info.metadata_storage = static_cast<u8>(metadata_storage);
            info.content_storage = static_cast<u8>(content_storage);
            if (content_id != nullptr) {
                std::memcpy(info.content_id, content_id->uuid.data, sizeof(info.content_id));
            }
            if (path != nullptr) {
                const size_t path_length = std::strlen(path);
                const size_t copied_length = std::min(path_length, sizeof(info.path) - 1);
                std::memcpy(info.path, path, copied_length);
                info.path[copied_length] = '\x00';
                if (copied_length < path_length) {
                    info.flags |= usb::content::LoaderTraceFlag_PathTruncated;
                }
            }

            const Result record_result = serviceDispatchIn(std::addressof(usb_service), usb::content::CommandId_RecordLoaderTrace, info);
            AMS_UNUSED(record_result);
        }

        void RecordNsPrePglPatchDiagnostic(const ncm::ProgramLocation &loc) {
            const auto diagnostic = GetNsPrePglPatchDiagnostic();
            char text[sizeof(usb::content::LoaderTraceInfo::path)]{};
            util::SNPrintf(text, sizeof(text),
                "ns-patch a=%u f=%u pre=%08X/%08X post=%08X/%08X mid=%08X%08X sz=%llX ldr=MP1",
                static_cast<u32>(diagnostic.apply_status),
                static_cast<u32>(diagnostic.final_status),
                diagnostic.pre_patch_words[0], diagnostic.pre_patch_words[1],
                diagnostic.final_words[0], diagnostic.final_words[1],
                diagnostic.module_id_words[1], diagnostic.module_id_words[0],
                static_cast<unsigned long long>(diagnostic.mapped_size));
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::NsPrePglPatchDiagnostic, loc, ResultSuccess(), ncm::StorageId::None, ncm::StorageId::None, nullptr, text);
        }

        Result BindUsbExternalCode(const char *path, const ncm::ProgramLocation &loc) {
            R_UNLESS(path != nullptr, fs::ResultInvalidPath());
            R_UNLESS(IsUsbContentPath(path), fs::ResultInvalidPath());

            const size_t path_length = strnlen(path, fs::EntryNameLengthMax);
            R_UNLESS(path_length > 0 && path_length < fs::EntryNameLengthMax, fs::ResultInvalidPath());

            /* Keep the bounded snapshot out of Loader's 16 KiB heap and small
             * request stack. Never resolve metadata from an active FS hook. */
            static os::SdkMutex selection_mutex;
            static usb::content::LaunchContent selection;
            std::scoped_lock selection_lock(selection_mutex);
            ncm::ContentMetaDatabase db;
            ncm::ContentStorage host;
            R_TRY(ncm::OpenContentMetaDatabase(std::addressof(db), ncm::StorageId::SdCard));
            R_TRY(ncm::OpenContentStorage(std::addressof(host), ncm::StorageId::Host));
            bool is_usb = false;
            const Result selection_result = SelectUsbLaunchContent(std::addressof(selection), db, host, loc.program_id, std::addressof(is_usb));
            char selection_text[128]{};
            util::SNPrintf(selection_text, sizeof(selection_text), "MP1 app=%016llx index=%u patch=%016llx version=%u addons=%u usb=%u",
                static_cast<unsigned long long>(selection.application_id),
                static_cast<u32>(is_usb ? selection.program_id - selection.application_id : 0),
                static_cast<unsigned long long>(selection.patch_id), selection.patch_version, selection.add_on_count, is_usb);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::MetadataLookup, loc, selection_result,
                ncm::StorageId::SdCard, ncm::StorageId::Host, nullptr, selection_text);
            R_TRY(selection_result);
            R_UNLESS(is_usb, ncm::ResultContentNotFound());

            os::NativeHandle service_handle;
            R_TRY(sm::GetServiceHandle(std::addressof(service_handle), sm::ServiceName::Encode(usb::content::ServiceName)));

            Service usb_service{};
            serviceCreate(std::addressof(usb_service), service_handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(usb_service)); };

            fs::CodeVerificationData verification{};
            Service filesystem_service{};
            auto filesystem_guard = SCOPE_GUARD {
                if (serviceIsActive(std::addressof(filesystem_service))) {
                    serviceClose(std::addressof(filesystem_service));
                }
            };

            R_TRY(serviceDispatchIn(std::addressof(usb_service), usb::content::CommandId_OpenLaunchCodeFileSystem, loc.program_id,
                .buffer_attrs = {
                    SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                    SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                },
                .buffers = {
                    { std::addressof(verification), sizeof(verification) },
                    { std::addressof(selection), sizeof(selection) },
                },
                .out_num_objects = 1,
                .out_objects = std::addressof(filesystem_service)));

            FsFileSystem remote_file_system{};
            remote_file_system.s = filesystem_service;
            R_TRY(fssystem::RegisterExternalCodeFileSystem(loc.program_id, remote_file_system));

            const Result remember_result = RememberUsbExternalCodeVerification(loc.program_id, verification);
            if (R_FAILED(remember_result)) {
                fssystem::DestroyExternalCode(loc.program_id);
                R_RETURN(remember_result);
            }

            /* The external-code map now owns the returned service object. */
            filesystem_guard.Cancel();
            R_SUCCEED();
        }

        bool TryResolveUsbProgramPath(lr::Path *out, const ncm::ProgramLocation &loc, Result *out_usb_error = nullptr) {
            AMS_ASSERT(out != nullptr);

            const auto storage_id = static_cast<ncm::StorageId>(loc.storage_id);
            /* KIP-loaded and BuiltInSystem programs are created before NCM and
             * ams:usb are available. Never let their Loader requests enter an
             * optional service path; doing so forms a boot dependency cycle. */
            if (!IsUsbUserProgramLocation(loc)) {
                return false;
            }

            if constexpr (usb::content::UseFileSystemContentRoute) {
                /* R97 records USB installs as ordinary SD titles. BuiltInUser
                 * remains native NAND and must never be redirected merely
                 * because an unrelated USB drive is mounted. */
                if (storage_id != ncm::StorageId::SdCard) {
                    return false;
                }
            }

            /* The persistent map is the authoritative opt-in boundary. Do not
             * reject a mapped launch because a prior status sample briefly
             * reported the medium as not ready; the exact Host Has/GetPath
             * operations below perform the bounded live-device check. This
             * keeps DBI and every unmapped SD application entirely on stock
             * Loader behavior while allowing a mapped title to recover from a
             * transient worker/status race at the launch boundary. */
            if (!usb::content::UseFileSystemContentRoute
                && !ncm::IsUsbTitleMapped(loc.program_id)) {
                RecordUsbLoaderProbe(usb::content::LoaderProbeStage::UsbTitleMapMiss, loc, ncm::ResultContentNotFound());
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbTitleMapMiss, loc, ncm::ResultContentNotFound());
                return false;
            }

            /* Initialize the remote NCM client only after a user-storage title
             * has passed the durable USB-map gate. Ordinary system startup and
             * unmapped applications therefore remain entirely stock. */
            static bool ncm_client_initialized = false;
            if (!ncm_client_initialized) {
                ncm::Initialize();
                ncm_client_initialized = true;
            }

            if (usb::content::UseFileSystemContentRoute
                && !ncm::IsUsbContentRouteReady()) {
                return false;
            }

            RecordNsPrePglPatchDiagnostic(loc);
            RecordUsbLoaderProbe(usb::content::LoaderProbeStage::Entered, loc);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::Entered, loc);
            RecordUsbLoaderProbe(usb::content::LoaderProbeStage::UsbTitleMapHit, loc);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbTitleMapHit, loc);

            /* USB installs retain their content meta in the requested stock
             * user database, but place the NCA bytes in Host storage. Run this
             * lookup from loader (an external NCM client), never from NCM's own
             * LR server thread, so it cannot deadlock NCM during boot. */
            /* R144: this lookup needs only the content ID. The ContentInfo
             * overload calls the remote GetContentInfoByType TODO/abort stub
             * (confirmed by the R143 Loader fatal report). The ContentId
             * overload uses the implemented GetContentIdByType binding. */
            ncm::ContentId program_content_id{};
            ncm::StorageId metadata_storage = ncm::StorageId::None;
            Result metadata_result = ncm::ResultContentMetaNotFound();
            const std::array metadata_candidates = storage_id == ncm::StorageId::BuiltInUser
                ? std::array{ncm::StorageId::BuiltInUser, ncm::StorageId::SdCard}
                : std::array{ncm::StorageId::SdCard, ncm::StorageId::BuiltInUser};

            for (const auto candidate : metadata_candidates) {
                if (usb::content::UseFileSystemContentRoute
                    && candidate != ncm::StorageId::SdCard) {
                    continue;
                }
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::MetadataDatabaseOpen, loc, ResultSuccess(), candidate);
                ncm::ContentMetaDatabase content_meta_database;
                metadata_result = ncm::OpenContentMetaDatabase(std::addressof(content_meta_database), candidate);
                if (R_FAILED(metadata_result)) {
                    RecordUsbLoaderTrace(usb::content::LoaderProbeStage::MetadataDatabaseOpen, loc, metadata_result, candidate);
                    continue;
                }

                /* The first lookup must understand collection program indexes
                 * too: otherwise the later snapshot selector is never reached. */
                ncm::ContentMetaKey program_meta_key{};
                metadata_result = ResolveUsbProgramContent(std::addressof(program_meta_key), std::addressof(program_content_id), content_meta_database, loc.program_id);
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::MetadataLookup, loc, metadata_result, candidate, ncm::StorageId::None, R_SUCCEEDED(metadata_result) ? std::addressof(program_content_id) : nullptr);
                if (R_SUCCEEDED(metadata_result)) {
                    metadata_storage = candidate;
                    break;
                }
            }

            if (metadata_storage == ncm::StorageId::None) {
                RecordUsbLoaderProbe(usb::content::LoaderProbeStage::MetadataNotFound, loc, metadata_result);
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::MetadataNotFound, loc, metadata_result);
                return false;
            }
            RecordUsbLoaderProbe(usb::content::LoaderProbeStage::MetadataFound, loc, ResultSuccess(), metadata_storage);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::MetadataFound, loc, ResultSuccess(), metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id));

            /* This helper is called only after stock path resolution or the
             * first stock code mount has failed. Do not repeat a stock-content
             * presence test here; resolve the exact Program content ID against
             * Host and leave every other title on its original error path.
             *
             * Metadata remains on the requested stock database, but the NCA
             * bytes are always owned by Host in the USB route. The SD-facing
             * filesystem overlay does not change the storage object returned
             * by NCM, so querying metadata_storage here can never find the
             * physical USB content. */
            constexpr ncm::StorageId byte_storage = ncm::StorageId::Host;
            ncm::ContentStorage usb_storage;
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostOpenAttempt, loc, ResultSuccess(), metadata_storage, byte_storage, std::addressof(program_content_id));
            const Result host_open_result = ncm::OpenContentStorage(std::addressof(usb_storage), byte_storage);
            if (R_FAILED(host_open_result)) {
                RecordUsbLoaderProbe(usb::content::LoaderProbeStage::HostOpenFailed, loc, host_open_result, metadata_storage);
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostOpenAttempt, loc, host_open_result, metadata_storage, byte_storage, std::addressof(program_content_id));
                return false;
            }

            bool usb_has_content = false;
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostContentCheck, loc, ResultSuccess(), metadata_storage, byte_storage, std::addressof(program_content_id));
            const Result has_result = usb_storage.Has(std::addressof(usb_has_content), program_content_id);
            if (R_FAILED(has_result) || !usb_has_content) {
                RecordUsbLoaderProbe(usb::content::LoaderProbeStage::HostContentMissing, loc, R_FAILED(has_result) ? has_result : ncm::ResultContentNotFound(), metadata_storage);
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostContentCheck, loc, R_FAILED(has_result) ? has_result : ncm::ResultContentNotFound(), metadata_storage, byte_storage, std::addressof(program_content_id));
                return false;
            }

            static_assert(sizeof(lr::Path) == sizeof(ncm::Path));
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostPathLookup, loc, ResultSuccess(), metadata_storage, byte_storage, std::addressof(program_content_id));
            const Result path_result = usb_storage.GetPath(reinterpret_cast<ncm::Path *>(out), program_content_id);
            if (R_FAILED(path_result)) {
                RecordUsbLoaderProbe(usb::content::LoaderProbeStage::HostPathInvalid, loc, path_result, metadata_storage);
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostPathInvalid, loc, path_result, metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id));
                return false;
            }
            constexpr const char UsbContentPrefix[] = "usbcontent:";
            constexpr const char StockSdContentPrefix[] = "@Sdcard:/Nintendo/Contents";
            constexpr const char NcmSdContentPrefix[] = "@SdCardContent:";
            const char *relative_content_path = nullptr;
            if (std::strncmp(out->str, UsbContentPrefix, sizeof(UsbContentPrefix) - 1) == 0) {
                /* Already in the private loader/FS form. */
            } else if (std::strncmp(out->str, StockSdContentPrefix, sizeof(StockSdContentPrefix) - 1) == 0) {
                /* Older HOS/NCM builds expose the content path beneath the
                 * SD mount as @Sdcard:/Nintendo/Contents/.... */
                relative_content_path = out->str + sizeof(StockSdContentPrefix) - 1;
            } else if (std::strncmp(out->str, NcmSdContentPrefix, sizeof(NcmSdContentPrefix) - 1) == 0) {
                /* Current HOS/NCM builds use the content-storage mount name
                 * instead. Host.GetPath() returns this form (often with two
                 * slashes), so accept it before rejecting an otherwise valid
                 * USB NCA path. */
                relative_content_path = out->str + sizeof(NcmSdContentPrefix) - 1;
            } else {
                RecordUsbLoaderProbe(usb::content::LoaderProbeStage::HostPathInvalid, loc, fs::ResultInvalidPath(), metadata_storage);
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostPathInvalid, loc, fs::ResultInvalidPath(), metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id), out->str);
                return false;
            }

            if (relative_content_path != nullptr) {
                while (*relative_content_path == '/') {
                    ++relative_content_path;
                }
                if (*relative_content_path == '\0') {
                    RecordUsbLoaderProbe(usb::content::LoaderProbeStage::HostPathInvalid, loc, fs::ResultInvalidPath(), metadata_storage);
                    RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostPathInvalid, loc, fs::ResultInvalidPath(), metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id), out->str);
                    return false;
                }

                /* NCM exposes a stock-looking path to Horizon. Translate the
                 * loader's private copy back to the USB filesystem mount. */
                char usb_path[sizeof(out->str)]{};
                const int usb_path_length = util::SNPrintf(usb_path, sizeof(usb_path), "%s/%s", UsbContentPrefix, relative_content_path);
                if (usb_path_length <= 0 || static_cast<size_t>(usb_path_length) >= sizeof(usb_path)) {
                    RecordUsbLoaderProbe(usb::content::LoaderProbeStage::HostPathInvalid, loc, fs::ResultInvalidPath(), metadata_storage);
                    RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostPathInvalid, loc, fs::ResultInvalidPath(), metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id), out->str);
                    return false;
                }
                std::memcpy(out->str, usb_path, sizeof(out->str));
            }

            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::ExternalCodeBindAttempt, loc, ResultSuccess(), metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id), out->str);
            const Result bind_result = BindUsbExternalCode(out->str, loc);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::ExternalCodeBindResult, loc, bind_result, metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id), out->str);
            if (R_FAILED(bind_result)) {
                if (out_usb_error != nullptr) *out_usb_error = bind_result;
                return false;
            }

            /* Repair/create the durable hint for installs made by older test
             * builds. Failure is non-fatal because the exact Host check above
             * has already proved this mapping for the current launch. */
            if constexpr (!usb::content::UseFileSystemContentRoute) {
                AMS_UNUSED(ncm::RegisterUsbTitle(loc.program_id));
            }

            RecordUsbLoaderProbe(usb::content::LoaderProbeStage::HostPathReady, loc, ResultSuccess(), metadata_storage);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::HostPathReady, loc, ResultSuccess(), metadata_storage, ncm::StorageId::Host, std::addressof(program_content_id), out->str);
            return true;
        }

    }

    void RecordUsbLoaderTraceForDiagnostics(usb::content::LoaderProbeStage stage, const ncm::ProgramLocation &loc, Result result) {
        if (!IsUsbUserProgramLocation(loc)) {
            return;
        }
        /* NS can reject a mapped title before it ever reaches Loader's USB
         * path resolver. Publish the boot-captured NS patch status from every
         * later user-program Loader request as well. This service-backed path
         * is unavailable during early boot by design and performs no SD I/O. */
        RecordNsPrePglPatchDiagnostic(loc);
        RecordUsbLoaderTrace(stage, loc, result);
    }

    namespace {
        /* Matches RoManager's bound on concurrently pinned programs. No
         * eviction of live bindings. Entries are released on UnpinProgram. */
        usb::content::ProcessContentBinding g_usb_process_bindings[0x40]{};
        os::SdkMutex g_usb_process_binding_mutex;
        Result SendProcessContentBinding(const usb::content::ProcessContentBinding &binding) {
            os::NativeHandle handle;
            R_TRY(sm::GetServiceHandle(std::addressof(handle), sm::ServiceName::Encode(usb::content::ServiceName)));
            Service service{};
            serviceCreate(std::addressof(service), handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };
            R_RETURN(serviceDispatchIn(std::addressof(service), usb::content::CommandId_SetProcessContent, binding));
        }
    }

    Result BindUsbProcessContent(PinId pin_id, ncm::ProgramId program_id, os::ProcessId process_id) {
        std::scoped_lock lock(g_usb_process_binding_mutex);
        for (auto &entry : g_usb_process_bindings) {
            if (entry.pin_id != 0 && entry.process_id == 0 && R_SUCCEEDED(SendProcessContentBinding(entry))) entry = {};
        }
        for (auto &entry : g_usb_process_bindings) {
            if (entry.pin_id == 0) {
                const usb::content::ProcessContentBinding binding{program_id.value, process_id.value, pin_id.value};
                R_TRY(SendProcessContentBinding(binding));
                entry = binding;
                R_SUCCEED();
            }
        }
        R_THROW(ldr::ResultMaxProcess());
    }

    void ReleaseUsbProcessContent(PinId pin_id) {
        std::scoped_lock lock(g_usb_process_binding_mutex);
        for (auto &entry : g_usb_process_bindings) {
            if (entry.pin_id != pin_id.value) continue;
            entry.process_id = 0;
            if (R_SUCCEEDED(SendProcessContentBinding(entry))) entry = {};
            return;
        }
    }

    /* ScopedCodeMount functionality. */
    ScopedCodeMount::ScopedCodeMount(const ncm::ProgramLocation &loc, const ldr::ProgramAttributes &attrs, os::SdkMutex &mutex, const char *ams, const char *sd_b, const char *b) : m_lk(mutex), m_ams_path(ams), m_sd_or_base_path(sd_b), m_base_path(b), m_has_status(false), m_mounted_ams(false), m_mounted_sd_or_code(false), m_mounted_code(false) {
        m_result = this->Initialize(loc, attrs);
    }

    ScopedCodeMount::ScopedCodeMount(const ncm::ProgramLocation &loc, const cfg::OverrideStatus &o, const ldr::ProgramAttributes &attrs, os::SdkMutex &mutex, const char *ams, const char *sd_b, const char *b) : m_lk(mutex), m_ams_path(ams), m_sd_or_base_path(sd_b), m_base_path(b), m_override_status(o), m_has_status(true), m_mounted_ams(false), m_mounted_sd_or_code(false), m_mounted_code(false) {
        m_result = this->Initialize(loc, attrs);
    }

    ScopedCodeMount::~ScopedCodeMount() {
        /* Unmount filesystems. */
        if (m_mounted_ams) {
            fs::Unmount(m_ams_path);
        }
        if (m_mounted_sd_or_code) {
            fs::Unmount(m_sd_or_base_path);
        }
        if (m_mounted_code) {
            fs::Unmount(m_base_path);
        }
    }

    Result ScopedCodeMount::Initialize(const ncm::ProgramLocation &loc, const ldr::ProgramAttributes &attrs) {
        /* Capture override status, if necessary. */
        this->EnsureOverrideStatus(loc);
        AMS_ABORT_UNLESS(m_has_status);

        /* Get the content path. */
        char content_path[fs::EntryNameLengthMax + 1];
        const bool trace_application = !ncm::IsSystemProgramId(loc.program_id);
        if (trace_application) {
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::ProgramPathResolveAttempt, loc);
        }
        const Result content_path_result = GetProgramPath(content_path, sizeof(content_path), loc, attrs);
        if (trace_application) {
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::ProgramPathResolveResult, loc, content_path_result, ncm::StorageId::None, static_cast<ncm::StorageId>(loc.storage_id), nullptr, R_SUCCEEDED(content_path_result) ? content_path : nullptr);
        }
        R_TRY(content_path_result);

        /* Get the content attributes. */
        const auto content_attributes = attrs.content_attributes;

        /* Mount the atmosphere code file system. A normal location resolver
         * may successfully return the deterministic SD path even though this
         * USB install has no NCA bytes there. Treat an actual stock code-mount
         * failure as the final, application-only opportunity to resolve the
         * same Program content ID from Host. */
        if (trace_application) {
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::StockCodeMountAttempt, loc, ResultSuccess(), ncm::StorageId::None, static_cast<ncm::StorageId>(loc.storage_id), nullptr, content_path);
        }
        const Result stock_mount_result = fs::MountCodeForAtmosphereWithRedirection(std::addressof(m_ams_code_verification_data), m_ams_path, content_path, content_attributes, loc.program_id, static_cast<ncm::StorageId>(loc.storage_id), m_override_status.IsHbl(), m_override_status.IsProgramSpecific());
        if (trace_application) {
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::StockCodeMountResult, loc, stock_mount_result, ncm::StorageId::None, static_cast<ncm::StorageId>(loc.storage_id), nullptr, content_path);
        }
        if (R_FAILED(stock_mount_result)) {
            RecordUsbLoaderProbe(usb::content::LoaderProbeStage::StockCodeMountFailed, loc, stock_mount_result);
            if (trace_application) {
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::StockCodeMountFailed, loc, stock_mount_result, ncm::StorageId::None, static_cast<ncm::StorageId>(loc.storage_id), nullptr, content_path);
            }
            lr::Path usb_path{};
            R_UNLESS(TryResolveUsbProgramPath(std::addressof(usb_path), loc), stock_mount_result);

            std::memset(content_path, 0, sizeof(content_path));
            std::memcpy(content_path, usb_path.str, std::min(sizeof(content_path), sizeof(usb_path)));
            RecordUsbLoaderProbe(usb::content::LoaderProbeStage::UsbCodeMountRetry, loc);
            if (trace_application) {
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbCodeMountAttempt, loc, ResultSuccess(), ncm::StorageId::None, ncm::StorageId::Host, nullptr, content_path);
            }
            const Result usb_mount_result = fs::MountCodeForAtmosphereWithRedirection(std::addressof(m_ams_code_verification_data), m_ams_path, content_path, content_attributes, loc.program_id, static_cast<ncm::StorageId>(loc.storage_id), m_override_status.IsHbl(), m_override_status.IsProgramSpecific());
            if (trace_application) {
                RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbCodeMountResult, loc, usb_mount_result, ncm::StorageId::None, ncm::StorageId::Host, nullptr, content_path);
            }
            R_TRY(usb_mount_result);
        }
        m_mounted_ams = true;

        const bool use_bound_usb_code = IsUsbContentPath(content_path) && fssystem::GetExternalCodeFileSystem(loc.program_id) != nullptr;
        m_uses_usb_code = use_bound_usb_code;
        if (use_bound_usb_code) {
            /* Loader needs all three names while reading and validating
             * main.npdm. Reuse the one already-open USB ExeFS for sd-code and
             * code so a redundant command-17 open cannot fail the launch. */
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbBoundSdMountAttempt, loc, ResultSuccess(), ncm::StorageId::None, ncm::StorageId::Host, nullptr, content_path);
            const Result sd_mount_result = MountBoundUsbExternalCode(std::addressof(m_sd_or_base_code_verification_data), m_sd_or_base_path, loc.program_id);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbBoundSdMountResult, loc, sd_mount_result, ncm::StorageId::None, ncm::StorageId::Host, nullptr, content_path);
            R_TRY(sd_mount_result);
            m_mounted_sd_or_code = true;

            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbBoundBaseMountAttempt, loc, ResultSuccess(), ncm::StorageId::None, ncm::StorageId::Host, nullptr, content_path);
            const Result base_mount_result = MountBoundUsbExternalCode(std::addressof(m_base_code_verification_data), m_base_path, loc.program_id);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::UsbBoundBaseMountResult, loc, base_mount_result, ncm::StorageId::None, ncm::StorageId::Host, nullptr, content_path);
            R_TRY(base_mount_result);
            m_mounted_code = true;
        } else {
            /* Preserve Atmosphere's stock SD/NAND behavior for every title
             * that does not have an exact, already-bound USB mapping. */
            R_TRY(fs::MountCodeForAtmosphere(std::addressof(m_sd_or_base_code_verification_data), m_sd_or_base_path, content_path, content_attributes, loc.program_id, static_cast<ncm::StorageId>(loc.storage_id)));
            m_mounted_sd_or_code = true;

            if (R_SUCCEEDED(fs::MountCode(std::addressof(m_base_code_verification_data), m_base_path, content_path, content_attributes, loc.program_id, static_cast<ncm::StorageId>(loc.storage_id)))) {
                m_mounted_code = true;
            }
        }

        R_SUCCEED();
    }

    void ScopedCodeMount::EnsureOverrideStatus(const ncm::ProgramLocation &loc) {
        if (m_has_status) {
            return;
        }
        m_override_status = cfg::CaptureOverrideStatus(loc.program_id);
        m_has_status = true;
    }

    /* Redirection API. */
    Result GetProgramPath(char *out_path, size_t out_size, const ncm::ProgramLocation &loc, const ldr::ProgramAttributes &attrs) {
        /* Get the content attributes. */
        const auto content_attributes = attrs.content_attributes;
        AMS_UNUSED(content_attributes);

        lr::Path path;

        /* Check that path registration is allowable. */
        /* A USB install deliberately leaves its metadata and application
         * record on the stock storage while its NCA bytes live in Host. A
         * registered SD path can still resolve successfully even when the
         * corresponding NCA is absent from SD, so do the exact Host lookup
         * first for user applications. Fall back to the stock resolver when
         * the USB backend does not contain this title. */
        bool resolved_usb_path = false;
        const auto storage_id = static_cast<ncm::StorageId>(loc.storage_id);
        if (IsUsbUserProgramLocation(loc)) {
            Result usb_error = ResultSuccess();
            resolved_usb_path = TryResolveUsbProgramPath(std::addressof(path), loc, std::addressof(usb_error));
            R_TRY(usb_error);
        }

        /* Preserve the stock Host/None rules when no exact USB title was
         * found.  An exact USB match is allowed through because the route is
         * explicitly armed and the path is served by the USB code filesystem. */
        if (storage_id == ncm::StorageId::Host) {
            AMS_ABORT_UNLESS(spl::IsDevelopment() || resolved_usb_path);
        }

        if (storage_id == ncm::StorageId::None && !resolved_usb_path) {
            std::memset(out_path, 0, out_size);
            std::memcpy(out_path, "/", std::min<size_t>(out_size, 2));
            R_SUCCEED();
        }

        if (!resolved_usb_path) {
            /* Try to get the path from the registered resolver. */
            lr::RegisteredLocationResolver reg;
            R_TRY(lr::OpenRegisteredLocationResolver(std::addressof(reg)));

            R_TRY_CATCH(reg.ResolveProgramPath(std::addressof(path), loc.program_id)) {
                R_CATCH(lr::ResultProgramNotFound) {
                    /* Program wasn't found via registered resolver, fall back to the normal resolver. */
                    lr::LocationResolver lr;
                    R_TRY(lr::OpenLocationResolver(std::addressof(lr), storage_id));
                    const Result resolve_result = lr.ResolveProgramPath(std::addressof(path), loc.program_id);
                    if (R_FAILED(resolve_result)) {
                        /* Preserve the stock error when the exact USB lookup
                         * was already attempted above and did not find the
                         * title. */
                        R_RETURN(resolve_result);
                    }
                }
            } R_END_TRY_CATCH;
        }

        /* LR may already know the persistent USB path even when Loader's NCM
         * lookup could not complete. Bind that exact path here so all three
         * code mounts share one remote filesystem and one verification block. */
        if (!resolved_usb_path && IsUsbContentPath(path.str)) {
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::ExternalCodeBindAttempt, loc, ResultSuccess(), ncm::StorageId::None, ncm::StorageId::Host, nullptr, path.str);
            const Result bind_result = BindUsbExternalCode(path.str, loc);
            RecordUsbLoaderTrace(usb::content::LoaderProbeStage::ExternalCodeBindResult, loc, bind_result, ncm::StorageId::None, ncm::StorageId::Host, nullptr, path.str);
            R_TRY(bind_result);
            resolved_usb_path = true;
        }

        /* Fix directory separators in path. */
        fs::Replace(path.str, sizeof(path.str), fs::StringTraits::AlternateDirectorySeparator, fs::StringTraits::DirectorySeparator);

        /* Check that the path is valid. */
        AMS_ABORT_UNLESS(path.IsValid());

        /* Copy the output path. */
        std::memset(out_path, 0, out_size);
        std::memcpy(out_path, path.str, std::min(out_size, sizeof(path)));

        R_SUCCEED();
    }

    void ClearUsbExternalCodeVerification(ncm::ProgramId program_id) {
        std::scoped_lock lk(g_usb_external_code_verification_mutex);
        g_usb_external_code_verification.Remove(program_id);
    }

    Result RedirectProgramPath(const char *path, size_t size, const ncm::ProgramLocation &loc) {
        /* Check for storage id none. */
        if (static_cast<ncm::StorageId>(loc.storage_id) == ncm::StorageId::None) {
            R_SUCCEED();
        }

        /* Open location resolver. */
        lr::LocationResolver lr;
        R_TRY(lr::OpenLocationResolver(std::addressof(lr),  static_cast<ncm::StorageId>(loc.storage_id)));

        /* Copy in path. */
        lr::Path lr_path;
        std::memcpy(lr_path.str, path, std::min(size, sizeof(lr_path.str)));
        lr_path.str[sizeof(lr_path.str) - 1] = '\x00';

        /* Redirect the path. */
        lr.RedirectProgramPath(lr_path, loc.program_id);

        R_SUCCEED();
    }

    Result RedirectHtmlDocumentPathForHbl(const ncm::ProgramLocation &loc) {
        lr::Path path;

        /* Open a location resolver. */
        lr::LocationResolver lr;
        R_TRY(lr::OpenLocationResolver(std::addressof(lr),  static_cast<ncm::StorageId>(loc.storage_id)));

        /* If there's already a Html Document path, we don't need to set one. */
        R_SUCCEED_IF(R_SUCCEEDED(lr.ResolveApplicationHtmlDocumentPath(std::addressof(path), loc.program_id)));

        /* We just need to set this to any valid NCA path. Let's use the executable path. */
        R_TRY(lr.ResolveProgramPath(std::addressof(path), loc.program_id));
        lr.RedirectApplicationHtmlDocumentPath(path, loc.program_id, loc.program_id);

        R_SUCCEED();
    }

}
