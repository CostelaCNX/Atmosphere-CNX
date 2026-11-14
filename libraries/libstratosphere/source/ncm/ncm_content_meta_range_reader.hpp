/*
 * Copyright (c) Atmosphère-NX
 * This program is free software under version 2 of the GNU General Public License.
 */
#pragma once
#include <stratosphere/ncm/ncm_content_meta.hpp>

namespace ams::ncm::impl {

    /* A checked, copying reader: no caller retains pointers into the store's
     * evictable cache, and no record-sized allocation is needed. */
    template<typename Store>
    class ContentMetaRangeReader {
        private:
            Store &m_store;
            typename Store::RecordRef m_record{};
            ContentMetaHeader m_header{};
            u64 m_content_offset = 0;
            u64 m_meta_offset = 0;
        private:
            Result ReadRange(void *out, u64 offset, size_t size) {
                R_UNLESS(offset <= m_record.size && size <= m_record.size - offset,
                    ncm::ResultInvalidContentMetaFileSize());
                R_RETURN(m_store.Read(m_record, offset, out, size));
            }
        public:
            explicit ContentMetaRangeReader(Store &store) : m_store(store) { /* ... */ }

            Result Initialize(const typename Store::RecordRef &record) {
                R_UNLESS(record.size >= sizeof(ContentMetaHeader), ncm::ResultInvalidContentMetaFileSize());
                m_record = record;
                R_TRY(m_store.Read(m_record, 0, std::addressof(m_header), sizeof(m_header)));
                m_content_offset = sizeof(m_header) + static_cast<u64>(m_header.extended_header_size);
                m_meta_offset = m_content_offset + static_cast<u64>(m_header.content_count) * sizeof(ContentInfo);
                /* Validate only bytes actually read. Older homebrew forwarders
                 * declared a dependency that was never stored. Their header,
                 * extended fields and content infos remain valid and must not
                 * fail merely because an unrelated dependency tail is absent. */
                R_SUCCEED();
            }

            const ContentMetaHeader &GetHeader() const { return m_header; }
            size_t GetContentCount() const { return m_header.content_count; }
            size_t GetContentMetaCount() const { return m_header.content_meta_count; }
            size_t GetExtendedHeaderSize() const { return m_header.extended_header_size; }

            Result ReadExtendedField(void *out, size_t offset, size_t size) {
                R_UNLESS(offset <= m_header.extended_header_size && size <= m_header.extended_header_size - offset,
                    ncm::ResultInvalidContentMetaFileSize());
                R_RETURN(this->ReadRange(out, sizeof(ContentMetaHeader) + offset, size));
            }

            template<typename T>
            Result ReadExtended(T *out) {
                R_RETURN(this->ReadExtendedField(out, 0, sizeof(T)));
            }

            Result ReadContentInfo(ContentInfo *out, size_t index) {
                R_UNLESS(index < this->GetContentCount(), ncm::ResultInvalidOffset());
                R_RETURN(this->ReadRange(out, m_content_offset + static_cast<u64>(index) * sizeof(ContentInfo), sizeof(*out)));
            }

            Result ReadContentMetaInfo(ContentMetaInfo *out, size_t index) {
                R_UNLESS(index < this->GetContentMetaCount(), ncm::ResultInvalidOffset());
                R_RETURN(this->ReadRange(out, m_meta_offset + static_cast<u64>(index) * sizeof(ContentMetaInfo), sizeof(*out)));
            }

            Result FindContentInfo(ContentInfo *out, ContentType type, util::optional<u8> id_offset) {
                bool found = false;
                for (size_t i = 0; i < this->GetContentCount(); ++i) {
                    ContentInfo info{};
                    R_TRY(this->ReadContentInfo(std::addressof(info), i));
                    if (info.GetType() != type) continue;
                    if (id_offset) {
                        if (info.GetIdOffset() == *id_offset) { *out = info; R_SUCCEED(); }
                    } else if (!found || info.GetIdOffset() < out->GetIdOffset()) {
                        *out = info;
                        found = true;
                    }
                }
                R_UNLESS(found, ncm::ResultContentNotFound());
                R_SUCCEED();
            }

            Result GetApplicationId(util::optional<ApplicationId> *out, const ContentMetaKey &key) {
                *out = util::nullopt;
                switch (key.type) {
                    case ContentMetaType::Application: *out = ApplicationId{key.id}; break;
                    case ContentMetaType::Patch:
                    case ContentMetaType::AddOnContent:
                    case ContentMetaType::Delta: {
                        ApplicationId owner{};
                        R_TRY(this->ReadExtended(std::addressof(owner)));
                        *out = owner;
                        break;
                    }
                    default: break;
                }
                R_SUCCEED();
            }
    };
}
