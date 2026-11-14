/*
 * Copyright (c) Atmosphère-NX
 * This program is free software under version 2 of the GNU General Public License.
 */
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include <stratosphere/ncm/ncm_usb_title_map.hpp>
#include "ncm_paged_content_meta_database_impl.hpp"
#include "ncm_usb_metadata_mapping.hpp"
#include "ncm_lookup_trace.hpp"

namespace ams::ncm {

    Result PagedContentMetaDatabaseImpl::Initialize(const char *root, const char *mount, fs::SaveDataSpaceId space, u64 save_id) {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(std::strlen(mount) <= fs::MountNameLengthMax, fs::ResultInvalidArgument());
        std::strcpy(m_mount_name, mount);
        R_RETURN(m_store.Initialize(root, mount, space, save_id));
    }

    Result PagedContentMetaDatabaseImpl::EnsureAvailable() const {
        R_UNLESS(!m_disabled && m_store.IsAvailable(), ncm::ResultInvalidContentMetaDatabase());
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::FindRecord(RecordRef *out, const ContentMetaKey &key) {
        bool found = false;
        R_TRY(m_store.Find(out, std::addressof(found), key));
        R_UNLESS(found, ncm::ResultContentMetaNotFound());
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::OpenReader(Reader *out, const ContentMetaKey &key) {
        RecordRef record{};
        R_TRY(this->FindRecord(std::addressof(record), key));
        R_RETURN(out->Initialize(record));
    }

    Result PagedContentMetaDatabaseImpl::GetApplicationId(util::optional<ApplicationId> *out, const RecordRef &record) {
        /* These cases depend only on the key, just as the legacy reader did. */
        *out = util::nullopt;
        if (record.key.type == ContentMetaType::Application) {
            *out = ApplicationId{record.key.id};
            R_SUCCEED();
        }
        if (record.key.type != ContentMetaType::Patch && record.key.type != ContentMetaType::AddOnContent
            && record.key.type != ContentMetaType::Delta) R_SUCCEED();
        Reader reader(m_store);
        R_TRY(reader.Initialize(record));
        R_RETURN(reader.GetApplicationId(out, record.key));
    }

    Result PagedContentMetaDatabaseImpl::GetContentInfoImpl(ContentInfo *out, const ContentMetaKey &key, ContentType type, util::optional<u8> id_offset) {
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureAvailable());
            u32 index = 0;
            R_TRY(m_store.LowerBound(std::addressof(index), key));
            R_UNLESS(index < m_store.GetCount(), ncm::ResultContentMetaNotFound());
            RecordRef record{};
            R_TRY(m_store.At(std::addressof(record), index));
            R_UNLESS(record.key.id == key.id, ncm::ResultContentMetaNotFound());
            Reader reader(m_store);
            R_TRY(reader.Initialize(record));
            R_RETURN(reader.FindContentInfo(out, type, id_offset));
        }();
        ContentId content_id{};
        if (R_SUCCEEDED(result)) content_id = out->GetId();
        lookup_trace::Metadata(usb_recovery::LookupOperation::MetadataContentInfo, m_storage_id,
            key.id, key.version, type, id_offset, result, R_SUCCEEDED(result) ? std::addressof(content_id) : nullptr);
        R_RETURN(result);
    }

    Result PagedContentMetaDatabaseImpl::Set(const ContentMetaKey &key, const sf::InBuffer &value) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        const bool is_sd_application = m_storage_id == StorageId::SdCard
            && key.type == ContentMetaType::Application;
        // A mounted USB drive does not identify this transaction's target.
        // Only content registered by an explicit USB handle can create a map.
        bool queue_usb_mapping = false;
        if (is_sd_application) {
            ContentId meta{}, control{}, program{};
            queue_usb_mapping = impl::GetRegisteredUsbApplicationIds(std::addressof(meta), std::addressof(control),
                std::addressof(program), value.GetPointer(), value.GetSize(), true);
        }
        bool preserve_usb_mapping = false;
        if (is_sd_application && !queue_usb_mapping
            && impl::HasSameRegisteredUsbApplicationMap(ProgramId{key.id}, value.GetPointer(), value.GetSize())) {
            RecordRef previous{};
            bool found = false;
            R_TRY(m_store.Find(std::addressof(previous), std::addressof(found), key));
            preserve_usb_mapping = found && previous.size == value.GetSize();
            u8 buffer[256];
            for (size_t offset = 0; preserve_usb_mapping && offset < value.GetSize(); offset += sizeof(buffer)) {
                const size_t count = std::min(sizeof(buffer), value.GetSize() - offset);
                R_TRY(m_store.Read(previous, offset, buffer, count));
                preserve_usb_mapping = std::memcmp(buffer, static_cast<const u8 *>(value.GetPointer()) + offset, count) == 0;
            }
        }
        const bool queue_stock_unmap = is_sd_application && !queue_usb_mapping && !preserve_usb_mapping;
        s32 usb_pending_index = 0;
        if (queue_usb_mapping) {
            for (; usb_pending_index < m_pending_usb_title_id_count; ++usb_pending_index) {
                if (m_pending_usb_title_ids[usb_pending_index] == key) break;
            }
            R_UNLESS(usb_pending_index < MaxPendingUsbTitleIds, ncm::ResultBufferInsufficient());
        }
        s32 stock_pending_index = 0;
        if (queue_stock_unmap) {
            for (; stock_pending_index < m_pending_stock_title_id_count; ++stock_pending_index) {
                if (m_pending_stock_title_ids[stock_pending_index] == key) break;
            }
            R_UNLESS(stock_pending_index < MaxPendingUsbTitleIds, ncm::ResultBufferInsufficient());
        }
        R_TRY(m_store.Set(key, value.GetPointer(), value.GetSize()));
        // Repeated Set on the same key is one transaction: the latest
        // successful destination replaces any earlier pending decision.
        if (queue_stock_unmap || preserve_usb_mapping) {
            for (s32 i = 0; i < m_pending_usb_title_id_count; ++i) {
                if (m_pending_usb_title_ids[i] == key) {
                    m_pending_usb_title_ids[i] = m_pending_usb_title_ids[--m_pending_usb_title_id_count];
                    break;
                }
            }
        }
        if (queue_usb_mapping || preserve_usb_mapping) {
            for (s32 i = 0; i < m_pending_stock_title_id_count; ++i) {
                if (m_pending_stock_title_ids[i] == key) {
                    m_pending_stock_title_ids[i] = m_pending_stock_title_ids[--m_pending_stock_title_id_count];
                    break;
                }
            }
        }

        if (queue_usb_mapping && usb_pending_index == m_pending_usb_title_id_count)
            m_pending_usb_title_ids[m_pending_usb_title_id_count++] = key;
        if (queue_stock_unmap && stock_pending_index == m_pending_stock_title_id_count)
            m_pending_stock_title_ids[m_pending_stock_title_id_count++] = key;
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::Get(sf::Out<u64> out_size, const ContentMetaKey &key, const sf::OutBuffer &out_value) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        RecordRef record{};
        R_TRY(this->FindRecord(std::addressof(record), key));
        const size_t size = static_cast<size_t>(std::min(record.size, static_cast<u64>(out_value.GetSize())));
        R_TRY(m_store.Read(record, 0, out_value.GetPointer(), size));
        out_size.SetValue(size);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::Remove(const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        R_TRY_CATCH(m_store.Remove(key)) {
            R_CONVERT(kvdb::ResultKeyNotFound, ncm::ResultContentMetaNotFound())
        } R_END_TRY_CATCH;
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetContentIdByType(sf::Out<ContentId> out, const ContentMetaKey &key, ContentType type) {
        std::scoped_lock lk(m_mutex);
        ContentInfo info{};
        R_TRY(this->GetContentInfoImpl(std::addressof(info), key, type, util::nullopt));
        out.SetValue(info.GetId());
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::ListContentInfo(sf::Out<s32> out_count, const sf::OutArray<ContentInfo> &out_info, const ContentMetaKey &key, s32 offset) {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(offset >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureAvailable());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        size_t count = 0;
        for (; count < out_info.GetSize() && count + static_cast<size_t>(offset) < reader.GetContentCount(); ++count)
            R_TRY(reader.ReadContentInfo(std::addressof(out_info[count]), count + static_cast<size_t>(offset)));
        out_count.SetValue(static_cast<s32>(count));
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::List(sf::Out<s32> out_total, sf::Out<s32> out_written, const sf::OutArray<ContentMetaKey> &out_info, ContentMetaType type, ApplicationId application_id, u64 min, u64 max, ContentInstallType install_type) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        s32 total = 0;
        s32 written = 0;
        for (u32 i = 0; i < m_store.GetCount(); ++i) {
            RecordRef record{};
            R_TRY(m_store.At(std::addressof(record), i));
            const auto key = record.key;
            if (!((type == ContentMetaType::Unknown || key.type == type) && min <= key.id && key.id <= max
                && (install_type == ContentInstallType::Unknown || key.install_type == install_type))) continue;
            if (application_id != InvalidApplicationId) {
                util::optional<ApplicationId> owner;
                R_TRY(this->GetApplicationId(std::addressof(owner), record));
                if (owner && *owner != application_id) continue;
            }
            R_UNLESS(total < INT32_MAX, ncm::ResultBufferInsufficient());
            if (static_cast<size_t>(written) < out_info.GetSize()) out_info[written++] = key;
            ++total;
        }
        out_total.SetValue(total);
        out_written.SetValue(written);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetLatestContentMetaKey(sf::Out<ContentMetaKey> out_key, u64 id) {
        std::scoped_lock lk(m_mutex);
        const Result result = [&]() -> Result {
            R_TRY(this->EnsureAvailable());
            u32 index = 0;
            R_TRY(m_store.LowerBound(std::addressof(index), ContentMetaKey::MakeUnknownType(id, 0)));
            util::optional<ContentMetaKey> latest;
            for (; index < m_store.GetCount(); ++index) {
                RecordRef record{};
                R_TRY(m_store.At(std::addressof(record), index));
                if (record.key.id != id) break;
                if (record.key.install_type == ContentInstallType::Full) latest = record.key;
            }
            R_UNLESS(latest, ncm::ResultContentMetaNotFound());
            out_key.SetValue(*latest);
            R_SUCCEED();
        }();
        lookup_trace::Metadata(usb_recovery::LookupOperation::MetadataLatestKey, m_storage_id,
            id, R_SUCCEEDED(result) ? out_key.GetPointer()->version : 0, ContentType::Meta, util::nullopt, result);
        R_RETURN(result);
    }

    Result PagedContentMetaDatabaseImpl::ListApplication(sf::Out<s32> out_total, sf::Out<s32> out_written, const sf::OutArray<ApplicationContentMetaKey> &out_keys, ContentMetaType type) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        s32 total = 0;
        s32 written = 0;
        for (u32 i = 0; i < m_store.GetCount(); ++i) {
            RecordRef record{};
            R_TRY(m_store.At(std::addressof(record), i));
            if (type != ContentMetaType::Unknown && type != record.key.type) continue;
            util::optional<ApplicationId> owner;
            R_TRY(this->GetApplicationId(std::addressof(owner), record));
            if (!owner) continue;
            R_UNLESS(total < INT32_MAX, ncm::ResultBufferInsufficient());
            if (static_cast<size_t>(written) < out_keys.GetSize()) out_keys[written++] = {record.key, *owner};
            ++total;
        }
        out_total.SetValue(total);
        out_written.SetValue(written);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::Has(sf::Out<bool> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        RecordRef record{};
        bool found = false;
        R_TRY(m_store.Find(std::addressof(record), std::addressof(found), key));
        out.SetValue(found);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::HasAll(sf::Out<bool> out, const sf::InArray<ContentMetaKey> &keys) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        out.SetValue(false);
        for (size_t i = 0; i < keys.GetSize(); ++i) {
            bool found = false;
            R_TRY(this->Has(std::addressof(found), keys[i]));
            R_SUCCEED_IF(!found);
        }
        out.SetValue(true);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetSize(sf::Out<u64> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        RecordRef record{};
        R_TRY(this->FindRecord(std::addressof(record), key));
        out.SetValue(record.size);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetRequiredSystemVersion(sf::Out<u32> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        R_UNLESS(key.type == ContentMetaType::Application || key.type == ContentMetaType::Patch, ncm::ResultInvalidContentMetaKey());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        const size_t offset = key.type == ContentMetaType::Application
            ? AMS_OFFSETOF(ApplicationMetaExtendedHeader, required_system_version)
            : AMS_OFFSETOF(PatchMetaExtendedHeader, required_system_version);
        u32 version = 0;
        R_TRY(reader.ReadExtendedField(std::addressof(version), offset, sizeof(version)));
        out.SetValue(version);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetPatchContentMetaId(sf::Out<u64> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        R_UNLESS(key.type == ContentMetaType::Application || key.type == ContentMetaType::AddOnContent, ncm::ResultInvalidContentMetaKey());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        size_t offset = AMS_OFFSETOF(ApplicationMetaExtendedHeader, patch_id);
        if (key.type == ContentMetaType::AddOnContent) {
            R_UNLESS(reader.GetExtendedHeaderSize() == sizeof(AddOnContentMetaExtendedHeader), ncm::ResultInvalidAddOnContentMetaExtendedHeader());
            offset = AMS_OFFSETOF(AddOnContentMetaExtendedHeader, data_patch_id);
        }
        u64 patch_id = 0;
        R_TRY(reader.ReadExtendedField(std::addressof(patch_id), offset, sizeof(patch_id)));
        out.SetValue(patch_id);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::DisableForcibly() {
        std::scoped_lock lk(m_mutex);
        m_disabled = true;
        m_store.Invalidate();
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::LookupOrphanContent(const sf::OutArray<bool> &out, const sf::InArray<ContentId> &ids) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        R_UNLESS(out.GetSize() >= ids.GetSize(), ncm::ResultBufferInsufficient());
        for (size_t i = 0; i < out.GetSize(); ++i) out[i] = true;
        for (u32 i = 0; i < m_store.GetCount(); ++i) {
            RecordRef record{};
            R_TRY(m_store.At(std::addressof(record), i));
            Reader reader(m_store);
            R_TRY(reader.Initialize(record));
            for (size_t j = 0; j < reader.GetContentCount(); ++j) {
                ContentInfo info{};
                R_TRY(reader.ReadContentInfo(std::addressof(info), j));
                for (size_t n = 0; n < ids.GetSize(); ++n) {
                    if (ids[n] == info.GetId()) { out[n] = false; break; }
                }
            }
        }
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetRegisteredUsbIds(ContentId *meta, ContentId *control, ContentId *program, const ContentMetaKey &key) {
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        R_UNLESS(reader.GetContentCount() != 0, ncm::ResultContentNotFound());
        *meta = *control = *program = InvalidContentId;
        u16 control_offset = 256, program_offset = 256;
        constexpr bool require_locations = true;
        bool registered_this_boot = false;
        for (size_t i = 0; i < reader.GetContentCount(); ++i) {
            ContentInfo info{};
            R_TRY(reader.ReadContentInfo(std::addressof(info), i));
            R_UNLESS(info.GetId() != InvalidContentId && (!require_locations || IsUsbContentLocationRegistered(info.GetId())), ncm::ResultContentNotFound());
            if (require_locations) registered_this_boot |= WasUsbContentRegisteredThisBoot(info.GetId());
            switch (info.GetType()) {
                case ContentType::Meta: *meta = info.GetId(); break;
                case ContentType::Control:
                    if (info.GetIdOffset() < control_offset) { control_offset = info.GetIdOffset(); *control = info.GetId(); }
                    break;
                case ContentType::Program:
                    if (info.GetIdOffset() < program_offset) { program_offset = info.GetIdOffset(); *program = info.GetId(); }
                    break;
                default: break;
            }
        }
        R_UNLESS((!require_locations || registered_this_boot) && *meta != InvalidContentId && *control != InvalidContentId && *program != InvalidContentId, ncm::ResultContentNotFound());
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::Commit() {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        /* The store owns both archive publication and CommitSaveData. Keep USB
         * title-map publication strictly after that durable transaction. */
        R_TRY(m_store.Commit());
        for (s32 i = 0; i < m_pending_usb_title_id_count; ++i) {
            const auto key = m_pending_usb_title_ids[i];
            ContentId meta{}, control{}, program{};
            const Result result = this->GetRegisteredUsbIds(std::addressof(meta), std::addressof(control), std::addressof(program), key);
            if (ncm::ResultContentMetaNotFound::Includes(result)) continue;
            R_TRY(result);
            R_TRY(RegisterUsbTitleContent(ProgramId{key.id}, meta, control, program));
            R_TRY(RegisterUsbTitle(ProgramId{key.id}));
        }
        m_pending_usb_title_id_count = 0;
        for (s32 i = 0; i < m_pending_stock_title_id_count; ++i)
            R_TRY(UnregisterUsbTitle(ProgramId{m_pending_stock_title_ids[i].id}));
        m_pending_stock_title_id_count = 0;
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::HasContent(sf::Out<bool> out, const ContentMetaKey &key, const ContentId &id) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        out.SetValue(true);
        for (size_t i = 0; i < reader.GetContentCount(); ++i) {
            ContentInfo info{};
            R_TRY(reader.ReadContentInfo(std::addressof(info), i));
            R_SUCCEED_IF(info.GetId() == id);
        }
        out.SetValue(false);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::ListContentMetaInfo(sf::Out<s32> out_count, const sf::OutArray<ContentMetaInfo> &out_info, const ContentMetaKey &key, s32 offset) {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(offset >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureAvailable());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        size_t count = 0;
        for (; count < out_info.GetSize() && count + static_cast<size_t>(offset) < reader.GetContentMetaCount(); ++count)
            R_TRY(reader.ReadContentMetaInfo(std::addressof(out_info[count]), count + static_cast<size_t>(offset)));
        out_count.SetValue(static_cast<s32>(count));
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetAttributes(sf::Out<u8> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        out.SetValue(reader.GetHeader().attributes);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetRequiredApplicationVersion(sf::Out<u32> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        R_UNLESS(key.type == ContentMetaType::Application || key.type == ContentMetaType::AddOnContent, ncm::ResultInvalidContentMetaKey());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        if (key.type == ContentMetaType::Application) {
            R_UNLESS(hos::GetVersion() >= hos::Version_9_0_0, ncm::ResultNotSupported());
        }
        const size_t offset = key.type == ContentMetaType::Application
            ? AMS_OFFSETOF(ApplicationMetaExtendedHeader, required_application_version)
            : AMS_OFFSETOF(LegacyAddOnContentMetaExtendedHeader, required_application_version);
        u32 version = 0;
        R_TRY(reader.ReadExtendedField(std::addressof(version), offset, sizeof(version)));
        out.SetValue(version);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetContentIdByTypeAndIdOffset(sf::Out<ContentId> out, const ContentMetaKey &key, ContentType type, u8 id_offset) {
        std::scoped_lock lk(m_mutex);
        ContentInfo info{};
        R_TRY(this->GetContentInfoImpl(std::addressof(info), key, type, util::make_optional(id_offset)));
        out.SetValue(info.GetId());
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetCount(sf::Out<u32> out) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        out.SetValue(m_store.GetCount());
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetOwnerApplicationId(sf::Out<ApplicationId> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        R_UNLESS(key.type == ContentMetaType::Application || key.type == ContentMetaType::Patch || key.type == ContentMetaType::AddOnContent, ncm::ResultInvalidContentMetaKey());
        if (key.type == ContentMetaType::Application) { out.SetValue({key.id}); R_SUCCEED(); }
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        ApplicationId owner{};
        R_TRY(reader.ReadExtended(std::addressof(owner)));
        out.SetValue(owner);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetContentAccessibilities(sf::Out<u8> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        R_UNLESS(key.type == ContentMetaType::AddOnContent, ncm::ResultInvalidContentMetaKey());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        u8 accessibility = 0;
        R_TRY(reader.ReadExtendedField(std::addressof(accessibility), AMS_OFFSETOF(AddOnContentMetaExtendedHeader, content_accessibilities), sizeof(accessibility)));
        out.SetValue(accessibility);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::GetContentInfoByType(sf::Out<ContentInfo> out, const ContentMetaKey &key, ContentType type) {
        std::scoped_lock lk(m_mutex);
        R_RETURN(this->GetContentInfoImpl(out.GetPointer(), key, type, util::nullopt));
    }

    Result PagedContentMetaDatabaseImpl::GetContentInfoByTypeAndIdOffset(sf::Out<ContentInfo> out, const ContentMetaKey &key, ContentType type, u8 id_offset) {
        std::scoped_lock lk(m_mutex);
        R_RETURN(this->GetContentInfoImpl(out.GetPointer(), key, type, util::make_optional(id_offset)));
    }

    Result PagedContentMetaDatabaseImpl::GetPlatform(sf::Out<ncm::ContentMetaPlatform> out, const ContentMetaKey &key) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        Reader reader(m_store);
        R_TRY(this->OpenReader(std::addressof(reader), key));
        out.SetValue(reader.GetHeader().platform);
        R_SUCCEED();
    }

    Result PagedContentMetaDatabaseImpl::HasAttributes(sf::Out<u8> out, u8 attribute_mask) {
        std::scoped_lock lk(m_mutex);
        R_TRY(this->EnsureAvailable());
        u8 combined = 0;
        for (u32 i = 0; i < m_store.GetCount(); ++i) {
            RecordRef record{};
            R_TRY(m_store.At(std::addressof(record), i));
            Reader reader(m_store);
            R_TRY(reader.Initialize(record));
            combined |= reader.GetHeader().attributes & attribute_mask;
            if ((combined & attribute_mask) == attribute_mask) break;
        }
        out.SetValue(combined);
        R_SUCCEED();
    }
}
