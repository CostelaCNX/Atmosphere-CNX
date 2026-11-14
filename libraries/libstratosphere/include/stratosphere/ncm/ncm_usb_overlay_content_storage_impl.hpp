/*
 * Copyright (c) Atmosphere-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#pragma once

#include <stratosphere.hpp>
#include <stratosphere/ncm/ncm_i_content_storage.hpp>
#include <stratosphere/ncm/ncm_usb_title_map.hpp>

namespace ams::ncm {

    /*
     * Presents the USB Host storage as an SD-card content overlay.
     *
     * The wrapper is safe to hand to Horizon before USB routing is armed. In
     * that state every operation is forwarded to the stock SD storage and no
     * USB map or device is touched. Once the post-boot route flag is enabled,
     * new placeholders and registrations are written to USB and reads are
     * selected per ContentId. This lets already-cached NCM handles observe the
     * route change without replacing Horizon's StorageId, content-meta
     * database or application records.
     */
    class UsbOverlayContentStorageImpl {
        NON_COPYABLE(UsbOverlayContentStorageImpl);
        NON_MOVEABLE(UsbOverlayContentStorageImpl);
        private:
            os::SdkRecursiveMutex m_mutex;
            sf::SharedPointer<IContentStorage> m_usb;
            sf::SharedPointer<IContentStorage> m_stock;
            bool m_disabled;
        private:
            Result EnsureEnabled() const;
            bool IsMappedContentId(ContentId content_id) const;
            IContentStorage *SelectContentStorage(ContentId content_id);
            IContentStorage *SelectWriteStorage();
        public:
            UsbOverlayContentStorageImpl(sf::SharedPointer<IContentStorage> usb, sf::SharedPointer<IContentStorage> stock) :
                m_mutex(), m_usb(std::move(usb)), m_stock(std::move(stock)), m_disabled(false) {
                /* Opening a storage handle must not probe USB or walk maps.
                 * All instances share the exact SD-backed location registry. */
            }

            Result GeneratePlaceHolderId(sf::Out<PlaceHolderId> out);
            Result CreatePlaceHolder(PlaceHolderId placeholder_id, ContentId content_id, s64 size);
            Result DeletePlaceHolder(PlaceHolderId placeholder_id);
            Result HasPlaceHolder(sf::Out<bool> out, PlaceHolderId placeholder_id);
            Result WritePlaceHolder(PlaceHolderId placeholder_id, s64 offset, const sf::InBuffer &data);
            Result Register(PlaceHolderId placeholder_id, ContentId content_id);
            Result Delete(ContentId content_id);
            Result Has(sf::Out<bool> out, ContentId content_id);
            Result GetPath(sf::Out<Path> out, ContentId content_id);
            Result GetPlaceHolderPath(sf::Out<Path> out, PlaceHolderId placeholder_id);
            Result CleanupAllPlaceHolder();
            Result ListPlaceHolder(sf::Out<s32> out_count, const sf::OutArray<PlaceHolderId> &out_buf);
            Result GetContentCount(sf::Out<s32> out_count);
            Result ListContentId(sf::Out<s32> out_count, const sf::OutArray<ContentId> &out_buf, s32 start_offset);
            Result GetSizeFromContentId(sf::Out<s64> out_size, ContentId content_id);
            Result DisableForcibly();
            Result RevertToPlaceHolder(PlaceHolderId placeholder_id, ContentId old_content_id, ContentId new_content_id);
            Result SetPlaceHolderSize(PlaceHolderId placeholder_id, s64 size);
            Result ReadContentIdFile(const sf::OutBuffer &buf, ContentId content_id, s64 offset);
            Result GetRightsIdFromPlaceHolderIdDeprecated(sf::Out<ams::fs::RightsId> out_rights_id, PlaceHolderId placeholder_id);
            Result GetRightsIdFromPlaceHolderIdDeprecated2(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id);
            Result GetRightsIdFromPlaceHolderId(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id, fs::ContentAttributes attr);
            Result GetRightsIdFromContentIdDeprecated(sf::Out<ams::fs::RightsId> out_rights_id, ContentId content_id);
            Result GetRightsIdFromContentIdDeprecated2(sf::Out<ncm::RightsId> out_rights_id, ContentId content_id);
            Result GetRightsIdFromContentId(sf::Out<ncm::RightsId> out_rights_id, ContentId content_id, fs::ContentAttributes attr);
            Result WriteContentForDebug(ContentId content_id, s64 offset, const sf::InBuffer &data);
            Result GetFreeSpaceSize(sf::Out<s64> out_size);
            Result GetTotalSpaceSize(sf::Out<s64> out_size);
            Result FlushPlaceHolder();
            Result GetSizeFromPlaceHolderId(sf::Out<s64> out, PlaceHolderId placeholder_id);
            Result RepairInvalidFileAttribute();
            Result GetRightsIdFromPlaceHolderIdWithCache(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id, ContentId cache_content_id, fs::ContentAttributes attr);
            Result RegisterPath(const ContentId &content_id, const Path &path);
            Result ClearRegisteredPath();
            Result GetProgramId(sf::Out<ncm::ProgramId> out, ContentId content_id, fs::ContentAttributes attr);

            Result CreatePlaceHolder_AtmosphereAlignmentFix(ContentId content_id, PlaceHolderId placeholder_id, s64 size) { R_RETURN(this->CreatePlaceHolder(placeholder_id, content_id, size)); }
            Result Register_AtmosphereAlignmentFix(ContentId content_id, PlaceHolderId placeholder_id) { R_RETURN(this->Register(placeholder_id, content_id)); }
            Result RevertToPlaceHolder_AtmosphereAlignmentFix(ncm::ContentId old_content_id, ncm::ContentId new_content_id, ncm::PlaceHolderId placeholder_id) { R_RETURN(this->RevertToPlaceHolder(placeholder_id, old_content_id, new_content_id)); }
            Result GetRightsIdFromPlaceHolderIdWithCacheDeprecated(sf::Out<ncm::RightsId> out_rights_id, ContentId cache_content_id, PlaceHolderId placeholder_id) { R_RETURN(this->GetRightsIdFromPlaceHolderIdWithCache(out_rights_id, placeholder_id, cache_content_id, fs::ContentAttributes_None)); }
    };
    static_assert(ncm::IsIContentStorage<UsbOverlayContentStorageImpl>);

}
