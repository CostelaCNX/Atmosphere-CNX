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
#include "../amsmitm_fs_utils.hpp"
#include "../amsmitm_initialization.hpp"
#include "fs_shim.h"
#include "fs_mitm_service.hpp"
#include "fsmitm_external_key.hpp"
#include "fsmitm_boot0storage.hpp"
#include "fsmitm_calibration_binary_storage.hpp"
#include "fsmitm_layered_romfs_storage.hpp"
#include "fsmitm_save_utils.hpp"
#include "fsmitm_readonly_layered_filesystem.hpp"
#include "fsmitm_domain_filesystem.hpp"
#include "../usb_mitm/usbmitm_module.hpp"
#include "../usb_mitm/usb_virtual_sd.hpp"
#include "../usb_mitm/usb_startup_timing.hpp"
#include "../usb_mitm/usb_romfs_mods.hpp"
#include "../usb_mitm/usb_launch_trace.hpp"
#include "../usb_mitm/usb_verification_path.hpp"
#include "../ns_mitm/ns_verification_trace.hpp"

namespace ams::mitm::fs {

    using namespace ams::fs;

    namespace {

        constexpr const ams::fs::Path AtmosphereHblWebContentDirPath = fs::MakeConstantPath("/atmosphere/hbl_html/");
        constexpr const char ProgramWebContentDir[] = "/manual_html/";
        constexpr const char UsbContentPreflightControlProbePrefix[] = "@Sdcard:/Nintendo/Contents/registered/FFFFFFFF/";

        bool IsUsbContentPreflightControlProbe(const char *path) {
            return path != nullptr
                && std::strncmp(path, UsbContentPreflightControlProbePrefix,
                    sizeof(UsbContentPreflightControlProbePrefix) - 1) == 0;
        }

        bool IsUsbContentByteRouteReady() {
            if constexpr (ams::usb::content::UseFileSystemContentRoute) {
                u64 generation = 0;
                return usb_storage::IsUsbContentFileSystemRouteReady(
                    std::addressof(generation), false);
            }
            return usb_storage::IsUsbContentLaunchPathEnabled();
        }

        constexpr size_t NativeProgramIdentityCacheCapacity = 8;

        struct NativeProgramIdentityCacheEntry {
            u64 program_id;
            u32 attributes;
            bool valid;
            char path[ams::fs::EntryNameLengthMax + 1];
        };

        constinit os::SdkMutex g_native_program_identity_cache_lock;
        constinit NativeProgramIdentityCacheEntry g_native_program_identity_cache[NativeProgramIdentityCacheCapacity]{};
        constinit size_t g_next_native_program_identity_cache_entry = 0;

        void RememberNativeProgramIdentity(const char *path, ams::fs::ContentAttributes attributes,
            ncm::ProgramId program_id) {
            std::scoped_lock lk(g_native_program_identity_cache_lock);

            size_t index = NativeProgramIdentityCacheCapacity;
            for (size_t i = 0; i < NativeProgramIdentityCacheCapacity; ++i) {
                const auto &entry = g_native_program_identity_cache[i];
                if (entry.valid && entry.program_id == program_id.value
                    && entry.attributes == static_cast<u32>(attributes)
                    && std::strcmp(entry.path, path) == 0) {
                    index = i;
                    break;
                }
            }
            if (index == NativeProgramIdentityCacheCapacity) {
                index = g_next_native_program_identity_cache_entry;
                g_next_native_program_identity_cache_entry =
                    (g_next_native_program_identity_cache_entry + 1) % NativeProgramIdentityCacheCapacity;
            }

            auto &entry = g_native_program_identity_cache[index];
            entry.valid = false;
            entry.program_id = program_id.value;
            entry.attributes = static_cast<u32>(attributes);
            util::Strlcpy(entry.path, path, sizeof(entry.path));
            entry.valid = true;
        }

        bool HasRememberedNativeProgramIdentity(const char *path, ams::fs::ContentAttributes attributes,
            ncm::ProgramId program_id) {
            std::scoped_lock lk(g_native_program_identity_cache_lock);
            for (const auto &entry : g_native_program_identity_cache) {
                if (entry.valid && entry.program_id == program_id.value
                    && entry.attributes == static_cast<u32>(attributes)
                    && std::strcmp(entry.path, path) == 0) {
                    return true;
                }
            }
            return false;
        }

        constinit os::SdkMutex g_boot0_detect_lock;
        constinit bool g_detected_boot0_kind = false;
        constinit bool g_is_boot0_custom_public_key = false;

        constinit fssrv::impl::ProgramIndexMapInfoManager g_program_index_map_info_manager;

        bool IsBoot0CustomPublicKey(::FsStorage &storage) {
            if (AMS_UNLIKELY(!g_detected_boot0_kind)) {
                std::scoped_lock lk(g_boot0_detect_lock);

                if (AMS_LIKELY(!g_detected_boot0_kind)) {
                    g_is_boot0_custom_public_key = DetectBoot0CustomPublicKey(storage);
                    g_detected_boot0_kind = true;
                }
            }

            return g_is_boot0_custom_public_key;
        }

        bool GetSettingsItemBooleanValue(const char *name, const char *key) {
            u8 tmp = 0;
            AMS_ABORT_UNLESS(settings::fwdbg::GetSettingsItemValue(std::addressof(tmp), sizeof(tmp), name, key) == sizeof(tmp));
            return (tmp != 0);
        }

        template<typename... Arguments>
        constexpr ALWAYS_INLINE auto MakeSharedFileSystem(Arguments &&... args) {
            return sf::CreateSharedObjectEmplaced<ams::fssrv::sf::IFileSystem, ams::fssrv::impl::FileSystemInterfaceAdapter>(std::forward<Arguments>(args)...);
        }

        template<typename... Arguments>
        constexpr ALWAYS_INLINE auto MakeSharedStorage(Arguments &&... args) {
            return sf::CreateSharedObjectEmplaced<ams::fssrv::sf::IStorage, ams::fssrv::impl::StorageInterfaceAdapter>(std::forward<Arguments>(args)...);
        }
        
        template<typename... Arguments>
        constexpr ALWAYS_INLINE auto MakeSharedStorageForBatchRead(Arguments &&... args) {
            return sf::CreateSharedObjectEmplaced<ams::fssrv::sf::IStorageForBatchRead, ams::fssrv::impl::StorageInterfaceAdapterForBatchRead>(std::forward<Arguments>(args)...);
        }

        Result PublishUsbStorageForMitm(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorage>> &out,
            std::shared_ptr<ams::fs::IStorage> storage, std::shared_ptr<::Service> forward,
            ncm::ProgramId program_id, u32 command, usb_storage::LaunchPin pin = {}) {
            sf::cmif::DomainObjectId object_id{};
            const Result result = [&]() -> Result {
                std::shared_ptr<ams::fs::IStorage> reserved;
                R_TRY(WrapUsbStorageForMitm(std::addressof(reserved), std::addressof(object_id),
                    std::move(storage), std::move(forward), program_id, std::move(pin)));
                auto object = MakeSharedStorage(std::move(reserved));
                R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
                out.SetValue(std::move(object), object_id);
                R_SUCCEED();
            }();
            char detail[128]{};
            util::SNPrintf(detail, sizeof(detail), "R157 storage handoff: cmd=%u id=%u", command, object_id.value);
            /* This is the complete handler result, not just a successful NCA
             * reader creation. Actual reply delivery is traced by fs.mitm. */
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsUsbStorageHandoff,
                program_id, 0, result, detail);
            R_RETURN(result);
        }

        Result OpenHblWebContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> &out, ncm::ProgramId program_id) {
            /* Verify eligibility. */
            bool is_hbl;
            R_UNLESS(R_SUCCEEDED(ams::pm::info::IsHblProgramId(std::addressof(is_hbl), program_id)), sm::mitm::ResultShouldForwardToSession());
            R_UNLESS(is_hbl,                                                     sm::mitm::ResultShouldForwardToSession());

            /* Hbl html directory must exist. */
            {
                FsDir d;
                R_UNLESS(R_SUCCEEDED(mitm::fs::OpenSdDirectory(std::addressof(d), AtmosphereHblWebContentDirPath.GetString(), fs::OpenDirectoryMode_Directory)), sm::mitm::ResultShouldForwardToSession());
                fsDirClose(std::addressof(d));
            }

            /* Open the SD card using fs.mitm's session. */
            FsFileSystem sd_fs;
            R_TRY(fsOpenSdCardFileSystem(std::addressof(sd_fs)));
            const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(sd_fs.s))};
            std::unique_ptr<fs::fsa::IFileSystem> sd_ifs = std::make_unique<fs::RemoteFileSystem>(sd_fs);

            auto subdir_fs = std::make_unique<fssystem::SubDirectoryFileSystem>(std::move(sd_ifs));
            R_TRY(subdir_fs->Initialize(AtmosphereHblWebContentDirPath));

            out.SetValue(MakeSharedFileSystem(std::make_shared<fs::ReadOnlyFileSystem>(std::move(subdir_fs)), false), target_object_id);
            R_SUCCEED();
        }

        Result OpenProgramSpecificWebContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> &out, ncm::ProgramId program_id, FsFileSystemType filesystem_type, Service *fwd, const fssrv::sf::Path *path, bool with_id, bool with_attributes) {
            /* Directory must exist. */
            R_UNLESS(HasSdManualHtmlContent(program_id), sm::mitm::ResultShouldForwardToSession());

            /* Open the SD card using fs.mitm's session. */
            FsFileSystem sd_fs;
            R_TRY(fsOpenSdCardFileSystem(std::addressof(sd_fs)));
            const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(sd_fs.s))};
            std::unique_ptr<fs::fsa::IFileSystem> sd_ifs = std::make_unique<fs::RemoteFileSystem>(sd_fs);

            /* Format the subdirectory path. */
            char program_web_content_raw_path[0x100];
            FormatAtmosphereSdPath(program_web_content_raw_path, sizeof(program_web_content_raw_path), program_id, ProgramWebContentDir);

            ams::fs::Path program_web_content_path;
            R_TRY(program_web_content_path.SetShallowBuffer(program_web_content_raw_path));

            /* Make a new filesystem. */
            {
                auto subdir_fs = std::make_unique<fssystem::SubDirectoryFileSystem>(std::move(sd_ifs));
                R_TRY(subdir_fs->Initialize(program_web_content_path));

                std::shared_ptr<fs::fsa::IFileSystem> new_fs = nullptr;

                /* Try to open the existing fs. */
                FsFileSystem base_fs;
                bool opened_base_fs = false;
                if (with_attributes) {
                    opened_base_fs = R_SUCCEEDED(fsOpenFileSystemWithIdFwd(fwd, std::addressof(base_fs), path->str, FsContentAttributes_None, static_cast<u64>(program_id), filesystem_type));
                } else if (with_id) {
                    opened_base_fs = R_SUCCEEDED(fsOpenFileSystemWithIdObsoleteFwd(fwd, std::addressof(base_fs), path->str, static_cast<u64>(program_id), filesystem_type));
                } else {
                    opened_base_fs = R_SUCCEEDED(fsOpenFileSystemWithPatchFwd(fwd, std::addressof(base_fs), static_cast<u64>(program_id), filesystem_type));
                }

                if (opened_base_fs) {
                    /* Create a layered adapter. */
                    new_fs = std::make_shared<ReadOnlyLayeredFileSystem>(std::move(subdir_fs), std::make_unique<fs::RemoteFileSystem>(base_fs));
                } else {
                    /* Without an existing FS, just make a read only adapter to the subdirectory. */
                    new_fs = std::make_shared<fs::ReadOnlyFileSystem>(std::move(subdir_fs));
                }

                out.SetValue(MakeSharedFileSystem(std::move(new_fs), false), target_object_id);
            }

            R_SUCCEED();
        }

        Result OpenWebContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> &out, ncm::ProgramId client_program_id, ncm::ProgramId program_id, FsFileSystemType filesystem_type, Service *fwd, const fssrv::sf::Path *path, bool with_id, bool with_attributes, bool try_program_specific) {
            /* Check first that we're a web applet opening web content. */
            R_UNLESS(ncm::IsWebAppletId(client_program_id),             sm::mitm::ResultShouldForwardToSession());
            R_UNLESS(filesystem_type == FsFileSystemType_ContentManual, sm::mitm::ResultShouldForwardToSession());

            /* Try to mount the HBL web filesystem. If this succeeds then we're done. */
            R_SUCCEED_IF(R_SUCCEEDED(OpenHblWebContentFileSystem(out, program_id)));

            /* If program specific override shouldn't be attempted, fall back. */
            R_UNLESS(try_program_specific, sm::mitm::ResultShouldForwardToSession());

            /* If we're not opening a HBL filesystem, just try to open a generic one. */
            R_RETURN(OpenProgramSpecificWebContentFileSystem(out, program_id, filesystem_type, fwd, path, with_id, with_attributes));
        }

    }

    bool HasSdManualHtmlContent(ncm::ProgramId program_id) {
        /* Directory must exist. */
        FsDir d;
        if (R_SUCCEEDED(OpenAtmosphereSdDirectory(std::addressof(d), program_id, ProgramWebContentDir, fs::OpenDirectoryMode_Directory))) {
            ::fsDirClose(std::addressof(d));
            return true;
        } else {
            return false;
        }
    }

    Result FsMitmService::OpenFileSystemWithPatch(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, ncm::ProgramId program_id, u32 file_system_proxy_type) {
        R_RETURN(OpenWebContentFileSystem(out, m_client_info.program_id, program_id, static_cast<FsFileSystemType>(file_system_proxy_type), m_forward_service.get(), nullptr, false, false, m_client_info.override_status.IsProgramSpecific()));
    }

    Result FsMitmService::OpenFileSystemWithIdObsolete(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, const fssrv::sf::Path &path, u64 program_id, u32 _filesystem_type) {
        if (ncm::IsWebAppletId(m_client_info.program_id)
                && _filesystem_type == FsFileSystemType_ContentManual) {
            R_RETURN(OpenWebContentFileSystem(out, m_client_info.program_id, ncm::ProgramId{program_id},
                static_cast<FsFileSystemType>(_filesystem_type), m_forward_service.get(),
                std::addressof(path), true, false, m_client_info.override_status.IsProgramSpecific()));
        }
        R_RETURN(this->OpenFileSystemWithId(out, path, ams::fs::ContentAttributes_None, program_id, _filesystem_type));
    }

    Result FsMitmService::OpenFileSystemWithId(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, const fssrv::sf::Path &path, ams::fs::ContentAttributes attributes, u64 raw_program_id, u32 _filesystem_type) {
        const ncm::ProgramId program_id{raw_program_id};
        if (ncm::IsWebAppletId(m_client_info.program_id)
                && _filesystem_type == FsFileSystemType_ContentManual) {
            R_RETURN(OpenWebContentFileSystem(out, m_client_info.program_id, program_id,
                static_cast<FsFileSystemType>(_filesystem_type), m_forward_service.get(),
                std::addressof(path), true, true, m_client_info.override_status.IsProgramSpecific()));
        }
        const auto filesystem_type = static_cast<FsFileSystemType>(_filesystem_type);

        u64 verify_title = 0, verify_token = 0;
        bool verify_trace = false;
        if (m_client_info.program_id == ncm::SystemProgramId::Ns
            && ams::mitm::ns::GetVerificationOpenContext(verify_title, verify_token, verify_trace)) {
            /* P7: native verification opens content filesystems without a
             * launch-cache entry. Preserve cmd10's attributes and ID, and
             * query stock exactly once before considering a USB archive. */
            FsFileSystem native{};
            const Result stock = fsOpenFileSystemWithIdAndAttributesFwd(m_forward_service.get(),
                &native, raw_program_id, filesystem_type, path.str, static_cast<u8>(attributes));
            const ncm::ProgramId title{verify_title};
            auto record = [&](u32 stage, Result rc, const char *text) {
                if (verify_trace) usb_storage::RecordLaunchBoundaryTrace(
                    static_cast<usb::content::LoaderProbeStage>(stage), title, 0, rc, text);
            };
            char detail[0x80]{};
            util::SNPrintf(detail, sizeof(detail), "P8 open session=%llu id=%016llX type=%u attr=%u native=%08X",
                static_cast<unsigned long long>(verify_token), static_cast<unsigned long long>(raw_program_id),
                _filesystem_type, static_cast<u32>(attributes), stock.GetValue());
            record(230, stock, detail);
            for (size_t i = 0; i < sizeof(detail)-1; ++i) {
                const unsigned char ch = path.str[i];
                detail[i] = ch == 0 ? 0 : (ch >= 32 && ch < 127 ? ch : '?');
                if (!ch) break;
            }
            detail[sizeof(detail)-1] = 0;
            record(231, stock, detail);
            if (R_SUCCEEDED(stock)) {
                const sf::cmif::DomainObjectId id{serviceGetObjectId(&native.s)};
                auto remote = std::make_shared<fs::RemoteFileSystem>(native);
                auto object = MakeSharedFileSystem(std::move(remote), false);
                R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
                out.SetValue(std::move(object), id);
                R_SUCCEED();
            }
            u64 generation = 0;
            const bool ready = usb::content::UseFileSystemContentRoute
                && usb_storage::IsUsbContentFileSystemRouteReady(&generation, false);
            R_UNLESS(usb_storage::verification::ShouldFallback(stock.GetValue(), static_cast<u8>(attributes), ready), stock);
            R_UNLESS(_filesystem_type >= 2 && _filesystem_type <= 6, stock);
            const Result routed = [&]() -> Result {
                std::shared_ptr<ams::fs::fsa::IFileSystem> base, domain;
                R_TRY(usb_storage::OpenUsbVerificationFileSystem(&base, path.str, sizeof(path.str), program_id, title, _filesystem_type));
                sf::cmif::DomainObjectId id{};
                R_TRY(WrapUsbProgramFileSystemForMitm(&domain, &id, std::move(base), m_forward_service, title));
                auto object = MakeSharedFileSystem(std::move(domain), false, true);
                R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
                out.SetValue(std::move(object), id);
                R_SUCCEED();
            }();
            record(232, routed, "P8 USB filesystem mount result; native verifier decides final outcome");
            R_RETURN(routed); // Never turn an archive/read/integrity failure into success.
        }

        /* P9 control route: native first, independent of Program-path caches.
         * HOME may ask for Control before any launch, and a bounded Program
         * cache is not evidence that a registered Control archive is absent. */
        if ((m_client_info.program_id == ncm::SystemProgramId::Am
                || m_client_info.program_id == ncm::SystemProgramId::Ns
                || m_client_info.program_id == ncm::SystemAppletId::Qlaunch)
            && usb::content::UseFileSystemContentRoute
            && filesystem_type == FsFileSystemType_ContentControl
            && attributes == ams::fs::ContentAttributes_None
            && ncm::IsApplicationId(program_id) && !ncm::IsSystemProgramId(program_id)) {
            char canonical[ams::fs::EntryNameLengthMax + 1]{};
            R_UNLESS(usb_storage::verification::Translate(canonical, sizeof(canonical), path.str, sizeof(path.str)),
                sm::mitm::ResultShouldForwardToSession());
            FsFileSystem native{};
            const Result stock = fsOpenFileSystemWithIdAndAttributesFwd(m_forward_service.get(),
                &native, raw_program_id, filesystem_type, path.str, static_cast<u8>(attributes));
            auto record = [&](u32 stage, Result rc, const char *text) {
                usb_storage::RecordLaunchBoundaryTrace(static_cast<usb::content::LoaderProbeStage>(stage),
                    program_id, static_cast<u8>(ncm::ContentType::Control), rc, text);
            };
            record(240, stock, "P9 Control native result; cache-independent");
            if (R_SUCCEEDED(stock)) {
                const sf::cmif::DomainObjectId id{serviceGetObjectId(&native.s)};
                auto remote = std::make_shared<fs::RemoteFileSystem>(native);
                auto object = MakeSharedFileSystem(std::move(remote), false);
                R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
                out.SetValue(std::move(object), id);
                R_SUCCEED();
            }
            u64 generation = 0;
            const bool ready = usb_storage::IsUsbContentFileSystemRouteReady(&generation, false);
            R_UNLESS(usb_storage::verification::ShouldFallback(stock.GetValue(), static_cast<u8>(attributes), ready), stock);
            record(241, stock, canonical);
            const Result routed = [&]() -> Result {
                std::shared_ptr<ams::fs::fsa::IFileSystem> base, domain;
                ncm::ProgramId resolved{};
                R_TRY(usb_storage::OpenUsbControlFileSystemByPath(&base, &resolved, path.str, program_id));
                sf::cmif::DomainObjectId id{};
                R_TRY(WrapUsbControlFileSystemForMitm(&domain, &id, std::move(base), m_forward_service, resolved));
                auto object = MakeSharedFileSystem(std::move(domain), false, true);
                R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
                out.SetValue(std::move(object), id);
                R_SUCCEED();
            }();
            record(242, routed, "P9 Control USB mount/publish result");
            /* Never forward a second time or turn an integrity/I/O failure
             * into success. The original archive supplies NACP and icons. */
            R_RETURN(routed);
        }
        /* P9 control route end. */

        if ((m_client_info.program_id == ncm::SystemProgramId::Am
                || m_client_info.program_id == ncm::SystemProgramId::Ns
                || m_client_info.program_id == ncm::SystemAppletId::Qlaunch)
                && usb_storage::HasCachedUsbProgramPath(program_id)) {
            char detail[0x80]{};
            util::SNPrintf(detail, sizeof(detail), "client=%016llX type=%u path=%.64s",
                static_cast<unsigned long long>(m_client_info.program_id.value),
                static_cast<unsigned int>(_filesystem_type), path.str);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsAmOpenFileSystemWithId,
                program_id, static_cast<u8>(_filesystem_type), ResultSuccess(), detail);
        }

        /* Qlaunch asks FS to mount a title's Control NCA before entering PGL.
         * GetProgramId has already asked stock FS to identify this path. Reuse
         * that exact successful identity here: issuing another stock command
         * and then forwarding the original request changes the ordering of the
         * client's domain IPC. Only use USB when no native identity was seen. */
        u64 usb_route_generation = 0;
        const bool filesystem_route_ready = ams::usb::content::UseFileSystemContentRoute
            && usb_storage::IsUsbContentFileSystemRouteReady(
                std::addressof(usb_route_generation), false);
        AMS_UNUSED(usb_route_generation);
        if (filesystem_type == FsFileSystemType_ContentControl
            && (filesystem_route_ready || usb_storage::IsUsbContentLaunchPathEnabled())
            && usb_storage::HasCachedUsbProgramPath(program_id)
            && !IsUsbContentPreflightControlProbe(path.str)) {
            const bool native_exact = HasRememberedNativeProgramIdentity(path.str, attributes, program_id);
            Result native_identity_result = ResultSuccess();
            if (!native_exact) {
                native_identity_result = ams::fs::ResultPathNotFound();
            }
            char native_detail[0x80]{};
            util::SNPrintf(native_detail, sizeof(native_detail),
                "native-control cache=%u forward=%u path=%.64s",
                native_exact ? 1u : 0u,
                native_exact ? 1u : 0u, path.str);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsNativeControlForward,
                program_id, static_cast<u8>(ncm::ContentType::Control),
                native_identity_result, native_detail);
            if (native_exact) {
                R_RETURN(sm::mitm::ResultShouldForwardToSession());
            }

            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsControlMountAttempt, program_id, static_cast<u8>(ncm::ContentType::Control), ResultSuccess(), path.str, static_cast<u8>(ncm::StorageId::None), static_cast<u8>(ncm::StorageId::Host));

            std::shared_ptr<ams::fs::fsa::IFileSystem> usb_control_fs;
            ncm::ProgramId resolved_program_id = program_id;
            const Result usb_result = filesystem_route_ready
                ? usb_storage::OpenUsbControlFileSystemByPath(
                    std::addressof(usb_control_fs), std::addressof(resolved_program_id),
                    path.str, program_id)
                : usb_storage::OpenUsbControlFileSystem(
                    std::addressof(usb_control_fs), std::addressof(resolved_program_id),
                    path.str, program_id);
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsControlMountResult, resolved_program_id, static_cast<u8>(ncm::ContentType::Control), usb_result, path.str, static_cast<u8>(ncm::StorageId::None), static_cast<u8>(ncm::StorageId::Host));
            if (R_SUCCEEDED(usb_result)) {
                std::shared_ptr<ams::fs::fsa::IFileSystem> domain_fs;
                sf::cmif::DomainObjectId object_id{};
                const Result publish_result = [&]() -> Result {
                    R_TRY(WrapUsbControlFileSystemForMitm(std::addressof(domain_fs), std::addressof(object_id),
                        std::move(usb_control_fs), m_forward_service, resolved_program_id));
                    /* Children must preserve their native reservations too. */
                    auto object = MakeSharedFileSystem(std::move(domain_fs), false, true);
                    R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
                    out.SetValue(std::move(object), object_id);
                    R_SUCCEED();
                }();
                char publish_path[96]{};
                util::SNPrintf(publish_path, sizeof(publish_path), "Control object publish: native_domain_id=%u", object_id.value);
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlObjectPublish, resolved_program_id,
                    static_cast<u8>(ncm::ContentType::Control), publish_result, publish_path);
                R_TRY(publish_result);
                R_SUCCEED();
            }

            /* A USB miss is non-authoritative. Let Horizon process the original
             * request unchanged instead of returning a synthetic USB error. */
            R_RETURN(sm::mitm::ResultShouldForwardToSession());
        }

        R_RETURN(OpenWebContentFileSystem(out, m_client_info.program_id, program_id, filesystem_type, m_forward_service.get(), std::addressof(path), true, true, m_client_info.override_status.IsProgramSpecific()));
    }

    Result FsMitmService::OpenDataFileSystemByProgramId(
        sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, ncm::ProgramId program_id) {
        /* AM uses this path before PGL/Loader process creation. Preserve the
         * native answer whenever it exists; a mapped USB Program NCA is used
         * only for the exact stock PathNotFound observed on external titles. */
        FsFileSystem stock_file_system{};
        const Result stock_result = fsOpenDataFileSystemByProgramIdFwd(m_forward_service.get(),
            std::addressof(stock_file_system), program_id.value);
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsProgramFileSystemStock, program_id,
            static_cast<u8>(ncm::ContentType::Program), stock_result,
            m_client_info.program_id == ncm::SystemProgramId::Am ? "client=am" : nullptr);
        if (R_SUCCEEDED(stock_result)) {
            const sf::cmif::DomainObjectId object_id{serviceGetObjectId(std::addressof(stock_file_system.s))};
            auto stock_fs = std::make_shared<fs::RemoteFileSystem>(stock_file_system);
            out.SetValue(MakeSharedFileSystem(std::move(stock_fs), false), object_id);
            R_SUCCEED();
        }

        R_UNLESS(fs::ResultPathNotFound::Includes(stock_result), stock_result);
        R_UNLESS(IsUsbContentByteRouteReady(), stock_result);
        R_UNLESS(!ncm::IsSystemProgramId(program_id), stock_result);
        R_UNLESS(usb_storage::HasCachedUsbProgramPath(program_id), stock_result);

        const Result usb_result = [&]() -> Result {
            std::shared_ptr<ams::fs::IStorage> storage;
            R_TRY(usb_storage::OpenUsbProgramRomFs(std::addressof(storage), program_id));

            std::shared_ptr<ams::fs::fsa::IFileSystem> program_fs;
            R_TRY(fssystem::GetFileSystemCreatorInterfaces()->rom_fs_creator->Create(
                std::addressof(program_fs), std::move(storage)));

            std::shared_ptr<ams::fs::fsa::IFileSystem> domain_fs;
            sf::cmif::DomainObjectId object_id{};
            R_TRY(WrapUsbProgramFileSystemForMitm(std::addressof(domain_fs), std::addressof(object_id),
                std::move(program_fs), m_forward_service, program_id));
            auto object = MakeSharedFileSystem(std::move(domain_fs), false, true);
            R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
            out.SetValue(std::move(object), object_id);
            R_SUCCEED();
        }();
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsProgramFileSystemUsb, program_id,
            static_cast<u8>(ncm::ContentType::Program), usb_result, "USB Program RomFS filesystem");
        R_RETURN(usb_result);
    }

    Result FsMitmService::GetRightsIdAndKeyGenerationByPathObsolete(sf::Out<ams::fs::RightsId> out, sf::Out<u8> out_generation, const fssrv::sf::Path &path) {
        R_RETURN(this->GetRightsIdAndKeyGenerationByPath(out, out_generation, path, ams::fs::ContentAttributes_None));
    }

    Result FsMitmService::GetRightsIdAndKeyGenerationByPath(sf::Out<ams::fs::RightsId> out, sf::Out<u8> out_generation,
        const fssrv::sf::Path &path, ams::fs::ContentAttributes attributes) {
        R_UNLESS(attributes == ams::fs::ContentAttributes_None, sm::mitm::ResultShouldForwardToSession());

        /* Shared forwarder NCAs make content ID alone ambiguous. Preserve the
         * native SD/NAND answer whenever stock FS can open this exact path;
         * USB header translation is strictly a fallback for virtual paths. */
        ams::fs::RightsId stock_rights_id{};
        u8 stock_generation{};
        const Result stock_result = fsGetRightsIdAndKeyGenerationByPathFwd(m_forward_service.get(), path.str,
            static_cast<FsContentAttributes>(attributes), std::addressof(stock_generation),
            reinterpret_cast<FsRightsId *>(std::addressof(stock_rights_id)));
        usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsStockRightsHeaderResult,
            ncm::InvalidProgramId, 0, stock_result, path.str);
        if (R_SUCCEEDED(stock_result)) {
            out.SetValue(stock_rights_id);
            out_generation.SetValue(stock_generation);
            R_SUCCEED();
        }

        usb_storage::UsbNcaHeaderInfo info{};
        u64 usb_route_generation = 0;
        const bool filesystem_route_ready = ams::usb::content::UseFileSystemContentRoute
            && usb_storage::IsUsbContentFileSystemRouteReady(
                std::addressof(usb_route_generation), false);
        AMS_UNUSED(usb_route_generation);
        const Result result = filesystem_route_ready
            ? usb_storage::ReadUsbNcaHeaderByPath(std::addressof(info), path.str)
            : usb_storage::ReadMappedUsbNcaHeader(std::addressof(info), path.str);
        if (!sm::mitm::ResultShouldForwardToSession::Includes(result)) {
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsUsbRightsHeader, info.program_id, 0, result, path.str);
        }
        R_UNLESS(!sm::mitm::ResultShouldForwardToSession::Includes(result), stock_result);
        R_TRY(result);
        out.SetValue(info.rights_id);
        out_generation.SetValue(info.key_generation);
        R_SUCCEED();
    }

    Result FsMitmService::GetProgramId(sf::Out<ncm::ProgramId> out, const fssrv::sf::Path &path, ams::fs::ContentAttributes attributes) {
        R_UNLESS(attributes == ams::fs::ContentAttributes_None, sm::mitm::ResultShouldForwardToSession());

        u64 stock_program_id{};
        const Result stock_result = fsGetProgramIdFwd(m_forward_service.get(), std::addressof(stock_program_id),
            path.str, static_cast<FsContentAttributes>(attributes));
        usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsStockProgramHeaderResult,
            ncm::ProgramId{stock_program_id}, 0, stock_result, path.str);
        if (R_SUCCEEDED(stock_result)) {
            const ncm::ProgramId stock_identity{stock_program_id};
            if (usb_storage::HasCachedUsbProgramPath(stock_identity)) {
                RememberNativeProgramIdentity(path.str, attributes, stock_identity);
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsNativeIdentityCacheSeed,
                    stock_identity, 0, ResultSuccess(), path.str);
            }
            out.SetValue(stock_identity);
            R_SUCCEED();
        }

        usb_storage::UsbNcaHeaderInfo info{};
        u64 usb_route_generation = 0;
        const bool filesystem_route_ready = ams::usb::content::UseFileSystemContentRoute
            && usb_storage::IsUsbContentFileSystemRouteReady(
                std::addressof(usb_route_generation), false);
        AMS_UNUSED(usb_route_generation);
        const Result result = filesystem_route_ready
            ? usb_storage::ReadUsbNcaHeaderByPath(std::addressof(info), path.str)
            : usb_storage::ReadMappedUsbNcaHeader(std::addressof(info), path.str);
        if (!sm::mitm::ResultShouldForwardToSession::Includes(result)) {
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsUsbProgramHeader, info.program_id, 0, result, path.str);
        }
        R_UNLESS(!sm::mitm::ResultShouldForwardToSession::Includes(result), stock_result);
        R_TRY(result);
        out.SetValue(info.program_id);
        R_SUCCEED();
    }

    Result FsMitmService::OpenSdCardFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out) {
        usb_storage::VirtualSdLease lease{};
        if(usb_storage::GetVirtualSdLease(&lease,m_client_info.process_id.value)) {
            FsFileSystem sd_fs{};
            R_TRY(fsOpenSdCardFileSystemFwd(m_forward_service.get(),&sd_fs));
            const sf::cmif::DomainObjectId object_id{serviceGetObjectId(&sd_fs.s)};
            auto anchor=std::make_shared<RemoteFileSystem>(sd_fs);
            std::unique_ptr<ams::fs::fsa::IFileSystem> view;
            R_TRY(usb_storage::CreateVirtualSdFileSystem(&view,lease,std::move(anchor)));
            if(const u64 capture=usb_storage::GetStartupTimingCapture(m_client_info.process_id.value,NEXUS_STARTUP_SOURCE_USB))
                view=usb_storage::WrapStartupTimingFileSystem(std::move(view),capture);
            R_UNLESS(view,ams::fs::ResultAllocationMemoryFailedMakeUnique());
            out.SetValue(MakeSharedFileSystem(std::move(view),false),object_id);
            R_SUCCEED();
        }
        if(const u64 capture=usb_storage::GetStartupTimingCapture(m_client_info.process_id.value,NEXUS_STARTUP_SOURCE_SD)) {
            FsFileSystem sd_fs{};
            R_TRY(fsOpenSdCardFileSystemFwd(m_forward_service.get(),&sd_fs));
            const sf::cmif::DomainObjectId object_id{serviceGetObjectId(&sd_fs.s)};
            auto view=usb_storage::WrapStartupTimingFileSystem(std::make_shared<RemoteFileSystem>(sd_fs),capture);
            R_UNLESS(view,ams::fs::ResultAllocationMemoryFailedMakeUnique());
            out.SetValue(MakeSharedFileSystem(std::move(view),false),object_id);
            R_SUCCEED();
        }
        /* We only care about redirecting this for NS/emummc. */
        R_UNLESS(m_client_info.program_id == ncm::SystemProgramId::Ns, sm::mitm::ResultShouldForwardToSession());
        R_UNLESS(emummc::IsActive(),                                   sm::mitm::ResultShouldForwardToSession());

        /* Create a new SD card filesystem. */
        FsFileSystem sd_fs;
        R_TRY(fsOpenSdCardFileSystemFwd(m_forward_service.get(), std::addressof(sd_fs)));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(sd_fs.s))};

        /* Return output filesystem. */
        auto redir_fs = std::make_shared<fssystem::DirectoryRedirectionFileSystem>(std::make_unique<RemoteFileSystem>(sd_fs));
        R_TRY(redir_fs->InitializeWithFixedPath("/Nintendo", emummc::GetNintendoDirPath()));

        out.SetValue(MakeSharedFileSystem(std::move(redir_fs), false), target_object_id);
        R_SUCCEED();
    }

    Result FsMitmService::OpenContentStorageFileSystem(
        sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, u32 id) {
        /* Preserve every non-SD content storage exactly as Horizon exposes it.
         * The USB route overlays only the byte-bearing directories in the
         * logical SD content filesystem. */
        R_UNLESS(ams::usb::content::UseFileSystemContentRoute
            && id == static_cast<u32>(FsContentStorageId_SdCard),
            sm::mitm::ResultShouldForwardToSession());

        FsFileSystem stock_fs{};
        R_TRY(fsOpenContentStorageFileSystemFwd(m_forward_service.get(),
            std::addressof(stock_fs), static_cast<FsContentStorageId>(id)));
        const sf::cmif::DomainObjectId target_object_id{
            serviceGetObjectId(std::addressof(stock_fs.s))};

        std::shared_ptr<fs::fsa::IFileSystem> stock =
            std::make_shared<fs::RemoteFileSystem>(stock_fs);
        R_UNLESS(stock != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());

        std::unique_ptr<fs::fsa::IFileSystem> overlay;
        R_TRY(usb_storage::CreateUsbContentOverlayFileSystem(
            std::addressof(overlay), std::move(stock)));
        std::shared_ptr<fs::fsa::IFileSystem> shared_overlay(std::move(overlay));
        R_UNLESS(shared_overlay != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());

        out.SetValue(MakeSharedFileSystem(std::move(shared_overlay), false), target_object_id);
        R_SUCCEED();
    }

    Result FsMitmService::OpenSaveDataFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, u8 space_id, const fs::SaveDataAttribute &attribute) {
        /* We only want to intercept saves for games, right now. */
        const bool is_game_or_hbl = m_client_info.override_status.IsHbl() || ncm::IsApplicationId(m_client_info.program_id);
        R_UNLESS(is_game_or_hbl, sm::mitm::ResultShouldForwardToSession());

        /* Only redirect if the appropriate system setting is set. */
        R_UNLESS(GetSettingsItemBooleanValue("atmosphere", "fsmitm_redirect_saves_to_sd"), sm::mitm::ResultShouldForwardToSession());

        /* Only redirect if the specific title being accessed has a redirect save flag. */
        R_UNLESS(cfg::HasContentSpecificFlag(m_client_info.program_id, "redirect_save"), sm::mitm::ResultShouldForwardToSession());

        /* Only redirect account savedata. */
        R_UNLESS(attribute.type == fs::SaveDataType::Account, sm::mitm::ResultShouldForwardToSession());

        /* Get enum type for space id. */
        auto _space_id = static_cast<FsSaveDataSpaceId>(space_id);

        /* Verify we can open the save. */
        static_assert(sizeof(fs::SaveDataAttribute) == sizeof(::FsSaveDataAttribute));
        FsFileSystem save_fs;
        R_UNLESS(R_SUCCEEDED(fsOpenSaveDataFileSystemFwd(m_forward_service.get(), std::addressof(save_fs), _space_id, reinterpret_cast<const FsSaveDataAttribute *>(std::addressof(attribute)))), sm::mitm::ResultShouldForwardToSession());
        std::unique_ptr<fs::fsa::IFileSystem> save_ifs = std::make_unique<fs::RemoteFileSystem>(save_fs);

        /* Mount the SD card using fs.mitm's session. */
        FsFileSystem sd_fs;
        R_TRY(fsOpenSdCardFileSystem(std::addressof(sd_fs)));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(sd_fs.s))};
        std::shared_ptr<fs::fsa::IFileSystem> sd_ifs = std::make_shared<fs::RemoteFileSystem>(sd_fs);

        /* Verify that we can open the save directory, and that it exists. */
        const ncm::ProgramId application_id = attribute.program_id == ncm::InvalidProgramId ? m_client_info.program_id : attribute.program_id;

        char save_dir_raw_path[0x100];
        R_TRY(mitm::fs::SaveUtil::GetDirectorySaveDataPath(save_dir_raw_path, sizeof(save_dir_raw_path), application_id, _space_id, attribute));

        ams::fs::Path save_dir_path;
        R_TRY(save_dir_path.SetShallowBuffer(save_dir_raw_path));

        /* Check if this is the first time we're making the save. */
        bool is_new_save = false;
        {
            fs::DirectoryEntryType ent;
            R_TRY_CATCH(sd_ifs->GetEntryType(std::addressof(ent), save_dir_path)) {
                R_CATCH(fs::ResultPathNotFound) { is_new_save = true; }
                R_CATCH_ALL() { /* ... */ }
            } R_END_TRY_CATCH;
        }

        /* Ensure the directory exists. */
        R_TRY(fssystem::EnsureDirectory(sd_ifs.get(), save_dir_path));

        /* Create directory savedata filesystem. */
        auto subdir_fs = std::make_unique<fssystem::SubDirectoryFileSystem>(sd_ifs);
        R_TRY(subdir_fs->Initialize(save_dir_path));

        std::shared_ptr<fssystem::DirectorySaveDataFileSystem> dirsave_ifs = std::make_shared<fssystem::DirectorySaveDataFileSystem>(std::move(subdir_fs));

        /* Ensure correct directory savedata filesystem state. */
        R_TRY(dirsave_ifs->Initialize(true, true, true));

        /* If it's the first time we're making the save, copy existing savedata over. */
        if (is_new_save) {
            /* TODO: Check error? */
            fs::DirectoryEntry work_entry;
            constexpr const fs::Path root_path = fs::MakeConstantPath("/");

            u8 savedata_copy_buffer[2_KB];
            R_DISCARD(fssystem::CopyDirectoryRecursively(dirsave_ifs.get(), save_ifs.get(), root_path, root_path, std::addressof(work_entry), savedata_copy_buffer, sizeof(savedata_copy_buffer)));
        }

        /* Set output. */
        out.SetValue(MakeSharedFileSystem(std::move(dirsave_ifs), false), target_object_id);
        R_SUCCEED();
    }

    Result FsMitmService::OpenBisStorage(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorage>> out, u32 bis_partition_id) {
        const ::FsBisPartitionId _bis_partition_id = static_cast<::FsBisPartitionId>(bis_partition_id);

        /* Try to open a storage for the partition. */
        FsStorage bis_storage;
        R_TRY(fsOpenBisStorageFwd(m_forward_service.get(), std::addressof(bis_storage), _bis_partition_id));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(bis_storage.s))};

        const bool is_sysmodule = ncm::IsSystemProgramId(m_client_info.program_id);
        const bool is_hbl = m_client_info.override_status.IsHbl();
        const bool can_write_bis = is_sysmodule || (is_hbl && GetSettingsItemBooleanValue("atmosphere", "enable_hbl_bis_write"));

        /* Allow HBL to write to boot1 (safe firm) + package2. */
        /* This is needed to not break compatibility with ChoiDujourNX, which does not check for write access before beginning an update. */
        /* TODO: get fixed so that this can be turned off without causing bricks :/ */
        const bool is_package2 = (FsBisPartitionId_BootConfigAndPackage2Part1 <= _bis_partition_id && _bis_partition_id <= FsBisPartitionId_BootConfigAndPackage2Part6);
        const bool is_boot1    = _bis_partition_id == FsBisPartitionId_BootPartition2Root;
        const bool can_write_bis_for_choi_support = is_hbl && (is_package2 || is_boot1);

        /* Set output storage. */
        if (_bis_partition_id == FsBisPartitionId_BootPartition1Root) {
            if (IsBoot0CustomPublicKey(bis_storage)) {
                out.SetValue(MakeSharedStorage(std::make_shared<CustomPublicKeyBoot0Storage>(bis_storage, m_client_info, spl::GetSocType())), target_object_id);
            } else {
                out.SetValue(MakeSharedStorage(std::make_shared<Boot0Storage>(bis_storage, m_client_info)), target_object_id);
            }
        } else if (_bis_partition_id == FsBisPartitionId_CalibrationBinary) {
            out.SetValue(MakeSharedStorage(std::make_shared<CalibrationBinaryStorage>(bis_storage, m_client_info)), target_object_id);
        } else {
            if (can_write_bis || can_write_bis_for_choi_support) {
                /* We can write, so create a writable storage. */
                out.SetValue(MakeSharedStorage(std::make_shared<RemoteStorage>(bis_storage)), target_object_id);
            } else {
                /* We can only read, so create a readable storage. */
                std::unique_ptr<ams::fs::IStorage> unique_bis = std::make_unique<RemoteStorage>(bis_storage);
                out.SetValue(MakeSharedStorage(std::make_shared<ReadOnlyStorageAdapter>(std::move(unique_bis))), target_object_id);
            }
        }

        R_SUCCEED();
    }

    Result FsMitmService::OpenDataStorageByProgramId(
        sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorage>> out, ncm::ProgramId program_id) {
        FsStorage stock_storage{};
        const Result stock_result = fsOpenDataStorageByProgramIdFwd(m_forward_service.get(),
            std::addressof(stock_storage), program_id.value);
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsProgramStorageStock, program_id,
            static_cast<u8>(ncm::ContentType::Program), stock_result,
            m_client_info.program_id == ncm::SystemProgramId::Am ? "client=am" : nullptr);
        if (R_SUCCEEDED(stock_result)) {
            const sf::cmif::DomainObjectId object_id{serviceGetObjectId(std::addressof(stock_storage.s))};
            std::shared_ptr<ams::fs::IStorage> storage = std::make_shared<fs::RemoteStorage>(stock_storage);
            /* Preserve fresh Nexus's SD LayeredFS support in the unified native-
             * first/USB-fallback handler, without opening the native object twice. */
            if (m_client_info.override_status.IsProgramSpecific() && mitm::fs::HasSdRomfsContent(program_id)) {
                storage = GetLayeredRomfsStorage(program_id, std::move(storage), true, nullptr);
            }
            out.SetValue(MakeSharedStorage(std::move(storage)), object_id);
            R_SUCCEED();
        }

        R_UNLESS(fs::ResultPathNotFound::Includes(stock_result), stock_result);
        R_UNLESS(IsUsbContentByteRouteReady(), stock_result);
        R_UNLESS(!ncm::IsSystemProgramId(program_id), stock_result);
        R_UNLESS(usb_storage::HasCachedUsbProgramPath(program_id), stock_result);

        std::shared_ptr<ams::fs::IStorage> storage;
        Result usb_result = usb_storage::OpenUsbProgramRomFs(std::addressof(storage), program_id);
        if (R_SUCCEEDED(usb_result)) {
            usb_result = PublishUsbStorageForMitm(out, std::move(storage), m_forward_service, program_id, 201);
        }
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsProgramStorageUsb, program_id,
            static_cast<u8>(ncm::ContentType::Program), usb_result, "USB Program RomFS storage");
        R_RETURN(usb_result);
    }

    Result FsMitmService::OpenDataStorageByCurrentProcess(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorage>> out) {
        std::shared_ptr<ams::fs::IStorage> usb_mods;
        if (m_client_info.override_status.IsProgramSpecific()) {
            R_TRY(usb_storage::OpenUsbModRomfs(&usb_mods, m_client_info.program_id));
            R_UNLESS(!usb_mods || !mitm::fs::HasSdRomfsContent(m_client_info.program_id), fs::ResultInvalidArgument());
        }
        usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsCurrentStorageAttempt, m_client_info.program_id, 0, ResultSuccess(), nullptr, static_cast<u8>(m_client_info.override_status.IsProgramSpecific() ? ncm::StorageId::Host : ncm::StorageId::None));
        /* A stale title-map hint must not redirect a native SD launch.
         * The PID comes from SM, not the request. Keep USB errors authoritative
         * even after removal; never switch a running USB title to native data. */
        if (usb_storage::HasUsbProcessContent(m_client_info.process_id.value, m_client_info.program_id)) {
            std::shared_ptr<ams::fs::IStorage> storage;
            usb_storage::LaunchPin pin;
            const Result result = usb_storage::OpenPinnedProcessRomFs(&storage, usb_mods ? nullptr : &pin, m_client_info.process_id.value, m_client_info.program_id);
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsCurrentStorageResult, m_client_info.program_id, 0, result, nullptr, static_cast<u8>(ncm::StorageId::Host), static_cast<u8>(ncm::StorageId::Host));
            R_TRY(result);
            if (usb_mods) {
                R_TRY(usb_storage::SnapshotUsbModBase(&storage));
                storage=GetLayeredRomfsStorage(m_client_info.program_id,std::move(storage),true,std::move(usb_mods));
            }
            R_RETURN(PublishUsbStorageForMitm(out, std::move(storage), m_forward_service, m_client_info.program_id, 200, std::move(pin)));
        }

        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsNativeProcessResources, m_client_info.program_id, 0, ResultSuccess(), "R159 native process resources; no USB PID binding");

        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(),     sm::mitm::ResultShouldForwardToSession());

        /* Only mitm if there is actually an override romfs. */
        R_UNLESS(usb_mods || mitm::fs::HasSdRomfsContent(m_client_info.program_id), sm::mitm::ResultShouldForwardToSession());

        /* Try to open the process romfs. */
        FsStorage data_storage;
        R_TRY(fsOpenDataStorageByCurrentProcessFwd(m_forward_service.get(), std::addressof(data_storage)));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(data_storage.s))};

        /* Get a layered storage for the process romfs. */
        std::shared_ptr<ams::fs::IStorage> base=std::make_shared<ams::fs::RemoteStorage>(data_storage);
        if (usb_mods) R_TRY(usb_storage::SnapshotUsbModBase(&base));
        if (usb_mods) {
            char detail[96]{};
            util::SNPrintf(detail,sizeof(detail),"M2 mods native publish c=200 object=%u",serviceGetObjectId(&data_storage.s));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsUsbStorageHandoff,m_client_info.program_id,0,ResultSuccess(),detail);
        }
        out.SetValue(MakeSharedStorage(GetLayeredRomfsStorage(m_client_info.program_id, std::move(base), true, std::move(usb_mods))), target_object_id);
        R_SUCCEED();
    }
    
    Result FsMitmService::OpenDataStorageByDataId(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorage>> out, ncm::DataId _data_id, u8 storage_id) {
        const ncm::ProgramId usb_data_id = {_data_id.value};
        usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsDataIdStorageAttempt, usb_data_id, 0, ResultSuccess(), nullptr, storage_id, storage_id);

        /* Data NCAs are not Program RomFS. Match the requesting application's
         * launch snapshot, exact DataId and stock storage. Native access errors
         * remain authoritative; only missing byte-paths can fall back to USB. */
        if (usb_storage::HasUsbAddOnContent(m_client_info.program_id, _data_id, storage_id, m_client_info.process_id.value)) {
            FsStorage native{};
            const Result native_result = fsOpenDataStorageByDataIdFwd(m_forward_service.get(),
                std::addressof(native), _data_id.value, static_cast<NcmStorageId>(storage_id));
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsDataIdStorageResult,
                usb_data_id, static_cast<u8>(ncm::ContentType::Data), native_result, "R155 DLC native open", storage_id, storage_id);
            if (R_SUCCEEDED(native_result)) {
                const sf::cmif::DomainObjectId object_id{serviceGetObjectId(std::addressof(native.s))};
                out.SetValue(MakeSharedStorage(std::make_shared<ams::fs::RemoteStorage>(native)), object_id);
                R_SUCCEED();
            }
            R_UNLESS(ams::fs::ResultPathNotFound::Includes(native_result), native_result);
            std::shared_ptr<ams::fs::IStorage> storage;
            const Result result = usb_storage::OpenUsbAddOnRomFs(std::addressof(storage),
                m_client_info.program_id, _data_id, storage_id, m_client_info.process_id.value);
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsDataIdStorageResult,
                usb_data_id, static_cast<u8>(ncm::ContentType::Data), result, "R155 DLC Data RomFS", storage_id, static_cast<u8>(ncm::StorageId::Host));
            R_TRY(result);
            R_RETURN(PublishUsbStorageForMitm(out, std::move(storage), m_forward_service, usb_data_id, 202));
        }

        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(), sm::mitm::ResultShouldForwardToSession());

        /* TODO: Decide how to handle DataId vs ProgramId for this API. */
        const ncm::ProgramId program_or_data_id = {_data_id.value};

        /* Only mitm if there is actually an override romfs. */
        R_UNLESS(mitm::fs::HasSdRomfsContent(program_or_data_id),                  sm::mitm::ResultShouldForwardToSession());

        /* Try to open the data id. */
        FsStorage data_storage;
        R_TRY(fsOpenDataStorageByDataIdFwd(m_forward_service.get(), std::addressof(data_storage), static_cast<u64>(program_or_data_id), static_cast<NcmStorageId>(storage_id)));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(data_storage.s))};

        /* Get a layered storage for the data id. */
        out.SetValue(MakeSharedStorage(GetLayeredRomfsStorage(program_or_data_id, data_storage, false)), target_object_id);
        R_SUCCEED();
    }

    Result FsMitmService::OpenDataStorageWithProgramIndex(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorage>> out, u8 program_index) {
        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(), sm::mitm::ResultShouldForwardToSession());

        /* Get the relevant program id. */
        const ncm::ProgramId program_id = g_program_index_map_info_manager.GetProgramId(m_client_info.program_id, program_index);
        std::shared_ptr<ams::fs::IStorage> usb_mods;
        if (program_id != ncm::InvalidProgramId) {
            R_TRY(usb_storage::OpenUsbModRomfs(&usb_mods,program_id));
            R_UNLESS(!usb_mods || !mitm::fs::HasSdRomfsContent(program_id),fs::ResultInvalidArgument());
        }

        if (program_id != ncm::InvalidProgramId
            && usb_storage::HasUsbProcessContent(m_client_info.process_id.value, program_id)) {
            std::shared_ptr<ams::fs::IStorage> storage;
            usb_storage::LaunchPin pin;
            R_TRY(usb_storage::OpenPinnedProcessRomFs(&storage, usb_mods ? nullptr : &pin, m_client_info.process_id.value, program_id));
            if (usb_mods) {
                R_TRY(usb_storage::SnapshotUsbModBase(&storage));
                storage=GetLayeredRomfsStorage(program_id,std::move(storage),true,std::move(usb_mods));
            }
            R_RETURN(PublishUsbStorageForMitm(out, std::move(storage), m_forward_service, program_id, 205, std::move(pin)));
        }

        /* If we don't know about the program or don't have content, forward. */
        R_UNLESS(program_id != ncm::InvalidProgramId,     sm::mitm::ResultShouldForwardToSession());
        R_UNLESS(usb_mods || mitm::fs::HasSdRomfsContent(program_id), sm::mitm::ResultShouldForwardToSession());

        /* Try to open the process romfs. */
        FsStorage data_storage;
        R_TRY(fsOpenDataStorageWithProgramIndexFwd(m_forward_service.get(), std::addressof(data_storage), program_index));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(data_storage.s))};

        /* Get a layered storage for the process romfs. */
        std::shared_ptr<ams::fs::IStorage> base=std::make_shared<ams::fs::RemoteStorage>(data_storage);
        if (usb_mods) R_TRY(usb_storage::SnapshotUsbModBase(&base));
        if (usb_mods) {
            char detail[96]{};
            util::SNPrintf(detail,sizeof(detail),"M2 mods native publish c=205 object=%u",serviceGetObjectId(&data_storage.s));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsUsbStorageHandoff,program_id,0,ResultSuccess(),detail);
        }
        out.SetValue(MakeSharedStorage(GetLayeredRomfsStorage(program_id, std::move(base), true, std::move(usb_mods))), target_object_id);
        R_SUCCEED();
    }
    
    Result FsMitmService::OpenDataStorageByPath(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorage>> out, const fssrv::sf::Path &path, u8 content_attributes, u32 file_system_proxy_type) {
        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(),     sm::mitm::ResultShouldForwardToSession());
        
        /* No custom implementation: preserve native object/error semantics. */
        AMS_UNUSED(out, path, content_attributes, file_system_proxy_type);
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }
    
    Result FsMitmService::OpenDataStorageByCurrentProcessForBatchRead(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorageForBatchRead>> out) {
        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(),     sm::mitm::ResultShouldForwardToSession());

        /* Only mitm if there is actually an override romfs. */
        R_UNLESS(mitm::fs::HasSdRomfsContent(m_client_info.program_id), sm::mitm::ResultShouldForwardToSession());

        /* Try to open the process romfs. */
        FsStorageForBatchRead data_storage;
        R_TRY(fsOpenDataStorageByCurrentProcessForBatchReadFwd(m_forward_service.get(), std::addressof(data_storage)));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(data_storage.s))};

        /* Get a layered storage for the program romfs (batch read variant). */
        out.SetValue(MakeSharedStorageForBatchRead(GetLayeredRomfsStorageForBatchRead(m_client_info.program_id, data_storage, true)), target_object_id);
        R_SUCCEED();
    }
    
    Result FsMitmService::OpenDataStorageByProgramIdForBatchRead(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorageForBatchRead>> out, ncm::ProgramId program_id) {
        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(),     sm::mitm::ResultShouldForwardToSession());

        /* Only mitm if there is actually an override romfs for the target program. */
        R_UNLESS(mitm::fs::HasSdRomfsContent(program_id), sm::mitm::ResultShouldForwardToSession());

        /* Try to open the program romfs. */
        FsStorageForBatchRead data_storage;
        R_TRY(fsOpenDataStorageByProgramIdForBatchReadFwd(m_forward_service.get(), std::addressof(data_storage), static_cast<u64>(program_id)));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(data_storage.s))};

        /* Get a layered storage for the program romfs (batch read variant). */
        out.SetValue(MakeSharedStorageForBatchRead(GetLayeredRomfsStorageForBatchRead(program_id, data_storage, true)), target_object_id);
        R_SUCCEED();
    }
    
    Result FsMitmService::OpenDataStorageWithProgramIndexForBatchRead(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorageForBatchRead>> out, u8 program_index) {
        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(), sm::mitm::ResultShouldForwardToSession());

        /* Get the relevant program id. */
        const ncm::ProgramId program_id = g_program_index_map_info_manager.GetProgramId(m_client_info.program_id, program_index);

        /* If we don't know about the program or don't have content, forward. */
        R_UNLESS(program_id != ncm::InvalidProgramId,     sm::mitm::ResultShouldForwardToSession());
        R_UNLESS(mitm::fs::HasSdRomfsContent(program_id), sm::mitm::ResultShouldForwardToSession());

        /* Try to open the process romfs. */
        FsStorageForBatchRead data_storage;
        R_TRY(fsOpenDataStorageWithProgramIndexForBatchReadFwd(m_forward_service.get(), std::addressof(data_storage), program_index));
        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(data_storage.s))};

        /* Get a layered storage for the program romfs (batch read variant). */
        out.SetValue(MakeSharedStorageForBatchRead(GetLayeredRomfsStorageForBatchRead(program_id, data_storage, true)), target_object_id);
        R_SUCCEED();
    }
    
    Result FsMitmService::OpenDataStorageByPathForBatchRead(sf::Out<sf::SharedPointer<ams::fssrv::sf::IStorageForBatchRead>> out, const fssrv::sf::Path &path, u8 content_attributes, u32 file_system_proxy_type) {
        /* Only mitm if we should override contents for the current process. */
        R_UNLESS(m_client_info.override_status.IsProgramSpecific(),     sm::mitm::ResultShouldForwardToSession());
        
        /* No custom implementation: preserve native object/error semantics. */
        AMS_UNUSED(out, path, content_attributes, file_system_proxy_type);
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }
    
    Result FsMitmService::RegisterExternalKey(const ExternalKeyRegistration &request) {
        R_RETURN(ForwardExternalKeyRegistration(m_forward_service.get(), m_client_info.program_id, request));
    }

    Result FsMitmService::UnregisterAllExternalKey() {
        R_RETURN(ForwardAllExternalKeyRemoval(m_forward_service.get(), m_client_info.program_id));
    }

    Result FsMitmService::UnregisterExternalKey(const fs::RightsId &rights_id) {
        R_RETURN(ForwardExternalKeyRemoval(m_forward_service.get(), m_client_info.program_id, rights_id));
    }

    Result FsMitmService::RegisterProgramIndexMapInfo(const sf::InBuffer &info_buffer, s32 info_count) {
        /* Try to register with FS. */
        R_TRY(fsRegisterProgramIndexMapInfoFwd(m_forward_service.get(), info_buffer.GetPointer(), info_buffer.GetSize(), info_count));

        /* Register with ourselves. */
        R_ABORT_UNLESS(g_program_index_map_info_manager.Reset(reinterpret_cast<const fs::ProgramIndexMapInfo *>(info_buffer.GetPointer()), info_count));

        R_SUCCEED();
    }

}
