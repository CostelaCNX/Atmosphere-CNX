/*
 * Copyright (c) Atmosphère-NX
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stratosphere.hpp>
#include <stratosphere/ncm/ncm_paged_content_meta_store.hpp>

namespace ams::fs::impl {
    /* Existing, space-aware implementation in fs_save_data_management.cpp. */
    Result ReadSaveDataFileSystemExtraData(SaveDataExtraData *out, SaveDataSpaceId space_id, SaveDataId id);
}

namespace ams::ncm {

    namespace {
        constexpr u64 GrowthAlignment = 0x4000;
        constexpr u64 GrowthHeadroom = 0x100000;
        constexpr u64 MaxFileSize = static_cast<u64>(INT64_MAX);

        bool Fits(u64 offset, u64 size, u64 total) {
            return offset <= total && size <= total - offset;
        }
    }

    PagedContentMetaStore::~PagedContentMetaStore() { this->CloseFiles(); }

    void PagedContentMetaStore::ResetCaches() {
        m_index_cache_size = m_value_cache_size = 0;
    }

    void PagedContentMetaStore::CloseFiles() {
        if (m_base_open) fs::CloseFile(m_base_file);
        if (m_index_open) fs::CloseFile(m_index_file);
        if (m_values_open) fs::CloseFile(m_values_file);
        m_base_open = m_index_open = m_values_open = false;
        this->ResetCaches();
    }

    void PagedContentMetaStore::Invalidate() {
        this->CloseFiles();
        m_available = false;
        ++m_generation;
        m_unavailable_result = ncm::ResultInvalidContentMetaDatabase();
    }

    Result PagedContentMetaStore::MakeUnavailable(Result result) {
        this->Invalidate();
        m_unavailable_result = result;
        R_RETURN(result);
    }

    Result PagedContentMetaStore::EnsureAvailable() const {
        R_UNLESS(m_available, R_FAILED(m_unavailable_result) ? m_unavailable_result : ncm::ResultInvalidContentMetaDatabase());
        R_SUCCEED();
    }

    Result PagedContentMetaStore::EnsureMutable() const {
        R_TRY(this->EnsureAvailable());
        /* A failed explicit save-data commit must be retried explicitly. In
         * particular, a later growth checkpoint cannot publish it implicitly. */
        R_UNLESS(!m_commit_prepared, R_FAILED(m_pending_commit_result) ? m_pending_commit_result : ncm::ResultInvalidContentMetaDatabase());
        R_SUCCEED();
    }

    Result PagedContentMetaStore::ReadBytes(fs::FileHandle file, u64 offset, void *buffer, size_t size) {
        R_UNLESS(Fits(offset, size, MaxFileSize), kvdb::ResultInvalidKeyValue());
        auto *out = static_cast<u8 *>(buffer);
        while (size != 0) {
            const size_t chunk = std::min(size, size_t{0x10000});
            size_t actual = 0;
            R_TRY(fs::ReadFile(std::addressof(actual), file, static_cast<s64>(offset), out, chunk));
            R_UNLESS(actual == chunk, kvdb::ResultInvalidKeyValue());
            offset += chunk;
            out += chunk;
            size -= chunk;
        }
        R_SUCCEED();
    }

    Result PagedContentMetaStore::WriteBytes(fs::FileHandle file, u64 offset, const void *buffer, size_t size) {
        R_UNLESS(Fits(offset, size, MaxFileSize), kvdb::ResultInvalidKeyValue());
        const auto *in = static_cast<const u8 *>(buffer);
        while (size != 0) {
            const size_t chunk = std::min(size, size_t{0x10000});
            R_TRY(fs::WriteFile(file, static_cast<s64>(offset), in, chunk, fs::WriteOption::None));
            offset += chunk;
            in += chunk;
            size -= chunk;
        }
        R_SUCCEED();
    }

    Result PagedContentMetaStore::DeleteIfPresent(const char *path) {
        R_TRY_CATCH(fs::DeleteFile(path)) {
            R_CONVERT(fs::ResultPathNotFound, ResultSuccess());
        } R_END_TRY_CATCH;
        R_SUCCEED();
    }

    Result PagedContentMetaStore::ReplaceScratchFile(fs::FileHandle *out, const char *path, u64 size) {
        R_UNLESS(size <= MaxFileSize, kvdb::ResultInvalidKeyValue());
        R_TRY(this->DeleteIfPresent(path));
        R_TRY(fs::CreateFile(path, static_cast<s64>(size)));
        R_RETURN(fs::OpenFile(out, path, fs::OpenMode_ReadWrite));
    }

    Result PagedContentMetaStore::OpenFiles() {
        this->CloseFiles();
        const auto result = [&]() -> Result {
            Result base_result = fs::OpenFile(std::addressof(m_base_file), m_archive, fs::OpenMode_Read);
            if (R_SUCCEEDED(base_result)) {
                m_base_open = true;
            } else {
                R_UNLESS(fs::ResultPathNotFound::Includes(base_result) && m_base_size == 0, base_result);
            }
            R_TRY(fs::OpenFile(std::addressof(m_index_file), m_index_paths[m_active_index], fs::OpenMode_Read));
            m_index_open = true;
            R_TRY(fs::OpenFile(std::addressof(m_values_file), m_values_path, fs::OpenMode_ReadWrite));
            m_values_open = true;
            R_SUCCEED();
        }();
        if (R_FAILED(result)) R_RETURN(this->MakeUnavailable(result));
        R_SUCCEED();
    }

    Result PagedContentMetaStore::EnsureSpace(u64 required_free_bytes) {
        R_UNLESS(required_free_bytes <= MaxFileSize - GrowthHeadroom - GrowthAlignment, fs::ResultNotEnoughFreeSpace());
        R_UNLESS(!m_commit_prepared, R_FAILED(m_pending_commit_result) ? m_pending_commit_result : ncm::ResultInvalidContentMetaDatabase());
        s64 free_bytes = 0;
        R_TRY(fs::GetFreeSpaceSize(std::addressof(free_bytes), m_root));
        R_UNLESS(free_bytes >= 0, kvdb::ResultInvalidKeyValue());
        R_SUCCEED_IF(static_cast<u64>(free_bytes) >= required_free_bytes);
        R_UNLESS(hos::GetVersion() >= hos::Version_3_0_0, fs::ResultNotEnoughFreeSpace());

        fs::SaveDataExtraData extra{};
        R_TRY(fs::impl::ReadSaveDataFileSystemExtraData(std::addressof(extra), m_space_id, m_save_id));
        R_UNLESS(extra.available_size > 0 && extra.journal_size >= 0, kvdb::ResultInvalidKeyValue());
        const u64 current = static_cast<u64>(extra.available_size);
        const u64 deficit = required_free_bytes - static_cast<u64>(free_bytes);
        R_UNLESS(current <= MaxFileSize - deficit - GrowthHeadroom - GrowthAlignment, fs::ResultNotEnoughFreeSpace());
        u64 target = current + deficit + GrowthHeadroom;
        if (current <= (MaxFileSize - GrowthAlignment) / 2) target = std::max(target, current * 2);
        target = (target + GrowthAlignment - 1) & ~(GrowthAlignment - 1);
        const u64 journal = std::max(target, static_cast<u64>(extra.journal_size));
        R_UNLESS(journal <= MaxFileSize, fs::ResultNotEnoughFreeSpace());

        /* All caller-visible Set/Remove changes are in scratch files only.
         * Checkpointing those permits an unmount/extend/remount even during a
         * dirty transaction without publishing changes to imkvdb.arc. Reboot
         * always rebuilds scratch from that unchanged canonical archive. */
        const bool base_was_open = m_base_open;
        const bool index_was_open = m_index_open;
        const bool values_was_open = m_values_open;
        this->CloseFiles();
        auto reopen = [&]() -> Result {
            if (base_was_open) {
                R_TRY(fs::OpenFile(std::addressof(m_base_file), m_archive, fs::OpenMode_Read));
                m_base_open = true;
            }
            if (index_was_open) {
                R_TRY(fs::OpenFile(std::addressof(m_index_file), m_index_paths[m_active_index], fs::OpenMode_Read));
                m_index_open = true;
            }
            if (values_was_open) {
                R_TRY(fs::OpenFile(std::addressof(m_values_file), m_values_path, fs::OpenMode_ReadWrite));
                m_values_open = true;
            }
            R_SUCCEED();
        };
        const Result checkpoint = fs::CommitSaveData(m_mount);
        if (R_FAILED(checkpoint)) {
            const Result reopened = reopen();
            if (R_FAILED(reopened)) R_RETURN(this->MakeUnavailable(reopened));
            R_RETURN(checkpoint);
        }
        fs::Unmount(m_mount);
        m_mounted = false;
        const Result extend = fs::ExtendSaveData(m_space_id, m_save_id, static_cast<s64>(target), static_cast<s64>(journal));
        const Result mounted = fs::MountSystemSaveData(m_mount, m_space_id, m_save_id);
        if (R_FAILED(mounted)) R_RETURN(this->MakeUnavailable(mounted));
        m_mounted = true;
        const Result reopened = reopen();
        if (R_FAILED(reopened)) R_RETURN(this->MakeUnavailable(reopened));
        R_TRY(extend);
        R_TRY(fs::GetFreeSpaceSize(std::addressof(free_bytes), m_root));
        R_UNLESS(free_bytes >= 0 && static_cast<u64>(free_bytes) >= required_free_bytes, fs::ResultNotEnoughFreeSpace());
        R_SUCCEED();
    }

    Result PagedContentMetaStore::ReadData(bool staged, u64 offset, void *buffer, size_t size) {
        const u64 file_size = staged ? m_values_size : m_base_size;
        R_UNLESS(Fits(offset, size, file_size), kvdb::ResultInvalidKeyValue());
        R_UNLESS(size == 0 || (staged ? m_values_open : m_base_open), kvdb::ResultInvalidKeyValue());
        auto *out = static_cast<u8 *>(buffer);
        while (size != 0) {
            const u64 page = offset & ~(static_cast<u64>(PageSize) - 1);
            if (m_value_cache_size == 0 || m_value_cache_staged != staged || m_value_cache_offset != page) {
                m_value_cache_size = 0;
                const size_t length = static_cast<size_t>(std::min(static_cast<u64>(PageSize), file_size - page));
                R_TRY(this->ReadBytes(staged ? m_values_file : m_base_file, page, m_value_cache, length));
                m_value_cache_offset = page;
                m_value_cache_staged = staged;
                m_value_cache_size = length;
            }
            const size_t within = static_cast<size_t>(offset - page);
            R_UNLESS(within < m_value_cache_size, kvdb::ResultInvalidKeyValue());
            const size_t part = std::min(size, m_value_cache_size - within);
            std::memcpy(out, m_value_cache + within, part);
            out += part;
            offset += part;
            size -= part;
        }
        R_SUCCEED();
    }

    Result PagedContentMetaStore::ReadIndex(IndexEntry *out, u32 index) {
        R_UNLESS(index < m_count && m_index_open, kvdb::ResultKeyNotFound());
        const u64 offset = static_cast<u64>(index) * sizeof(IndexEntry);
        const u64 page = offset & ~(static_cast<u64>(PageSize) - 1);
        if (m_index_cache_size == 0 || m_index_cache_offset != page) {
            m_index_cache_size = 0;
            const u64 bytes = static_cast<u64>(m_count) * sizeof(IndexEntry);
            const size_t size = static_cast<size_t>(std::min(static_cast<u64>(PageSize), bytes - page));
            R_TRY(this->ReadBytes(m_index_file, page, m_index_cache, size));
            m_index_cache_offset = page;
            m_index_cache_size = size;
        }
        const size_t within = static_cast<size_t>(offset - page);
        R_UNLESS(Fits(within, sizeof(*out), m_index_cache_size), kvdb::ResultInvalidKeyValue());
        std::memcpy(out, m_index_cache + within, sizeof(*out));
        R_UNLESS(out->staged <= 1 && Fits(out->offset, out->size, out->staged ? m_values_size : m_base_size), kvdb::ResultInvalidKeyValue());
        R_SUCCEED();
    }

    Result PagedContentMetaStore::At(RecordRef *out, u32 index) {
        R_TRY(this->EnsureAvailable());
        IndexEntry entry;
        R_TRY(this->ReadIndex(std::addressof(entry), index));
        *out = {entry.key, entry.offset, entry.size, m_generation, entry.staged != 0};
        R_SUCCEED();
    }

    Result PagedContentMetaStore::LowerBound(u32 *out_index, const ContentMetaKey &key) {
        R_TRY(this->EnsureAvailable());
        u32 low = 0, high = m_count;
        while (low < high) {
            const u32 mid = low + (high - low) / 2;
            IndexEntry entry;
            R_TRY(this->ReadIndex(std::addressof(entry), mid));
            if (entry.key < key) low = mid + 1;
            else high = mid;
        }
        *out_index = low;
        R_SUCCEED();
    }

    Result PagedContentMetaStore::Find(RecordRef *out, bool *found, const ContentMetaKey &key) {
        *found = false;
        u32 index;
        R_TRY(this->LowerBound(std::addressof(index), key));
        R_SUCCEED_IF(index == m_count);
        R_TRY(this->At(out, index));
        *found = out->key == key;
        R_SUCCEED();
    }

    Result PagedContentMetaStore::Read(const RecordRef &record, u64 offset, void *buffer, size_t size) {
        R_TRY(this->EnsureAvailable());
        R_UNLESS(record.generation == m_generation && Fits(offset, size, record.size), kvdb::ResultInvalidKeyValue());
        R_UNLESS(Fits(record.offset, record.size, record.staged ? m_values_size : m_base_size), kvdb::ResultInvalidKeyValue());
        R_RETURN(this->ReadData(record.staged, record.offset + offset, buffer, size));
    }

    Result PagedContentMetaStore::ImportArchive() {
        ArchiveHeader header{};
        if (m_base_open) {
            R_TRY(this->ReadData(false, 0, std::addressof(header), sizeof(header)));
            R_UNLESS(std::memcmp(header.magic, "IMKV", 4) == 0 && header.second <= INT32_MAX, kvdb::ResultInvalidKeyValue());
            /* Even an empty value needs an entry header and key. Reject an
             * impossible count before it can request save-data growth. */
            R_UNLESS(header.second <= (m_base_size - sizeof(ArchiveHeader)) / (sizeof(ArchiveHeader) + sizeof(ContentMetaKey)), kvdb::ResultInvalidKeyValue());
        }
        m_count = header.second;
        const u64 index_size = static_cast<u64>(m_count) * sizeof(IndexEntry);
        R_TRY(this->EnsureSpace(index_size + PageSize));
        fs::FileHandle file;
        R_TRY(this->ReplaceScratchFile(std::addressof(file), m_index_paths[0], index_size));
        ON_SCOPE_EXIT { fs::CloseFile(file); };
        u64 source = sizeof(ArchiveHeader);
        u64 destination = 0;
        size_t buffered = 0;
        ContentMetaKey previous{};
        for (u32 i = 0; i < m_count; ++i) {
            ArchiveHeader entry_header;
            R_TRY(this->ReadData(false, source, std::addressof(entry_header), sizeof(entry_header)));
            R_UNLESS(std::memcmp(entry_header.magic, "IMEN", 4) == 0 && entry_header.first == sizeof(ContentMetaKey), kvdb::ResultInvalidKeyValue());
            source += sizeof(entry_header);
            ContentMetaKey key;
            R_TRY(this->ReadData(false, source, std::addressof(key), sizeof(key)));
            R_UNLESS(i == 0 || previous < key, kvdb::ResultInvalidKeyValue());
            source += sizeof(key);
            R_UNLESS(Fits(source, entry_header.second, m_base_size), kvdb::ResultInvalidKeyValue());
            IndexEntry entry{key, source, entry_header.second, 0};
            std::memcpy(m_transfer + buffered, std::addressof(entry), sizeof(entry));
            buffered += sizeof(entry);
            if (buffered == sizeof(m_transfer)) {
                R_TRY(this->WriteBytes(file, destination, m_transfer, buffered));
                destination += buffered;
                buffered = 0;
            }
            source += entry_header.second;
            previous = key;
        }
        if (buffered != 0) R_TRY(this->WriteBytes(file, destination, m_transfer, buffered));
        R_TRY(fs::FlushFile(file));
        /* Legacy readers accept bytes after the declared entries. */
        m_archive_size = source;
        R_SUCCEED();
    }

    Result PagedContentMetaStore::Initialize(const char *root_path, const char *mount_name, fs::SaveDataSpaceId space_id, u64 save_id) {
        this->Invalidate();
        /* The manager owns the initial mount; growth may temporarily remove it. */
        m_mounted = true;
        R_UNLESS(std::strlen(root_path) < sizeof(m_root) && std::strlen(mount_name) < sizeof(m_mount), kvdb::ResultInvalidKeyValue());
        std::strcpy(m_root, root_path);
        std::strcpy(m_mount, mount_name);
        const size_t root_length = std::strlen(m_root);
        const char *separator = root_length != 0 && m_root[root_length - 1] == '/' ? "" : "/";
        util::SNPrintf(m_archive, sizeof(m_archive), "%s%simkvdb.arc", m_root, separator);
        util::SNPrintf(m_index_paths[0], sizeof(m_index_paths[0]), "%s%sdb2-index-a.tmp", m_root, separator);
        util::SNPrintf(m_index_paths[1], sizeof(m_index_paths[1]), "%s%sdb2-index-b.tmp", m_root, separator);
        util::SNPrintf(m_values_path, sizeof(m_values_path), "%s%sdb2-values.tmp", m_root, separator);
        util::SNPrintf(m_temp_archive, sizeof(m_temp_archive), "%s%sdb2-archive.tmp", m_root, separator);
        util::SNPrintf(m_previous_archive, sizeof(m_previous_archive), "%s%sdb2-archive.prev", m_root, separator);
        m_space_id = space_id;
        m_save_id = save_id;
        m_count = m_active_index = 0;
        m_values_size = m_base_size = 0;
        m_archive_size = sizeof(ArchiveHeader);
        m_dirty = m_commit_prepared = false;
        m_pending_commit_result = ResultSuccess();

        const Result result = [&]() -> Result {
            /* Scratch files never determine the committed database at boot. */
            for (const auto *path : {m_index_paths[0], m_index_paths[1], m_values_path, m_temp_archive}) R_TRY(this->DeleteIfPresent(path));
            const Result opened = fs::OpenFile(std::addressof(m_base_file), m_archive, fs::OpenMode_Read);
            if (R_SUCCEEDED(opened)) {
                m_base_open = true;
                s64 size;
                R_TRY(fs::GetFileSize(std::addressof(size), m_base_file));
                R_UNLESS(size >= static_cast<s64>(sizeof(ArchiveHeader)), kvdb::ResultInvalidKeyValue());
                m_base_size = static_cast<u64>(size);
            } else {
                R_TRY_CATCH(opened) {
                    R_CATCH(fs::ResultPathNotFound) {
                        /* An absent archive normally means an empty, newly
                         * created database. Do not erase a prior generation
                         * after an interrupted/failed rename restoration. */
                        fs::DirectoryEntryType type;
                        const Result previous = fs::GetEntryType(std::addressof(type), m_previous_archive);
                        R_UNLESS(R_FAILED(previous), kvdb::ResultInvalidKeyValue());
                        R_UNLESS(fs::ResultPathNotFound::Includes(previous), previous);
                    }
                } R_END_TRY_CATCH;
            }
            R_TRY(this->ImportArchive());
            R_TRY(fs::OpenFile(std::addressof(m_index_file), m_index_paths[0], fs::OpenMode_Read));
            m_index_open = true;
            R_TRY(this->ReplaceScratchFile(std::addressof(m_values_file), m_values_path, 0));
            m_values_open = true;
            /* A previous file can only be a disposable prior generation; the
             * canonical file is the sole source of committed state. */
            R_TRY(this->DeleteIfPresent(m_previous_archive));
            R_SUCCEED();
        }();
        if (R_FAILED(result)) R_RETURN(this->MakeUnavailable(result));
        m_available = true;
        m_unavailable_result = ResultSuccess();
        this->ResetCaches();
        R_SUCCEED();
    }

    Result PagedContentMetaStore::CopyIndexRange(fs::FileHandle out, u64 out_offset, u64 source_offset, u64 bytes) {
        while (bytes != 0) {
            const size_t part = static_cast<size_t>(std::min(bytes, static_cast<u64>(sizeof(m_transfer))));
            R_TRY(this->ReadBytes(m_index_file, source_offset, m_transfer, part));
            R_TRY(this->WriteBytes(out, out_offset, m_transfer, part));
            source_offset += part;
            out_offset += part;
            bytes -= part;
        }
        R_SUCCEED();
    }

    Result PagedContentMetaStore::PublishIndex(u32 slot, u32 count, u64 archive_size, u64 values_size) {
        /* Open the replacement before abandoning the valid active index. */
        fs::FileHandle next;
        R_TRY(fs::OpenFile(std::addressof(next), m_index_paths[slot], fs::OpenMode_Read));
        if (m_index_open) fs::CloseFile(m_index_file);
        m_index_file = next;
        m_index_open = true;
        m_active_index = slot;
        m_count = count;
        m_archive_size = archive_size;
        m_values_size = values_size;
        ++m_generation;
        this->ResetCaches();
        R_SUCCEED();
    }

    Result PagedContentMetaStore::RewriteIndex(u32 position, const IndexEntry *entry, bool remove) {
        IndexEntry old{};
        bool exists = false;
        if (position < m_count) {
            R_TRY(this->ReadIndex(std::addressof(old), position));
            exists = remove || old.key == entry->key;
        }
        R_UNLESS(!remove || exists, kvdb::ResultKeyNotFound());
        R_UNLESS(remove || exists || m_count < INT32_MAX, kvdb::ResultOutOfKeyResource());
        const u32 next_count = remove ? m_count - 1 : m_count + (exists ? 0 : 1);
        u64 archive_size = m_archive_size;
        if (exists) archive_size -= sizeof(ArchiveHeader) + sizeof(ContentMetaKey) + old.size;
        if (!remove) {
            const u64 added = sizeof(ArchiveHeader) + sizeof(ContentMetaKey) + static_cast<u64>(entry->size);
            R_UNLESS(Fits(archive_size, added, MaxFileSize), kvdb::ResultInvalidKeyValue());
            archive_size += added;
        }
        const u32 slot = 1 - m_active_index;
        fs::FileHandle out;
        R_TRY(this->ReplaceScratchFile(std::addressof(out), m_index_paths[slot], static_cast<u64>(next_count) * sizeof(IndexEntry)));
        {
            ON_SCOPE_EXIT { fs::CloseFile(out); };
            const u64 prefix = static_cast<u64>(position) * sizeof(IndexEntry);
            R_TRY(this->CopyIndexRange(out, 0, 0, prefix));
            u64 destination = prefix;
            if (!remove) {
                R_TRY(this->WriteBytes(out, destination, entry, sizeof(*entry)));
                destination += sizeof(*entry);
            }
            const u64 suffix = prefix + (exists ? sizeof(IndexEntry) : 0);
            R_TRY(this->CopyIndexRange(out, destination, suffix, static_cast<u64>(m_count) * sizeof(IndexEntry) - suffix));
            R_TRY(fs::FlushFile(out));
        }
        const u64 values_size = !remove ? std::max(m_values_size, entry->offset + entry->size) : m_values_size;
        R_TRY(this->PublishIndex(slot, next_count, archive_size, values_size));
        m_dirty = true;
        R_SUCCEED();
    }

    Result PagedContentMetaStore::Set(const ContentMetaKey &key, const void *value, size_t size) {
        R_TRY(this->EnsureMutable());
        R_UNLESS(size <= UINT32_MAX && Fits(m_values_size, size, MaxFileSize), kvdb::ResultInvalidKeyValue());
        u32 position;
        R_TRY(this->LowerBound(std::addressof(position), key));
        bool exists = false;
        if (position < m_count) {
            IndexEntry old;
            R_TRY(this->ReadIndex(std::addressof(old), position));
            exists = old.key == key;
        }
        R_UNLESS(exists || m_count < INT32_MAX, kvdb::ResultOutOfKeyResource());
        const u64 index_bytes = static_cast<u64>(m_count + (exists ? 0 : 1)) * sizeof(IndexEntry);
        /* Discard only the inactive index before reserving room. Neither the
         * current index nor canonical database is touched on a failed write. */
        R_TRY(this->DeleteIfPresent(m_index_paths[1 - m_active_index]));
        R_TRY(this->EnsureSpace(static_cast<u64>(size) + index_bytes + PageSize));
        R_TRY(fs::SetFileSize(m_values_file, static_cast<s64>(m_values_size + size)));
        R_TRY(this->WriteBytes(m_values_file, m_values_size, value, size));
        R_TRY(fs::FlushFile(m_values_file));
        IndexEntry entry{key, m_values_size, static_cast<u32>(size), 1};
        R_RETURN(this->RewriteIndex(position, std::addressof(entry), false));
    }

    Result PagedContentMetaStore::Remove(const ContentMetaKey &key) {
        R_TRY(this->EnsureMutable());
        u32 position;
        R_TRY(this->LowerBound(std::addressof(position), key));
        R_UNLESS(position < m_count, kvdb::ResultKeyNotFound());
        IndexEntry entry;
        R_TRY(this->ReadIndex(std::addressof(entry), position));
        R_UNLESS(entry.key == key, kvdb::ResultKeyNotFound());
        R_TRY(this->DeleteIfPresent(m_index_paths[1 - m_active_index]));
        R_TRY(this->EnsureSpace(static_cast<u64>(m_count - 1) * sizeof(IndexEntry) + PageSize));
        R_RETURN(this->RewriteIndex(position, nullptr, true));
    }

    Result PagedContentMetaStore::PrepareCommit() {
        const u32 next_slot = 1 - m_active_index;
        R_TRY(this->DeleteIfPresent(m_index_paths[next_slot]));
        R_TRY(this->DeleteIfPresent(m_temp_archive));
        const u64 index_bytes = static_cast<u64>(m_count) * sizeof(IndexEntry);
        R_UNLESS(Fits(m_archive_size, index_bytes + PageSize, MaxFileSize), fs::ResultNotEnoughFreeSpace());
        R_TRY(this->EnsureSpace(m_archive_size + index_bytes + PageSize));
        fs::FileHandle archive;
        R_TRY(this->ReplaceScratchFile(std::addressof(archive), m_temp_archive, m_archive_size));
        {
            ON_SCOPE_EXIT { fs::CloseFile(archive); };
            fs::FileHandle index;
            R_TRY(this->ReplaceScratchFile(std::addressof(index), m_index_paths[next_slot], index_bytes));
            ON_SCOPE_EXIT { fs::CloseFile(index); };
            ArchiveHeader header{};
            std::memcpy(header.magic, "IMKV", 4);
            header.second = m_count;
            R_TRY(this->WriteBytes(archive, 0, std::addressof(header), sizeof(header)));
            u64 offset = sizeof(header);
            for (u32 i = 0; i < m_count; ++i) {
                IndexEntry entry;
                R_TRY(this->ReadIndex(std::addressof(entry), i));
                ArchiveHeader entry_header{};
                std::memcpy(entry_header.magic, "IMEN", 4);
                entry_header.first = sizeof(ContentMetaKey);
                entry_header.second = entry.size;
                R_TRY(this->WriteBytes(archive, offset, std::addressof(entry_header), sizeof(entry_header)));
                offset += sizeof(entry_header);
                R_TRY(this->WriteBytes(archive, offset, std::addressof(entry.key), sizeof(entry.key)));
                offset += sizeof(entry.key);
                IndexEntry next_entry{entry.key, offset, entry.size, 0};
                R_TRY(this->WriteBytes(index, static_cast<u64>(i) * sizeof(IndexEntry), std::addressof(next_entry), sizeof(next_entry)));
                u64 copied = 0;
                while (copied != entry.size) {
                    const size_t part = static_cast<size_t>(std::min(static_cast<u64>(sizeof(m_transfer)), static_cast<u64>(entry.size) - copied));
                    R_TRY(this->ReadData(entry.staged != 0, entry.offset + copied, m_transfer, part));
                    R_TRY(this->WriteBytes(archive, offset + copied, m_transfer, part));
                    copied += part;
                }
                offset += entry.size;
            }
            R_UNLESS(offset == m_archive_size, kvdb::ResultInvalidKeyValue());
            R_TRY(fs::FlushFile(index));
            R_TRY(fs::FlushFile(archive));
        }

        /* Keep an old name until the replacement has been renamed. A failed
         * second rename restores the canonical name, preserving current reads.
         * This is inside the existing transactional save-data filesystem. */
        const bool had_base = m_base_open;
        this->CloseFiles();
        Result result = this->DeleteIfPresent(m_previous_archive);
        if (R_SUCCEEDED(result) && had_base) result = fs::RenameFile(m_archive, m_previous_archive);
        if (R_FAILED(result)) {
            const Result reopened = this->OpenFiles();
            if (R_FAILED(reopened)) R_RETURN(reopened);
            R_RETURN(result);
        }
        result = fs::RenameFile(m_temp_archive, m_archive);
        if (R_FAILED(result)) {
            if (had_base) {
                const Result restored = fs::RenameFile(m_previous_archive, m_archive);
                if (R_FAILED(restored)) R_RETURN(this->MakeUnavailable(restored));
            }
            const Result reopened = this->OpenFiles();
            if (R_FAILED(reopened)) R_RETURN(reopened);
            R_RETURN(result);
        }

        /* The live namespace now contains all pending records. If the following
         * commit fails, keep it readable and allow only an explicit Commit retry. */
        m_active_index = next_slot;
        m_base_size = m_archive_size;
        m_values_size = 0;
        m_commit_prepared = true;
        m_pending_commit_result = ncm::ResultInvalidContentMetaDatabase();
        ++m_generation;
        R_RETURN(this->OpenFiles());
    }

    Result PagedContentMetaStore::FinishCommit() {
        this->CloseFiles();
        Result result = this->DeleteIfPresent(m_previous_archive);
        if (R_SUCCEEDED(result)) result = this->DeleteIfPresent(m_index_paths[1 - m_active_index]);
        if (R_SUCCEEDED(result)) {
            fs::FileHandle values;
            result = fs::OpenFile(std::addressof(values), m_values_path, fs::OpenMode_ReadWrite);
            if (R_SUCCEEDED(result)) {
                result = fs::SetFileSize(values, 0);
                if (R_SUCCEEDED(result)) result = fs::FlushFile(values);
                fs::CloseFile(values);
            }
        }
        if (R_SUCCEEDED(result)) result = fs::CommitSaveData(m_mount);
        m_pending_commit_result = result;
        const Result reopened = this->OpenFiles();
        if (R_FAILED(reopened)) R_RETURN(reopened);
        R_TRY(result);
        m_commit_prepared = m_dirty = false;
        ++m_generation;
        this->ResetCaches();
        R_SUCCEED();
    }

    Result PagedContentMetaStore::Commit() {
        R_TRY(this->EnsureAvailable());
        if (!m_commit_prepared) R_TRY(this->PrepareCommit());
        R_RETURN(this->FinishCommit());
    }

}
