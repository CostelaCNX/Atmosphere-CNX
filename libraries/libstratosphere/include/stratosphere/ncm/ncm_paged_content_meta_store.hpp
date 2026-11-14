/*
 * Copyright (c) Atmosphère-NX
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include <vapours.hpp>
#include <stratosphere/fs.hpp>
#include <stratosphere/ncm/ncm_content_meta_key.hpp>

namespace ams::ncm {

    /* Persistent user-content metadata. The canonical IMKV archive remains
     * unchanged until Commit; every index/value work file is disposable.
     * Callers serialize commands. Returned references are copied values and
     * remain valid only until a successful mutation, a Commit attempt which
     * replaces the live archive, or Invalidate. Scope references to a command. */
    class PagedContentMetaStore {
        NON_COPYABLE(PagedContentMetaStore);
        NON_MOVEABLE(PagedContentMetaStore);
        public:
            struct RecordRef {
                ContentMetaKey key;
                u64 offset;
                u64 size;
                u64 generation;
                bool staged;
            };

            static constexpr size_t PageSize = 0x1000;
            static constexpr size_t CacheBytes = 2 * PageSize;
            static constexpr size_t WorkingBufferBytes = PageSize;

            PagedContentMetaStore() = default;
            ~PagedContentMetaStore();
            Result Initialize(const char *root_path, const char *mount_name, fs::SaveDataSpaceId space_id, u64 save_id);
            u32 GetCount() const { return m_count; }
            Result At(RecordRef *out, u32 index);
            Result Find(RecordRef *out, bool *found, const ContentMetaKey &key);
            Result LowerBound(u32 *out_index, const ContentMetaKey &key);
            Result Read(const RecordRef &record, u64 offset, void *buffer, size_t size);
            Result Set(const ContentMetaKey &key, const void *value, size_t size);
            Result Remove(const ContentMetaKey &key);
            Result Commit();
            void Invalidate();
            bool IsAvailable() const { return m_available; }
            bool IsMounted() const { return m_mounted; }

        private:
            struct IndexEntry {
                ContentMetaKey key;
                u64 offset;
                u32 size;
                u32 staged;
            };
            static_assert(sizeof(IndexEntry) == 0x20);
            struct ArchiveHeader {
                u8 magic[4];
                u32 first;
                u32 second;
            };
            static_assert(sizeof(ArchiveHeader) == 0xC);

            char m_root[128]{};
            char m_mount[fs::MountNameLengthMax + 1]{};
            char m_archive[160]{};
            char m_index_paths[2][160]{};
            char m_values_path[160]{};
            char m_temp_archive[160]{};
            char m_previous_archive[160]{};
            fs::SaveDataSpaceId m_space_id{};
            u64 m_save_id{};
            fs::FileHandle m_base_file{};
            fs::FileHandle m_index_file{};
            fs::FileHandle m_values_file{};
            bool m_base_open{};
            bool m_index_open{};
            bool m_values_open{};
            bool m_available{};
            bool m_mounted{};
            bool m_dirty{};
            bool m_commit_prepared{};
            u32 m_count{};
            u32 m_active_index{};
            u64 m_generation{1};
            u64 m_base_size{};
            u64 m_values_size{};
            u64 m_archive_size{sizeof(ArchiveHeader)};
            Result m_unavailable_result{ResultSuccess()};
            Result m_pending_commit_result{ResultSuccess()};

            alignas(8) u8 m_index_cache[PageSize]{};
            alignas(8) u8 m_value_cache[PageSize]{};
            alignas(8) u8 m_transfer[PageSize]{};
            u64 m_index_cache_offset{};
            size_t m_index_cache_size{};
            u64 m_value_cache_offset{};
            size_t m_value_cache_size{};
            bool m_value_cache_staged{};

            Result EnsureAvailable() const;
            Result EnsureMutable() const;
            void ResetCaches();
            void CloseFiles();
            Result OpenFiles();
            Result MakeUnavailable(Result result);
            Result EnsureSpace(u64 required_free_bytes);
            Result ReadIndex(IndexEntry *out, u32 index);
            Result ReadData(bool staged, u64 offset, void *buffer, size_t size);
            Result ReadBytes(fs::FileHandle file, u64 offset, void *buffer, size_t size);
            Result WriteBytes(fs::FileHandle file, u64 offset, const void *buffer, size_t size);
            Result DeleteIfPresent(const char *path);
            Result ReplaceScratchFile(fs::FileHandle *out, const char *path, u64 size);
            Result CopyIndexRange(fs::FileHandle out, u64 out_offset, u64 source_offset, u64 bytes);
            Result PublishIndex(u32 slot, u32 count, u64 archive_size, u64 values_size);
            Result RewriteIndex(u32 position, const IndexEntry *entry, bool remove);
            Result ImportArchive();
            Result PrepareCommit();
            Result FinishCommit();
    };

}
