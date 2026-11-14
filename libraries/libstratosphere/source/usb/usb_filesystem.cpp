/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>
#include <stratosphere/fs/fs_read_only_filesystem.hpp>
#include <stratosphere/fs/impl/fs_newable.hpp>
#include <stratosphere/usb/usb_filesystem.hpp>

namespace ams::usb::filesystem {

    namespace {

        class ReadOnlyFile final : public fs::fsa::IFile, public fs::impl::Newable {
            NON_COPYABLE(ReadOnlyFile);
            NON_MOVEABLE(ReadOnlyFile);

            private:
                storage::EjectLease m_eject_lease;
                Service *m_service;
                u64 m_media_generation;
                char m_path[fs::EntryNameLengthMax + 1]{};

            public:
                Result AcquireLease() { R_RETURN(m_service->AcquireEjectLease(&m_eject_lease,m_media_generation)); }
                ReadOnlyFile(Service *service, const fs::Path &path, u64 media_generation) : m_service(service), m_media_generation(media_generation) {
                    AMS_ASSERT(m_service != nullptr);
                    const size_t length = path.GetLength();
                    AMS_ASSERT(length <= fs::EntryNameLengthMax);
                    std::memcpy(m_path, path.GetString(), length);
                    m_path[length] = '\0';
                }

            private:
                Result CheckMediaGeneration() const {
                    u64 current_media_generation = 0;
                    R_TRY(m_service->GetMediaGeneration(std::addressof(current_media_generation)));
                    R_UNLESS(current_media_generation == m_media_generation, usb::ResultMediaChanged());
                    R_SUCCEED();
                }

                virtual Result DoRead(size_t *out, s64 offset, void *buffer, size_t size, const fs::ReadOption &) override final {
                    R_TRY(this->CheckMediaGeneration());
                    R_UNLESS(offset >= 0, fs::ResultOutOfRange());
                    volume::FileInfo info{};
                    R_TRY(m_service->GetFileInfoAtGeneration(m_path, std::addressof(info), m_media_generation));
                    R_UNLESS(!info.directory && static_cast<u64>(offset) <= info.size, fs::ResultOutOfRange());
                    /* IFile allows a short read at EOF; the block-backed service
                     * expects an exact byte count and would treat it as I/O failure. */
                    const size_t request = static_cast<size_t>(std::min<u64>(size, info.size - static_cast<u64>(offset)));
                    *out = 0;
                    R_SUCCEED_IF(request == 0);
                    R_RETURN(m_service->ReadFileAtGeneration(m_path, static_cast<u64>(offset), buffer, request, out, m_media_generation));
                }

                virtual Result DoGetSize(s64 *out) override final {
                    R_TRY(this->CheckMediaGeneration());
                    volume::FileInfo info{};
                    R_TRY(m_service->GetFileInfoAtGeneration(m_path, std::addressof(info), m_media_generation));
                    R_UNLESS(!info.directory && info.size <= static_cast<u64>(std::numeric_limits<s64>::max()), fs::ResultOutOfRange());
                    *out = static_cast<s64>(info.size);
                    R_SUCCEED();
                }

                virtual Result DoFlush() override final {
                    R_SUCCEED();
                }

                virtual Result DoWrite(s64, const void *, size_t, const fs::WriteOption &) override final {
                    R_THROW(fs::ResultUnsupportedWriteForReadOnlyFile());
                }

                virtual Result DoSetSize(s64) override final {
                    R_THROW(fs::ResultUnsupportedWriteForReadOnlyFile());
                }

                virtual Result DoOperateRange(void *dst, size_t dst_size, fs::OperationId op_id, s64 offset, s64 size, const void *, size_t) override final {
                    switch (op_id) {
                        case fs::OperationId::Invalidate:
                            // RomFS can invalidate its entire backing file in one call.
                            R_RETURN(m_service->InvalidateReadCacheAtGeneration(m_media_generation));
                        case fs::OperationId::QueryRange: {
                            R_UNLESS(dst != nullptr, fs::ResultNullptrArgument());
                            R_UNLESS(dst_size == sizeof(fs::QueryRangeInfo), fs::ResultInvalidSize());
                            s64 file_size;
                            R_TRY(this->DoGetSize(&file_size));
                            R_UNLESS(offset >= 0 && offset <= file_size, fs::ResultOutOfRange());
                            R_UNLESS(size >= 0 && size <= file_size - offset, fs::ResultInvalidSize());
                            static_cast<fs::QueryRangeInfo *>(dst)->Clear();
                            R_SUCCEED();
                        }
                        default:
                            R_THROW(fs::ResultNotImplemented());
                    }
                }

            public:
                virtual sf::cmif::DomainObjectId GetDomainObjectId() const override {
                    AMS_ABORT();
                }
        };

        class ReadOnlyDirectory final : public fs::fsa::IDirectory, public fs::impl::Newable {
            NON_COPYABLE(ReadOnlyDirectory);
            NON_MOVEABLE(ReadOnlyDirectory);

            private:
                storage::EjectLease m_eject_lease;
                Service *m_service;
                fs::OpenDirectoryMode m_mode;
                u64 m_media_generation;
                size_t m_offset{0};
                char m_path[fs::EntryNameLengthMax + 1]{};

            public:
                Result AcquireLease() { R_RETURN(m_service->AcquireEjectLease(&m_eject_lease,m_media_generation)); }
                ReadOnlyDirectory(Service *service, const fs::Path &path, fs::OpenDirectoryMode mode, u64 media_generation) : m_service(service), m_mode(mode), m_media_generation(media_generation) {
                    AMS_ASSERT(m_service != nullptr);
                    const size_t length = path.GetLength();
                    AMS_ASSERT(length <= fs::EntryNameLengthMax);
                    std::memcpy(m_path, path.GetString(), length);
                    m_path[length] = '\0';
                }

            private:
                Result CheckMediaGeneration() const {
                    u64 current_media_generation = 0;
                    R_TRY(m_service->GetMediaGeneration(std::addressof(current_media_generation)));
                    R_UNLESS(current_media_generation == m_media_generation, usb::ResultMediaChanged());
                    R_SUCCEED();
                }

                virtual Result DoRead(s64 *out_count, fs::DirectoryEntry *out_entries, s64 max_entries) override final {
                    R_TRY(this->CheckMediaGeneration());
                    R_UNLESS(max_entries >= 0 && static_cast<u64>(max_entries) <= SIZE_MAX, fs::ResultOutOfRange());
                    *out_count = 0;
                    R_SUCCEED_IF(max_entries == 0);

                    volume::DirectoryEntryInfo entries[8]{};
                    const size_t requested = std::min<size_t>(static_cast<size_t>(max_entries), util::size(entries));
                    size_t count = 0;
                    R_TRY(m_service->ReadDirectoryAtGeneration(m_path, entries, requested, m_offset, (m_mode & fs::OpenDirectoryMode_Directory) != 0, (m_mode & fs::OpenDirectoryMode_File) != 0, std::addressof(count), m_media_generation));

                    for (size_t i = 0; i < count; ++i) {
                        std::strncpy(out_entries[i].name, entries[i].name, sizeof(out_entries[i].name) - 1);
                        out_entries[i].name[sizeof(out_entries[i].name) - 1] = '\0';
                        out_entries[i].type = entries[i].directory ? fs::DirectoryEntryType_Directory : fs::DirectoryEntryType_File;
                        R_UNLESS(entries[i].size <= static_cast<u64>(std::numeric_limits<s64>::max()), fs::ResultOutOfRange());
                        out_entries[i].file_size = entries[i].directory ? 0 : static_cast<s64>(entries[i].size);
                    }

                    R_UNLESS(count <= SIZE_MAX - m_offset, fs::ResultOutOfRange());
                    m_offset += count;
                    *out_count = static_cast<s64>(count);
                    R_SUCCEED();
                }

                virtual Result DoGetEntryCount(s64 *out) override final {
                    R_TRY(this->CheckMediaGeneration());
                    size_t count = 0;
                    R_TRY(m_service->ReadDirectoryAtGeneration(m_path, nullptr, 0, 0, (m_mode & fs::OpenDirectoryMode_Directory) != 0, (m_mode & fs::OpenDirectoryMode_File) != 0, std::addressof(count), m_media_generation));
                    R_UNLESS(count <= static_cast<size_t>(std::numeric_limits<s64>::max()), fs::ResultOutOfRange());
                    *out = static_cast<s64>(count);
                    R_SUCCEED();
                }

            public:
                virtual sf::cmif::DomainObjectId GetDomainObjectId() const override {
                    AMS_ABORT();
                }
        };

        class ReadOnlyFileSystem final : public fs::fsa::IFileSystem, public fs::impl::Newable {
            NON_COPYABLE(ReadOnlyFileSystem);
            NON_MOVEABLE(ReadOnlyFileSystem);

            private:
                Service *m_service;

            public:
                explicit ReadOnlyFileSystem(Service *service) : m_service(service) {
                    AMS_ASSERT(m_service != nullptr);
                }

            private:
                virtual Result DoCreateFile(const fs::Path &, s64, int) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }
                virtual Result DoDeleteFile(const fs::Path &) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }
                virtual Result DoCreateDirectory(const fs::Path &) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }
                virtual Result DoDeleteDirectory(const fs::Path &) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }
                virtual Result DoDeleteDirectoryRecursively(const fs::Path &) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }
                virtual Result DoRenameFile(const fs::Path &, const fs::Path &) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }
                virtual Result DoRenameDirectory(const fs::Path &, const fs::Path &) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }

                virtual Result DoGetEntryType(fs::DirectoryEntryType *out, const fs::Path &path) override final {
                    u64 media_generation = 0;
                    R_TRY(m_service->GetMediaGeneration(std::addressof(media_generation)));
                    if (path.IsEmpty() || path == "/") {
                        *out = fs::DirectoryEntryType_Directory;
                        R_SUCCEED();
                    }

                    volume::FileInfo info{};
                    R_TRY(m_service->GetFileInfoAtGeneration(path.GetString(), std::addressof(info), media_generation));
                    *out = info.directory ? fs::DirectoryEntryType_Directory : fs::DirectoryEntryType_File;
                    R_SUCCEED();
                }

                virtual Result DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out_file, const fs::Path &path, fs::OpenMode mode) override final {
                    R_UNLESS((mode & fs::OpenMode_All) == fs::OpenMode_Read, fs::ResultInvalidOpenMode());
                    R_UNLESS(path.GetLength() <= fs::EntryNameLengthMax, fs::ResultTooLongPath());

                    u64 media_generation = 0;
                    R_TRY(m_service->GetMediaGeneration(std::addressof(media_generation)));
                    volume::FileInfo info{};
                    R_TRY(m_service->GetFileInfoAtGeneration(path.GetString(), std::addressof(info), media_generation));
                    R_UNLESS(!info.directory, fs::ResultPathNotFound());

                    auto file = std::make_unique<ReadOnlyFile>(m_service, path, media_generation);
                    R_UNLESS(file != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());
                    R_TRY(file->AcquireLease());
                    *out_file = std::move(file);
                    R_SUCCEED();
                }

                virtual Result DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *out_dir, const fs::Path &path, fs::OpenDirectoryMode mode) override final {
                    R_UNLESS(path.GetLength() <= fs::EntryNameLengthMax, fs::ResultTooLongPath());
                    u64 media_generation = 0;
                    R_TRY(m_service->GetMediaGeneration(std::addressof(media_generation)));
                    if (!path.IsEmpty() && path != "/") {
                        volume::FileInfo info{};
                        R_TRY(m_service->GetFileInfoAtGeneration(path.GetString(), std::addressof(info), media_generation));
                        R_UNLESS(info.directory, fs::ResultPathNotFound());
                    }

                    auto dir = std::make_unique<ReadOnlyDirectory>(m_service, path, mode, media_generation);
                    R_UNLESS(dir != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());
                    R_TRY(dir->AcquireLease());
                    *out_dir = std::move(dir);
                    R_SUCCEED();
                }

                virtual Result DoCommit() override final { R_SUCCEED(); }
                virtual Result DoGetFreeSpaceSize(s64 *out, const fs::Path &) override final {
                    u64 free_size = 0;
                    u64 total_size = 0;
                    R_TRY(m_service->GetSpaceSize(std::addressof(free_size), std::addressof(total_size)));
                    R_UNLESS(free_size <= static_cast<u64>(std::numeric_limits<s64>::max()), fs::ResultOutOfRange());
                    *out = static_cast<s64>(free_size);
                    R_SUCCEED();
                }
                virtual Result DoGetTotalSpaceSize(s64 *out, const fs::Path &) override final {
                    u64 free_size = 0;
                    u64 total_size = 0;
                    R_TRY(m_service->GetSpaceSize(std::addressof(free_size), std::addressof(total_size)));
                    R_UNLESS(total_size <= static_cast<u64>(std::numeric_limits<s64>::max()), fs::ResultOutOfRange());
                    *out = static_cast<s64>(total_size);
                    R_SUCCEED();
                }
                virtual Result DoCleanDirectoryRecursively(const fs::Path &) override final { R_THROW(fs::ResultUnsupportedWriteForReadOnlyFileSystem()); }
                virtual Result DoCommitProvisionally(s64) override final { R_THROW(fs::ResultNotImplemented()); }
                virtual Result DoRollback() override final { R_SUCCEED(); }

        };

    }

    Result Service::AcquireEjectLease(storage::EjectLease *lease,u64 generation) {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage && lease,usb::ResultNotInitialized());
        R_RETURN(lease->Acquire(m_storage,generation));
    }

    Service::~Service() {
        if (this->IsMounted()) {
            static_cast<void>(this->Unmount());
        }
    }

    Result Service::Mount(storage::Service *storage) {
        R_UNLESS(storage != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(storage->IsRunning(), usb::ResultNotInitialized());

        std::scoped_lock lk(m_mutex);
        R_UNLESS(!m_mounted, usb::ResultAlreadyInitialized());
        m_storage = storage;
        m_mounted = true;
        R_SUCCEED();
    }

    Result Service::Unmount() {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted, usb::ResultNotInitialized());
        m_storage = nullptr;
        m_mounted = false;
        R_SUCCEED();
    }

    Result Service::GetStatus(Status *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());

        std::scoped_lock lk(m_mutex);
        out->mounted = m_mounted;
        out->ready = false;
        out->reserved = 0;
        out->last_result = static_cast<Result>(usb::ResultNotInitialized()).GetValue();
        if (!m_mounted) {
            R_SUCCEED();
        }

        storage::Status storage_status{};
        R_TRY(m_storage->GetStatus(std::addressof(storage_status)));
        out->ready = storage_status.state == storage::State::Ready;
        out->last_result = storage_status.last_result;
        R_SUCCEED();
    }

    Result Service::GetMediaGeneration(u64 *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->GetMediaGeneration(out));
    }

    Result Service::GetFileInfo(const char *path, volume::FileInfo *out) {
        R_UNLESS(path != nullptr && out != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->GetFileInfo(path, out));
    }

    Result Service::ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_UNLESS(path != nullptr && out_read != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->ReadFile(path, offset, buffer, size, out_read));
    }

    Result Service::ReadDirectory(const char *path, volume::DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count) {
        R_UNLESS(path != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->ReadDirectory(path, out, max_entries, skip_entries, include_directories, include_files, out_count));
    }

    Result Service::GetFileInfoAtGeneration(const char *path, volume::FileInfo *out, u64 media_generation) {
        R_UNLESS(path != nullptr && out != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->GetFileInfoAtGeneration(path, out, media_generation));
    }

    Result Service::ReadFileAtGeneration(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read, u64 media_generation) {
        R_UNLESS(path != nullptr && out_read != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->ReadFileAtGeneration(path, offset, buffer, size, out_read, media_generation));
    }

    Result Service::InvalidateReadCacheAtGeneration(u64 media_generation) {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->InvalidateReadCacheAtGeneration(media_generation));
    }

    Result Service::ReadDirectoryAtGeneration(const char *path, volume::DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count, u64 media_generation) {
        R_UNLESS(path != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->ReadDirectoryAtGeneration(path, out, max_entries, skip_entries, include_directories, include_files, out_count, media_generation));
    }

    Result Service::GetSpaceSize(u64 *out_free_size, u64 *out_total_size) {
        R_UNLESS(out_free_size != nullptr && out_total_size != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_mounted && m_storage != nullptr, usb::ResultNotInitialized());
        R_RETURN(m_storage->GetSpaceSize(out_free_size, out_total_size));
    }

    bool Service::IsMounted() const {
        std::scoped_lock lk(m_mutex);
        return m_mounted;
    }

    Result CreateReadOnlyFileSystem(std::unique_ptr<fs::fsa::IFileSystem> *out, Service *service) {
        R_UNLESS(out != nullptr && service != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(service->IsMounted(), usb::ResultNotInitialized());
        auto fs = std::make_unique<ReadOnlyFileSystem>(service);
        R_UNLESS(fs != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());
        *out = std::move(fs);
        R_SUCCEED();
    }

}
