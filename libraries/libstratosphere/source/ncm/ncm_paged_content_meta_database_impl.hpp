/*
 * Copyright (c) Atmosphère-NX
 * This program is free software under version 2 of the GNU General Public License.
 */
#pragma once
#include <stratosphere.hpp>
#include <stratosphere/ncm/ncm_paged_content_meta_store.hpp>
#include "ncm_content_meta_database_impl_base.hpp"
#include "ncm_content_meta_range_reader.hpp"

namespace ams::ncm {

    class PagedContentMetaDatabaseImpl : public ContentMetaDatabaseImplBase {
        private:
            using RecordRef = PagedContentMetaStore::RecordRef;
            using Reader = impl::ContentMetaRangeReader<PagedContentMetaStore>;
            static constexpr s32 MaxPendingUsbTitleIds = 32;
            os::SdkRecursiveMutex m_mutex{};
            PagedContentMetaStore m_store;
            StorageId m_storage_id;
            ContentMetaKey m_pending_usb_title_ids[MaxPendingUsbTitleIds]{};
            s32 m_pending_usb_title_id_count = 0;
            ContentMetaKey m_pending_stock_title_ids[MaxPendingUsbTitleIds]{};
            s32 m_pending_stock_title_id_count = 0;

            Result EnsureAvailable() const;
            Result FindRecord(RecordRef *out, const ContentMetaKey &key);
            Result OpenReader(Reader *out, const ContentMetaKey &key);
            Result GetApplicationId(util::optional<ApplicationId> *out, const RecordRef &record);
            Result GetContentInfoImpl(ContentInfo *out, const ContentMetaKey &key, ContentType type, util::optional<u8> id_offset);
            Result GetRegisteredUsbIds(ContentId *meta, ContentId *control, ContentId *program, const ContentMetaKey &key);
        public:
            explicit PagedContentMetaDatabaseImpl(StorageId storage_id) : ContentMetaDatabaseImplBase(nullptr), m_storage_id(storage_id) { /* ... */ }
            Result Initialize(const char *root, const char *mount, fs::SaveDataSpaceId space, u64 save_id);

            virtual Result Set(const ContentMetaKey &key, const sf::InBuffer &value) override;
            virtual Result Get(sf::Out<u64> out_size, const ContentMetaKey &key, const sf::OutBuffer &out_value) override;
            virtual Result Remove(const ContentMetaKey &key) override;
            virtual Result GetContentIdByType(sf::Out<ContentId> out_content_id, const ContentMetaKey &key, ContentType type) override;
            virtual Result ListContentInfo(sf::Out<s32> out_entries_written, const sf::OutArray<ContentInfo> &out_info, const ContentMetaKey &key, s32 offset) override;
            virtual Result List(sf::Out<s32> out_entries_total, sf::Out<s32> out_entries_written, const sf::OutArray<ContentMetaKey> &out_info, ContentMetaType meta_type, ApplicationId application_id, u64 min, u64 max, ContentInstallType install_type) override;
            virtual Result GetLatestContentMetaKey(sf::Out<ContentMetaKey> out_key, u64 id) override;
            virtual Result ListApplication(sf::Out<s32> out_entries_total, sf::Out<s32> out_entries_written, const sf::OutArray<ApplicationContentMetaKey> &out_keys, ContentMetaType meta_type) override;
            virtual Result Has(sf::Out<bool> out, const ContentMetaKey &key) override;
            virtual Result HasAll(sf::Out<bool> out, const sf::InArray<ContentMetaKey> &keys) override;
            virtual Result GetSize(sf::Out<u64> out_size, const ContentMetaKey &key) override;
            virtual Result GetRequiredSystemVersion(sf::Out<u32> out_version, const ContentMetaKey &key) override;
            virtual Result GetPatchContentMetaId(sf::Out<u64> out_patch_id, const ContentMetaKey &key) override;
            virtual Result DisableForcibly() override;
            virtual Result LookupOrphanContent(const sf::OutArray<bool> &out_orphaned, const sf::InArray<ContentId> &content_ids) override;
            virtual Result Commit() override;
            virtual Result HasContent(sf::Out<bool> out, const ContentMetaKey &key, const ContentId &content_id) override;
            virtual Result ListContentMetaInfo(sf::Out<s32> out_entries_written, const sf::OutArray<ContentMetaInfo> &out_meta_info, const ContentMetaKey &key, s32 offset) override;
            virtual Result GetAttributes(sf::Out<u8> out_attributes, const ContentMetaKey &key) override;
            virtual Result GetRequiredApplicationVersion(sf::Out<u32> out_version, const ContentMetaKey &key) override;
            virtual Result GetContentIdByTypeAndIdOffset(sf::Out<ContentId> out_content_id, const ContentMetaKey &key, ContentType type, u8 id_offset) override;
            virtual Result GetCount(sf::Out<u32> out_count) override;
            virtual Result GetOwnerApplicationId(sf::Out<ApplicationId> out_id, const ContentMetaKey &key) override;
            virtual Result GetContentAccessibilities(sf::Out<u8> out_accessibilities, const ContentMetaKey &key) override;
            virtual Result GetContentInfoByType(sf::Out<ContentInfo> out_content_info, const ContentMetaKey &key, ContentType type) override;
            virtual Result GetContentInfoByTypeAndIdOffset(sf::Out<ContentInfo> out_content_info, const ContentMetaKey &key, ContentType type, u8 id_offset) override;
            virtual Result GetPlatform(sf::Out<ncm::ContentMetaPlatform> out, const ContentMetaKey &key) override;
            virtual Result HasAttributes(sf::Out<u8> out, u8 attr_mask) override;
    };
    static_assert(ncm::IsIContentMetaDatabase<PagedContentMetaDatabaseImpl>);
    static_assert(sizeof(PagedContentMetaDatabaseImpl) < 64 * 1024);
}
