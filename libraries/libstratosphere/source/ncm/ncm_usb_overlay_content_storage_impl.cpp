/*
 * Copyright (c) Atmosphere-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>
#include <stratosphere/ncm/ncm_usb_overlay_content_storage_impl.hpp>
#include "ncm_lookup_trace.hpp"

namespace ams::ncm {

    Result UsbOverlayContentStorageImpl::EnsureEnabled() const {
        R_UNLESS(!m_disabled, ncm::ResultInvalidContentStorage());
        R_SUCCEED();
    }

    bool UsbOverlayContentStorageImpl::IsMappedContentId(ContentId content_id) const {
        /* Do not mount the SD-backed USB map during Horizon's boot-time NCM
         * enumeration. The in-memory route is armed only after NCM and the
         * rest of Horizon have initialized. */
        if (!IsUsbContentEnabled()) {
            return false;
        }
        return IsUsbContentLocationRegistered(content_id);
    }

    IContentStorage *UsbOverlayContentStorageImpl::SelectContentStorage(ContentId content_id) {
        /* Never ask USB whether it has an arbitrary content id. DBI and
         * Horizon enumerate the SD-facing storage through Has/List calls;
         * probing Host for each item can block that scan. Only content ids
         * explicitly persisted for a USB title may select the Host backend. */
        if (!this->IsMappedContentId(content_id)) {
            return m_stock.Get();
        }

        /* An explicit USB location must never turn into a different SD file
         * merely because USB is absent or an I/O request failed. */
        return m_usb.Get();
    }

    IContentStorage *UsbOverlayContentStorageImpl::SelectWriteStorage() {
        /* Keep read mapping independent from the selected write target. The
         * cached SD object can continue resolving known USB NCAs while a
         * caller deliberately writes unrelated content to the real SD card. */
        return IsUsbContentEnabledForInstall() ? m_usb.Get() : m_stock.Get();
    }

    Result UsbOverlayContentStorageImpl::GeneratePlaceHolderId(sf::Out<PlaceHolderId> out) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GeneratePlaceHolderId(out));
    }

    Result UsbOverlayContentStorageImpl::CreatePlaceHolder(PlaceHolderId placeholder_id, ContentId content_id, s64 size) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->CreatePlaceHolder(placeholder_id, content_id, size));
    }

    Result UsbOverlayContentStorageImpl::DeletePlaceHolder(PlaceHolderId placeholder_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->DeletePlaceHolder(placeholder_id));
    }

    Result UsbOverlayContentStorageImpl::HasPlaceHolder(sf::Out<bool> out, PlaceHolderId placeholder_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->HasPlaceHolder(out, placeholder_id));
    }

    Result UsbOverlayContentStorageImpl::WritePlaceHolder(PlaceHolderId placeholder_id, s64 offset, const sf::InBuffer &data) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->WritePlaceHolder(placeholder_id, offset, data));
    }

    Result UsbOverlayContentStorageImpl::Register(PlaceHolderId placeholder_id, ContentId content_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());

        auto *write_storage = this->SelectWriteStorage();
        if (write_storage == m_stock.Get()) {
            /* A real-SD install supersedes any old Host location for this
             * content ID. Without removing the durable entry here, the new
             * SD NCA is registered successfully but later Has/GetPath calls
             * still select usbcontent:, making ordinary SD forwarders fail
             * before their code can start. */
            R_TRY(write_storage->Register(placeholder_id, content_id));
            R_TRY(UnregisterUsbContentLocation(content_id));
            R_SUCCEED();
        }

        /* P1: Match native SD registration: an existing content ID remains
         * ContentAlreadyExists. Equal size does not prove equal NCA bytes.
         * The installer owns duplicate verification/replacement and placeholder
         * cleanup; swallowing the conflict bypasses its hash-verification path.
         * Do not delete a freshly written placeholder or publish a route for
         * unverified existing content. Host registers the route on success. */
        R_RETURN(write_storage->Register(placeholder_id, content_id));
    }

    Result UsbOverlayContentStorageImpl::Delete(ContentId content_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectContentStorage(content_id)->Delete(content_id));
    }

    Result UsbOverlayContentStorageImpl::Has(sf::Out<bool> out, ContentId content_id) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB" : "stock SD";
            R_RETURN(storage->Has(out, content_id));
        }();
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayHas, StorageId::SdCard, content_id,
            result, R_SUCCEEDED(result), R_SUCCEEDED(result) && *out.GetPointer(), selected);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::GetPath(sf::Out<Path> out, ContentId content_id) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB" : "stock SD";
            R_RETURN(storage->GetPath(out, content_id));
        }();
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayPath, StorageId::SdCard, content_id,
            result, R_SUCCEEDED(result), false, selected);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::GetPlaceHolderPath(sf::Out<Path> out, PlaceHolderId placeholder_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetPlaceHolderPath(out, placeholder_id));
    }

    Result UsbOverlayContentStorageImpl::CleanupAllPlaceHolder() {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->CleanupAllPlaceHolder());
    }

    Result UsbOverlayContentStorageImpl::ListPlaceHolder(sf::Out<s32> out_count, const sf::OutArray<PlaceHolderId> &out_buf) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->ListPlaceHolder(out_count, out_buf));
    }

    Result UsbOverlayContentStorageImpl::GetContentCount(sf::Out<s32> out_count) {
        std::scoped_lock lk(m_mutex);
        const char *mode = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());

            if (!IsUsbContentEnabled()) {
                mode = "stock SD";
                R_RETURN(m_stock->GetContentCount(out_count));
            }

            mode = "merged SD+USB";
            s32 count = 0;
            R_TRY(m_stock->GetContentCount(std::addressof(count)));
            ContentId batch[32]{};
            for (size_t offset = 0; ; offset += util::size(batch)) {
                size_t read = 0;
                R_TRY(ListUsbContentLocations(batch, util::size(batch), offset, std::addressof(read)));
                for (size_t i = 0; i < read; ++i) {
                    bool on_sd = false;
                    R_TRY(m_stock->Has(std::addressof(on_sd), batch[i]));
                    if (!on_sd) {
                        ++count;
                    }
                }
                if (read < util::size(batch)) {
                    break;
                }
            }
            out_count.SetValue(count);
            R_SUCCEED();
        }();
        char detail[48]{};
        util::SNPrintf(detail, sizeof(detail), "%s count=%d", mode,
            R_SUCCEEDED(result) ? *out_count.GetPointer() : -1);
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayCount, StorageId::SdCard,
            InvalidContentId, result, R_SUCCEEDED(result),
            R_SUCCEEDED(result) && *out_count.GetPointer() > 0, detail);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::ListContentId(sf::Out<s32> out_count, const sf::OutArray<ContentId> &out_buf, s32 start_offset) {
        std::scoped_lock lk(m_mutex);
        const char *mode = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            R_UNLESS(start_offset >= 0, ncm::ResultInvalidOffset());

            if (!IsUsbContentEnabled()) {
                mode = "stock SD";
                R_RETURN(m_stock->ListContentId(out_count, out_buf, start_offset));
            }

            mode = "merged SD+USB";
            /* Merge the cached registration list, not a live USB directory walk.
             * Count/list and exact-ID queries now describe the same content. */
            s32 stock_count = 0;
            s32 written = 0;
            R_TRY(m_stock->GetContentCount(std::addressof(stock_count)));
            if (start_offset < stock_count && out_buf.GetSize() > 0) {
                R_TRY(m_stock->ListContentId(std::addressof(written), out_buf, start_offset));
            }
            size_t skip = start_offset > stock_count ? static_cast<size_t>(start_offset - stock_count) : 0;
            ContentId batch[32]{};
            for (size_t offset = 0; static_cast<size_t>(written) < out_buf.GetSize(); offset += util::size(batch)) {
                size_t read = 0;
                R_TRY(ListUsbContentLocations(batch, util::size(batch), offset, std::addressof(read)));
                for (size_t i = 0; i < read && static_cast<size_t>(written) < out_buf.GetSize(); ++i) {
                    bool on_sd = false;
                    R_TRY(m_stock->Has(std::addressof(on_sd), batch[i]));
                    if (on_sd) {
                        continue;
                    }
                    if (skip > 0) {
                        --skip;
                    } else {
                        out_buf[written++] = batch[i];
                    }
                }
                if (read < util::size(batch)) {
                    break;
                }
            }
            out_count.SetValue(written);
            R_SUCCEED();
        }();
        char detail[48]{};
        util::SNPrintf(detail, sizeof(detail), "%s off=%d cap=%zu got=%d", mode, start_offset,
            out_buf.GetSize(), R_SUCCEEDED(result) ? *out_count.GetPointer() : -1);
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayList, StorageId::SdCard,
            InvalidContentId, result, R_SUCCEEDED(result),
            R_SUCCEEDED(result) && *out_count.GetPointer() > 0, detail);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::GetSizeFromContentId(sf::Out<s64> out_size, ContentId content_id) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB" : "stock SD";
            R_RETURN(storage->GetSizeFromContentId(out_size, content_id));
        }();
        char detail[48]{};
        util::SNPrintf(detail, sizeof(detail), "%s size=%lld", selected,
            static_cast<long long>(R_SUCCEEDED(result) ? *out_size.GetPointer() : -1));
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlaySize, StorageId::SdCard,
            content_id, result, R_SUCCEEDED(result), false, detail);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::DisableForcibly() {
        std::scoped_lock lk(m_mutex);
        m_disabled = true;
        R_SUCCEED();
    }

    Result UsbOverlayContentStorageImpl::RevertToPlaceHolder(PlaceHolderId placeholder_id, ContentId old_content_id, ContentId new_content_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->RevertToPlaceHolder(placeholder_id, old_content_id, new_content_id));
    }

    Result UsbOverlayContentStorageImpl::SetPlaceHolderSize(PlaceHolderId placeholder_id, s64 size) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->SetPlaceHolderSize(placeholder_id, size));
    }

    Result UsbOverlayContentStorageImpl::ReadContentIdFile(const sf::OutBuffer &buf, ContentId content_id, s64 offset) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB" : "stock SD";
            R_RETURN(storage->ReadContentIdFile(buf, content_id, offset));
        }();
        char detail[48]{};
        util::SNPrintf(detail, sizeof(detail), "%s off=%lld bytes=%zu", selected,
            static_cast<long long>(offset), buf.GetSize());
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayRead, StorageId::SdCard,
            content_id, result, R_SUCCEEDED(result), false, detail);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::GetRightsIdFromPlaceHolderIdDeprecated(sf::Out<ams::fs::RightsId> out_rights_id, PlaceHolderId placeholder_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetRightsIdFromPlaceHolderIdDeprecated(out_rights_id, placeholder_id));
    }

    Result UsbOverlayContentStorageImpl::GetRightsIdFromPlaceHolderIdDeprecated2(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetRightsIdFromPlaceHolderIdDeprecated2(out_rights_id, placeholder_id));
    }

    Result UsbOverlayContentStorageImpl::GetRightsIdFromPlaceHolderId(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id, fs::ContentAttributes attr) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetRightsIdFromPlaceHolderId(out_rights_id, placeholder_id, attr));
    }

    Result UsbOverlayContentStorageImpl::GetRightsIdFromContentIdDeprecated(sf::Out<ams::fs::RightsId> out_rights_id, ContentId content_id) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB legacy" : "stock SD legacy";
            R_RETURN(storage->GetRightsIdFromContentIdDeprecated(out_rights_id, content_id));
        }();
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayRights, StorageId::SdCard,
            content_id, result, R_SUCCEEDED(result), false, selected);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::GetRightsIdFromContentIdDeprecated2(sf::Out<ncm::RightsId> out_rights_id, ContentId content_id) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB legacy2" : "stock SD legacy2";
            R_RETURN(storage->GetRightsIdFromContentIdDeprecated2(out_rights_id, content_id));
        }();
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayRights, StorageId::SdCard,
            content_id, result, R_SUCCEEDED(result), false, selected);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::GetRightsIdFromContentId(sf::Out<ncm::RightsId> out_rights_id, ContentId content_id, fs::ContentAttributes attr) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB current" : "stock SD current";
            R_RETURN(storage->GetRightsIdFromContentId(out_rights_id, content_id, attr));
        }();
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayRights, StorageId::SdCard,
            content_id, result, R_SUCCEEDED(result), false, selected);
        R_RETURN(result);
    }

    Result UsbOverlayContentStorageImpl::WriteContentForDebug(ContentId content_id, s64 offset, const sf::InBuffer &data) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->WriteContentForDebug(content_id, offset, data));
    }

    Result UsbOverlayContentStorageImpl::GetFreeSpaceSize(sf::Out<s64> out_size) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetFreeSpaceSize(out_size));
    }

    Result UsbOverlayContentStorageImpl::GetTotalSpaceSize(sf::Out<s64> out_size) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetTotalSpaceSize(out_size));
    }

    Result UsbOverlayContentStorageImpl::FlushPlaceHolder() {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->FlushPlaceHolder());
    }

    Result UsbOverlayContentStorageImpl::GetSizeFromPlaceHolderId(sf::Out<s64> out, PlaceHolderId placeholder_id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetSizeFromPlaceHolderId(out, placeholder_id));
    }

    Result UsbOverlayContentStorageImpl::RepairInvalidFileAttribute() {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        if (!IsUsbContentEnabled()) {
            R_RETURN(m_stock->RepairInvalidFileAttribute());
        }
        AMS_UNUSED(m_usb->RepairInvalidFileAttribute());
        R_RETURN(m_stock->RepairInvalidFileAttribute());
    }

    Result UsbOverlayContentStorageImpl::GetRightsIdFromPlaceHolderIdWithCache(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id, ContentId cache_content_id, fs::ContentAttributes attr) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->GetRightsIdFromPlaceHolderIdWithCache(out_rights_id, placeholder_id, cache_content_id, attr));
    }

    Result UsbOverlayContentStorageImpl::RegisterPath(const ContentId &content_id, const Path &path) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->RegisterPath(content_id, path));
    }

    Result UsbOverlayContentStorageImpl::ClearRegisteredPath() {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureEnabled());
        R_RETURN(this->SelectWriteStorage()->ClearRegisteredPath());
    }

    Result UsbOverlayContentStorageImpl::GetProgramId(sf::Out<ncm::ProgramId> out, ContentId content_id, fs::ContentAttributes attr) {
        std::scoped_lock lk(m_mutex);
        const char *selected = "selection not reached";
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureEnabled());
            auto *storage = this->SelectContentStorage(content_id);
            selected = storage == m_usb.Get() ? "Host/USB" : "stock SD";
            R_RETURN(storage->GetProgramId(out, content_id, attr));
        }();
        char detail[48]{};
        util::SNPrintf(detail, sizeof(detail), "%s program=%016llX", selected,
            static_cast<unsigned long long>(R_SUCCEEDED(result) ? out.GetPointer()->value : 0));
        lookup_trace::Storage(usb_recovery::LookupOperation::OverlayProgramId, StorageId::SdCard,
            content_id, result, R_SUCCEEDED(result), false, detail);
        R_RETURN(result);
    }

}
