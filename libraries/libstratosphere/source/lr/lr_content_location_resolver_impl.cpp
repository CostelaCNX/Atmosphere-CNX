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
#include <stratosphere/usb/usb_content_proxy.hpp>
#include "lr_content_location_resolver_impl.hpp"

namespace {

    /* The registered LR object normally owns a metadata snapshot created by
     * Refresh().  A title can be installed after that snapshot was created,
     * so a USB launch lookup must reopen the persistent stock database at the
     * launch boundary.  Otherwise the NCA can be present and registered on
     * USB while LR still reports the title as missing. */
    ams::Result GetFreshUserProgramContentInfo(ams::ncm::ContentInfo *out, ams::ncm::StorageId preferred_storage, ams::ncm::ProgramId program_id) {
        AMS_ASSERT(out != nullptr);

        const auto metadata_candidates = preferred_storage == ams::ncm::StorageId::BuiltInUser
            ? std::array{ams::ncm::StorageId::BuiltInUser, ams::ncm::StorageId::SdCard}
            : std::array{ams::ncm::StorageId::SdCard, ams::ncm::StorageId::BuiltInUser};

        ams::Result result = ams::ncm::ResultContentMetaNotFound();
        for (const auto candidate : metadata_candidates) {
            ams::ncm::ContentMetaDatabase metadata_database;
            result = ams::ncm::OpenContentMetaDatabase(std::addressof(metadata_database), candidate);
            if (R_FAILED(result)) {
                continue;
            }

            result = metadata_database.GetLatestProgram(out, program_id);
            if (R_SUCCEEDED(result)) {
                R_RETURN(result);
            }
        }

        R_RETURN(result);
    }

    ams::Result ConvertHostContentPathToUsb(ams::lr::Path *out, const ams::ncm::Path &source) {
        AMS_ASSERT(out != nullptr);

        constexpr const char UsbContentPrefix[] = "usbcontent:";
        constexpr const char StockSdContentPrefix[] = "@Sdcard:/Nintendo/Contents";
        constexpr const char StockSdContentPrefixAlt[] = "@SdCard:/Nintendo/Contents";
        constexpr const char NcmSdContentPrefix[] = "@SdCardContent:";

        const char *relative_path = nullptr;
        if (std::strncmp(source.str, UsbContentPrefix, sizeof(UsbContentPrefix) - 1) == 0) {
            std::memset(out->str, 0, sizeof(out->str));
            std::memcpy(out->str, source.str, std::min(sizeof(out->str), sizeof(source.str)));
            out->str[sizeof(out->str) - 1] = '\0';
            R_SUCCEED();
        } else if (std::strncmp(source.str, StockSdContentPrefix, sizeof(StockSdContentPrefix) - 1) == 0) {
            relative_path = source.str + sizeof(StockSdContentPrefix) - 1;
        } else if (std::strncmp(source.str, StockSdContentPrefixAlt, sizeof(StockSdContentPrefixAlt) - 1) == 0) {
            relative_path = source.str + sizeof(StockSdContentPrefixAlt) - 1;
        } else if (std::strncmp(source.str, NcmSdContentPrefix, sizeof(NcmSdContentPrefix) - 1) == 0) {
            relative_path = source.str + sizeof(NcmSdContentPrefix) - 1;
        } else {
            R_THROW(ams::lr::ResultInvalidPath());
        }

        while (*relative_path == '/') {
            ++relative_path;
        }
        R_UNLESS(*relative_path != '\0', ams::lr::ResultInvalidPath());

        const int path_length = ams::util::SNPrintf(out->str, sizeof(out->str), "%s/%s", UsbContentPrefix, relative_path);
        R_UNLESS(path_length > 0 && static_cast<size_t>(path_length) < sizeof(out->str), ams::lr::ResultInvalidPath());
        R_SUCCEED();
    }

    void RecordUsbLrTrace(ams::usb::content::LoaderProbeStage stage, ams::ncm::ProgramId program_id, ams::Result result = ams::ResultSuccess(), const char *path = nullptr, ams::ncm::StorageId location_storage = ams::ncm::StorageId::None, ams::ncm::StorageId content_storage = ams::ncm::StorageId::None) {
        if (ams::ncm::IsSystemProgramId(program_id)) {
            return;
        }

        ams::os::NativeHandle service_handle;
        if (R_FAILED(ams::sm::GetServiceHandle(std::addressof(service_handle), ams::sm::ServiceName::Encode(ams::usb::content::ServiceName)))) {
            return;
        }

        Service usb_service{};
        serviceCreate(std::addressof(usb_service), service_handle);
        ON_SCOPE_EXIT { serviceClose(std::addressof(usb_service)); };

        ams::usb::content::LoaderTraceInfo info{};
        info.program_id = program_id.value;
        info.stage = static_cast<u32>(stage);
        info.result = result.GetValue();
        info.location_storage = static_cast<u8>(location_storage);
        info.content_storage = static_cast<u8>(content_storage);
        if (path != nullptr) {
            const size_t copied_length = std::min(std::strlen(path), sizeof(info.path) - 1);
            std::memcpy(info.path, path, copied_length);
            info.path[copied_length] = '\0';
            if (copied_length < std::strlen(path)) {
                info.flags |= ams::usb::content::LoaderTraceFlag_PathTruncated;
            }
        }

        const ams::Result record_result = serviceDispatchIn(std::addressof(usb_service), ams::usb::content::CommandId_RecordLoaderTrace, info);
        AMS_UNUSED(record_result);
    }

}

namespace ams::lr {

    Result ContentLocationResolverImpl::TryResolveUsbProgramPath(Path *out, RedirectionAttributes *out_attr, ncm::ProgramId id) {
        AMS_ASSERT(out != nullptr);
        AMS_ASSERT(out_attr != nullptr);

        /* The resolver is created before AMS arms the USB route, so its
         * cached m_content_storage normally points at SD. Re-check the
         * optional Host backend at launch time for user applications. */
        if (!m_enabled || ncm::IsSystemProgramId(id)) {
            R_THROW(lr::ResultProgramNotFound());
        }
        if (m_storage_id != ncm::StorageId::SdCard && m_storage_id != ncm::StorageId::BuiltInUser) {
            R_THROW(lr::ResultProgramNotFound());
        }
        /* The exact persistent title map is the launch authorization. A
         * volatile ready sample can be stale between device detection and
         * Loader's request, so let Host Has/GetPath perform the authoritative
         * bounded live-medium check. Unmapped applications never touch Host. */
        if (!ncm::IsUsbTitleMapped(id)) {
            R_THROW(lr::ResultProgramNotFound());
        }

        /* Metadata remains on the requested stock database; only the NCA
         * bytes are looked up in Host content storage.  Use a fresh database
         * handle here because this resolver's normal metadata object may be
         * a boot-time snapshot that predates the install. */
        ncm::ContentInfo program_content_info;
        R_TRY(GetFreshUserProgramContentInfo(std::addressof(program_content_info), m_storage_id, id));

        ncm::ContentStorage host_storage;
        R_TRY(ncm::OpenContentStorage(std::addressof(host_storage), ncm::StorageId::Host));

        bool present = false;
        R_TRY(host_storage.Has(std::addressof(present), program_content_info.GetId()));
        R_UNLESS(present, lr::ResultProgramNotFound());

        ncm::Path host_path{};
        R_TRY(host_storage.GetPath(std::addressof(host_path), program_content_info.GetId()));
        R_TRY(ConvertHostContentPathToUsb(out, host_path));
        *out_attr = RedirectionAttributes::Make(program_content_info.GetContentAttributes());
        R_SUCCEED();
    }

    ContentLocationResolverImpl::~ContentLocationResolverImpl() {
        this->ClearRedirections();
    }

    /* Helper function. */
    void ContentLocationResolverImpl::GetContentStoragePath(Path *out, ncm::ContentId content_id) {
        static_assert(sizeof(lr::Path) == sizeof(ncm::Path));
        m_content_storage.GetPath(reinterpret_cast<ncm::Path *>(out), content_id);
    }

    Result ContentLocationResolverImpl::ResolveProgramPath(Path *out, RedirectionAttributes *out_attr, ncm::ProgramId id) {
        /* Use a redirection if present. */
        R_SUCCEED_IF(m_program_redirector.FindRedirection(out, out_attr, id));

        const bool trace_usb_lookup = !ncm::IsSystemProgramId(id) && (m_storage_id == ncm::StorageId::SdCard || m_storage_id == ncm::StorageId::BuiltInUser);
        if (trace_usb_lookup) {
            RecordUsbLrTrace(usb::content::LoaderProbeStage::LrResolveProgramPathAttempt, id, ResultSuccess(), nullptr, m_storage_id);
        }

        /* When USB content is armed, prefer an exact Host copy for the
         * application before using the resolver's boot-time SD snapshot. */
        const Result usb_lookup_result = this->TryResolveUsbProgramPath(out, out_attr, id);
        if (R_SUCCEEDED(usb_lookup_result)) {
            if (trace_usb_lookup) {
                RecordUsbLrTrace(usb::content::LoaderProbeStage::LrResolveProgramPathResult, id, ResultSuccess(), out->str, m_storage_id, ncm::StorageId::Host);
            }
            R_SUCCEED();
        } else if (trace_usb_lookup && ncm::IsUsbTitleMapped(id)) {
            RecordUsbLrTrace(usb::content::LoaderProbeStage::LrResolveProgramPathResult, id, usb_lookup_result, nullptr, m_storage_id, ncm::StorageId::Host);
        }

        /* If we're not enabled, we can't resolve a program. */
        R_UNLESS(m_enabled, lr::ResultProgramNotFound())

        /* Find the latest program content for the program id.  For user
         * applications, refresh the metadata view at launch time so a title
         * installed after LR initialization is visible immediately. */
        ncm::ContentInfo program_content_info;
        if (!ncm::IsSystemProgramId(id) && (m_storage_id == ncm::StorageId::SdCard || m_storage_id == ncm::StorageId::BuiltInUser)) {
            R_TRY_CATCH(GetFreshUserProgramContentInfo(std::addressof(program_content_info), m_storage_id, id)) {
                R_CONVERT(ncm::ResultContentMetaNotFound, lr::ResultProgramNotFound())
            } R_END_TRY_CATCH;
        } else {
            R_TRY_CATCH(m_content_meta_database.GetLatestProgram(std::addressof(program_content_info), id)) {
                R_CONVERT(ncm::ResultContentMetaNotFound, lr::ResultProgramNotFound())
            } R_END_TRY_CATCH;
        }

        /* Obtain the content path and attributes. */
        this->GetContentStoragePath(out, program_content_info.GetId());
        *out_attr = RedirectionAttributes::Make(program_content_info.GetContentAttributes());

        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ResolveProgramPath(sf::Out<Path> out, ncm::ProgramId id) {
        RedirectionAttributes attr;
        R_RETURN(this->ResolveProgramPath(out.GetPointer(), std::addressof(attr), id));
    }

    Result ContentLocationResolverImpl::RedirectProgramPath(const Path &path, ncm::ProgramId id) {
        m_program_redirector.SetRedirection(id, path, DefaultRedirectionAttributes);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ResolveApplicationControlPath(sf::Out<Path> out, ncm::ProgramId id) {
        RedirectionAttributes attr;
        R_UNLESS(m_app_control_redirector.FindRedirection(out.GetPointer(), std::addressof(attr), id), lr::ResultControlNotFound());
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ResolveApplicationHtmlDocumentPath(sf::Out<Path> out, ncm::ProgramId id) {
        RedirectionAttributes attr;
        R_UNLESS(m_html_docs_redirector.FindRedirection(out.GetPointer(), std::addressof(attr), id), lr::ResultHtmlDocumentNotFound());
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ResolveDataPath(sf::Out<Path> out, ncm::DataId id) {
        /* If we're not enabled, we can't resolve data. */
        R_UNLESS(m_enabled, lr::ResultDataNotFound())

        /* Find the latest data content info for the program id. */
        ncm::ContentInfo data_content_info;
        R_TRY(m_content_meta_database.GetLatestData(std::addressof(data_content_info), id));

        /* Obtain the content path. */
        this->GetContentStoragePath(out.GetPointer(), data_content_info.GetId());

        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationControlPathDeprecated(const Path &path, ncm::ProgramId id) {
        m_app_control_redirector.SetRedirection(id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationControlPath(const Path &path, ncm::ProgramId id, ncm::ProgramId owner_id) {
        m_app_control_redirector.SetRedirection(id, owner_id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationHtmlDocumentPathDeprecated(const Path &path, ncm::ProgramId id) {
        m_html_docs_redirector.SetRedirection(id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationHtmlDocumentPath(const Path &path, ncm::ProgramId id, ncm::ProgramId owner_id) {
        m_html_docs_redirector.SetRedirection(id, owner_id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ResolveApplicationLegalInformationPath(sf::Out<Path> out, ncm::ProgramId id) {
        RedirectionAttributes attr;
        R_UNLESS(m_legal_info_redirector.FindRedirection(out.GetPointer(), std::addressof(attr), id), lr::ResultLegalInformationNotFound());
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationLegalInformationPathDeprecated(const Path &path, ncm::ProgramId id) {
        m_legal_info_redirector.SetRedirection(id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationLegalInformationPath(const Path &path, ncm::ProgramId id, ncm::ProgramId owner_id) {
        m_legal_info_redirector.SetRedirection(id, owner_id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::Refresh() {
        /* Obtain content meta database and content storage objects for this resolver's storage. */
        ncm::ContentMetaDatabase meta_db;
        ncm::ContentStorage storage;
        R_TRY(ncm::OpenContentMetaDatabase(std::addressof(meta_db), m_storage_id));
        R_TRY(ncm::OpenContentStorage(std::addressof(storage), m_storage_id));

        /* Store the acquired objects. */
        m_content_meta_database = std::move(meta_db);
        m_content_storage       = std::move(storage);

        /* Remove any existing redirections. */
        this->ClearRedirections();

        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationProgramPathDeprecated(const Path &path, ncm::ProgramId id) {
        m_program_redirector.SetRedirection(id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationProgramPath(const Path &path, ncm::ProgramId id, ncm::ProgramId owner_id) {
        m_program_redirector.SetRedirection(id, owner_id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ClearApplicationRedirectionDeprecated() {
        this->ClearRedirections(RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ClearApplicationRedirection(const sf::InArray<ncm::ProgramId> &excluding_ids) {
        this->ClearRedirections(excluding_ids.GetPointer(), excluding_ids.GetSize());
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::EraseProgramRedirection(ncm::ProgramId id) {
        m_program_redirector.EraseRedirection(id);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::EraseApplicationControlRedirection(ncm::ProgramId id) {
        m_app_control_redirector.EraseRedirection(id);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::EraseApplicationHtmlDocumentRedirection(ncm::ProgramId id) {
        m_html_docs_redirector.EraseRedirection(id);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::EraseApplicationLegalInformationRedirection(ncm::ProgramId id) {
        m_legal_info_redirector.EraseRedirection(id);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::ResolveProgramPathForDebug(sf::Out<Path> out, ncm::ProgramId id) {
        /* Use a redirection if present. */
        RedirectionAttributes attr;
        R_SUCCEED_IF(m_debug_program_redirector.FindRedirection(out.GetPointer(), std::addressof(attr), id));

        /* If we're not enabled, we can't resolve a program. */
        R_UNLESS(m_enabled, lr::ResultDebugProgramNotFound())

        /* Otherwise find the path for the program id. */
        R_TRY_CATCH(this->ResolveProgramPath(out.GetPointer(), std::addressof(attr), id)) {
            R_CONVERT(lr::ResultProgramNotFound, lr::ResultDebugProgramNotFound())
        } R_END_TRY_CATCH;

        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectProgramPathForDebug(const Path &path, ncm::ProgramId id) {
        m_debug_program_redirector.SetRedirection(id, path, DefaultRedirectionAttributes);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationProgramPathForDebugDeprecated(const Path &path, ncm::ProgramId id) {
        m_debug_program_redirector.SetRedirection(id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::RedirectApplicationProgramPathForDebug(const Path &path, ncm::ProgramId id, ncm::ProgramId owner_id) {
        m_debug_program_redirector.SetRedirection(id, owner_id, path, DefaultRedirectionAttributes, RedirectionFlags_Application);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::EraseProgramRedirectionForDebug(ncm::ProgramId id) {
        m_debug_program_redirector.EraseRedirection(id);
        R_SUCCEED();
    }

    Result ContentLocationResolverImpl::Disable() {
        m_enabled = false;
        R_SUCCEED();
    }

}
