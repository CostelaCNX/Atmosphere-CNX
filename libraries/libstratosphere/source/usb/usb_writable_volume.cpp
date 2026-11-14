/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 or later.
 */
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_io_diagnostics.hpp>
#include <stratosphere/usb/usb_pause_diagnostics.hpp>
#include <stratosphere/usb/usb_metadata_cache.hpp>
#include <stratosphere/usb/usb_startup_io_diagnostics.hpp>
#include "usb_fatfs.hpp"

#include "ff.h"

static_assert(FF_LBA64 == 1 && sizeof(LBA_t) == sizeof(u64), "Writable USB needs 64-bit FatFs sectors");

namespace ams::usb::volume {

    namespace {

        constexpr size_t FatPathBufferSize = MaxFileNameLength + 4;
        static_assert(FF_FS_LOCK >= 10, "USB mixed-file cache requires FatFs alias protection");

        Result TranslateFatResult(FRESULT result) {
            switch (result) {
                case FR_OK:
                    R_SUCCEED();
                case FR_NO_FILE:
                case FR_NO_PATH:
                    R_RETURN(usb::ResultFileNotFound());
                case FR_INVALID_NAME:
                case FR_INVALID_OBJECT:
                case FR_INVALID_PARAMETER:
                    R_RETURN(usb::ResultInvalidParameter());
                case FR_EXIST:
                    R_RETURN(usb::ResultAlreadyInitialized());
                case FR_NOT_READY:
                    R_RETURN(usb::ResultNotInitialized());
                case FR_WRITE_PROTECTED:
                    R_RETURN(usb::ResultOperationDenied());
                case FR_NOT_ENOUGH_CORE:
                    R_RETURN(usb::ResultMemAllocFailure());
                case FR_DENIED:
                    R_RETURN(usb::ResultOperationDenied());
                case FR_TOO_MANY_OPEN_FILES:
                case FR_LOCKED:
                case FR_TIMEOUT:
                    R_RETURN(usb::ResultResourceBusy());
                case FR_DISK_ERR:
                case FR_INT_ERR:
                case FR_NOT_ENABLED:
                case FR_NO_FILESYSTEM:
                case FR_MKFS_ABORTED:
                default:
                    R_RETURN(usb::ResultTransactionError());
            }
        }

        Result MakeFatPath(char *out, size_t out_size, const char *path) {
            R_UNLESS(out != nullptr && out_size >= FatPathBufferSize && path != nullptr, usb::ResultInvalidParameter());

            const size_t length = std::strlen(path);
            R_UNLESS(length <= MaxFileNameLength, usb::ResultInvalidParameter());

            size_t write_offset = 0;
            const char drive_prefix[] = "0:/";
            std::memcpy(out, drive_prefix, sizeof(drive_prefix) - 1);
            write_offset = sizeof(drive_prefix) - 1;

            size_t read_offset = 0;
            while (read_offset < length && path[read_offset] == '/') {
                ++read_offset;
            }

            if (read_offset == length) {
                out[write_offset] = '\0';
                R_SUCCEED();
            }

            while (read_offset < length) {
                const size_t segment_start = read_offset;
                while (read_offset < length && path[read_offset] != '/') {
                    R_UNLESS(path[read_offset] != '\\' && path[read_offset] != ':', usb::ResultInvalidParameter());
                    ++read_offset;
                }
                const size_t segment_length = read_offset - segment_start;
                R_UNLESS(segment_length > 0 && !(segment_length == 1 && path[segment_start] == '.') && !(segment_length == 2 && path[segment_start] == '.' && path[segment_start + 1] == '.'), usb::ResultInvalidParameter());
                R_UNLESS(write_offset <= out_size - 1 && segment_length <= out_size - write_offset - 1, usb::ResultInvalidParameter());

                std::memcpy(out + write_offset, path + segment_start, segment_length);
                write_offset += segment_length;
                if (read_offset < length) {
                    ++read_offset;
                    R_UNLESS(read_offset < length, usb::ResultInvalidParameter());
                    R_UNLESS(path[read_offset] != '/', usb::ResultInvalidParameter());
                    out[write_offset++] = '/';
                }
            }

            out[write_offset] = '\0';
            R_SUCCEED();
        }

        Result MakeFatPathPair(char *old_path, char *new_path, const char *old_name, const char *new_name) {
            R_TRY(MakeFatPath(old_path, FatPathBufferSize, old_name));
            R_RETURN(MakeFatPath(new_path, FatPathBufferSize, new_name));
        }

        struct ScopedFile {
            FIL file{};
            bool open{false};

            ~ScopedFile() {
                if (open) {
                    static_cast<void>(f_close(std::addressof(file)));
                }
            }
        };

        Result CloseFile(ScopedFile *file, Result result) {
            if (file->open) {
                const auto close_result = TranslateFatResult(f_close(std::addressof(file->file)));
                /* A failed f_close still owns its FatFs lock. Keep the handle
                 * for retry/volume finalization rather than leaking the lock. */
                if (R_SUCCEEDED(close_result)) {
                    file->open = false;
                }
                if (R_SUCCEEDED(result)) {
                    result = close_result;
                }
            }
            R_RETURN(result);
        }

    }

    struct WritableVolume::Impl {
        mass_storage::Device *device{nullptr};
        Partition partition{};
        FATFS filesystem{};

        /* All access is serialized by storage::Service::m_volume_mutex.
         * Keep read cursors for interleaved NCA sections, and one writer for
         * sequential installer chunks. Unrelated reads/writes may coexist;
         * FatFs' directory-object locks catch case and short-name aliases. */
        struct CachedFile {
            ScopedFile handle{};
            char path[FatPathBufferSize]{};
            DWORD contiguous_map[4]{};
        };
        CachedFile readers[8]{};
        CachedFile writer{};
        size_t next_reader{0};
        metadata::Cache<FileInfo, FatPathBufferSize, 64> metadata_cache{};

        void ClearMetadataCache() {
            if (metadata_cache.Clear()) {
                io_diagnostics::metadata_cache_invalidations.fetch_add(1, std::memory_order_relaxed);
            }
        }

        FIL *FindCached(const char *path) {
            if (writer.handle.open && std::strcmp(writer.path, path) == 0) {
                return std::addressof(writer.handle.file);
            }
            for (auto &entry : readers) {
                if (entry.handle.open && std::strcmp(entry.path, path) == 0) {
                    return std::addressof(entry.handle.file);
                }
            }
            return nullptr;
        }

        Result CloseReaders() {
            Result result = ResultSuccess();
            for (auto &entry : readers) {
                result = CloseFile(std::addressof(entry.handle), result);
            }
            R_RETURN(result);
        }

        Result CloseAll() {
            // Invalidate before mutations/flush, even when closing later fails.
            this->ClearMetadataCache();
            const auto result = this->CloseReaders();
            R_RETURN(CloseFile(std::addressof(writer.handle), result));
        }

        Result OpenCached(FIL **out, const char *path, bool write) {
            CachedFile *entry = nullptr;
            if (write) {
                entry = std::addressof(writer);
            } else {
                for (auto &candidate : readers) {
                    if (candidate.handle.open && std::strcmp(candidate.path, path) == 0) {
                        io_diagnostics::read_hits.fetch_add(1, std::memory_order_relaxed);
                        *out = std::addressof(candidate.handle.file);
                        R_SUCCEED();
                    }
                }
                entry = std::addressof(readers[next_reader]);
                next_reader = (next_reader + 1) % util::size(readers);
            }
            if (!entry->handle.open || std::strcmp(entry->path, path) != 0) {
                R_TRY(CloseFile(std::addressof(entry->handle), ResultSuccess()));
                entry->handle.file = {};
                const BYTE mode = write ? FA_OPEN_EXISTING | FA_WRITE : FA_READ;
                auto open_result = f_open(std::addressof(entry->handle.file), path, mode);
                if (open_result == FR_LOCKED) {
                    /* Only a real alias conflict switches access modes. For a
                     * USB-to-USB install the NSP source and NCA destination
                     * are different files: retain both sequential cursors. */
                    if (write) {
                        R_TRY(this->CloseReaders());
                    } else {
                        R_TRY(CloseFile(std::addressof(writer.handle), ResultSuccess()));
                    }
                    entry->handle.file = {};
                    open_result = f_open(std::addressof(entry->handle.file), path, mode);
                }
                R_TRY(TranslateFatResult(open_result));
                entry->handle.open = true;
                /* exFAT's NoFatChain flag proves the extent is contiguous.
                 * Construct its constant-size map without scanning a large
                 * file on first open. Readers only: a writer may grow. */
                auto &file = entry->handle.file;
                if (!write && file.obj.fs->fs_type == FS_EXFAT && file.obj.stat == 2 && f_size(&file) > 0) {
                    const u64 cluster_bytes = static_cast<u64>(file.obj.fs->csize) * file.obj.fs->ssize;
                    const u64 clusters = (f_size(&file) - 1) / cluster_bytes + 1;
                    if (file.obj.sclust >= 2 && file.obj.sclust < file.obj.fs->n_fatent &&
                        clusters <= file.obj.fs->n_fatent - file.obj.sclust) {
                        entry->contiguous_map[0] = 4;
                        entry->contiguous_map[1] = static_cast<DWORD>(clusters);
                        entry->contiguous_map[2] = file.obj.sclust;
                        entry->contiguous_map[3] = 0;
                        file.cltbl = entry->contiguous_map;
                    }
                }
                io_diagnostics::file_opens.fetch_add(1, std::memory_order_relaxed);
                std::memcpy(entry->path, path, std::strlen(path) + 1);
            } else if (write) {
                io_diagnostics::write_hits.fetch_add(1, std::memory_order_relaxed);
            }
            *out = std::addressof(entry->handle.file);
            R_SUCCEED();
        }

        Result FinishIo(Result result) {
            /* Drop poisoned FIL objects on any failed request, while keeping
             * the original failure even if closing also fails. */
            if (R_FAILED(result)) {
                static_cast<void>(this->CloseAll());
            }
            R_RETURN(result);
        }
    };

    WritableVolume::~WritableVolume() {
        if (this->IsInitialized()) {
            static_cast<void>(this->Finalize());
        }
    }

    Result WritableVolume::Initialize(mass_storage::Device *device, const Partition &partition) {
        io_diagnostics::mount_stage.store(1);
        io_diagnostics::mount_fatfs_result.store(UINT32_MAX);
        io_diagnostics::mount_first_lba.store(partition.first_lba);
        io_diagnostics::mount_sector_count.store(partition.sector_count);
        io_diagnostics::mount_sector_size.store(0);
        R_UNLESS(!this->IsInitialized(), usb::ResultAlreadyInitialized());
        R_UNLESS(device != nullptr && device->IsInitialized(), usb::ResultNotInitialized());
        const u32 block_size = device->GetGeometry().block_size;
        io_diagnostics::mount_sector_size.store(block_size);
        R_UNLESS(block_size >= 0x200 && block_size <= 0x1000 && util::IsPowerOfTwo(block_size), usb::ResultUnsupportedFilesystem());
        R_UNLESS(partition.sector_count > 0, usb::ResultUnsupportedFilesystem());
        R_UNLESS(partition.first_lba < device->GetGeometry().block_count && partition.sector_count <= device->GetGeometry().block_count - partition.first_lba, usb::ResultInvalidParameter());

        auto *impl = new (std::nothrow) Impl;
        R_UNLESS(impl != nullptr, usb::ResultMemAllocFailure());
        impl->device = device;
        impl->partition = partition;

        io_diagnostics::mount_stage.store(2);
        const auto attach_result = fatfs::Attach(device, partition);
        if (R_FAILED(attach_result)) {
            delete impl;
            R_RETURN(attach_result);
        }

        io_diagnostics::mount_stage.store(3);
        const auto fat_result = f_mount(std::addressof(impl->filesystem), "0:", 1);
        io_diagnostics::mount_fatfs_result.store(static_cast<u32>(fat_result));
        const auto mount_result = TranslateFatResult(fat_result);
        if (R_FAILED(mount_result)) {
            // FatFs registers the object even when mounting immediately fails.
            // Unregister while its storage is still alive.
            static_cast<void>(f_mount(nullptr, "0:", 0));
            fatfs::Detach();
            delete impl;
            R_RETURN(mount_result);
        }

        m_impl = impl;
        io_diagnostics::mount_stage.store(4);
        R_SUCCEED();
    }

    Result WritableVolume::Finalize() {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());

        auto *impl = m_impl;
        m_impl = nullptr;
        auto result = impl->CloseAll();
        const auto unmount_result = TranslateFatResult(f_mount(nullptr, "0:", 0));
        if (R_SUCCEEDED(result)) {
            result = unmount_result;
        }
        fatfs::Detach();
        delete impl;
        R_RETURN(result);
    }

    const Partition &WritableVolume::GetPartition() const {
        AMS_ASSERT(this->IsInitialized());
        return m_impl->partition;
    }

    Result WritableVolume::CreateDirectory(const char *path) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));
        R_TRY(m_impl->CloseAll());
        R_RETURN(TranslateFatResult(f_mkdir(fat_path)));
    }

    Result WritableVolume::CreateFile(const char *path, u64 size) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(size <= static_cast<u64>(std::numeric_limits<FSIZE_t>::max()), usb::ResultInvalidParameter());

        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));

        ScopedFile file;
        R_TRY(m_impl->CloseAll());
        Result result = TranslateFatResult(f_open(std::addressof(file.file), fat_path, FA_CREATE_ALWAYS | FA_WRITE));
        if (R_SUCCEEDED(result)) {
            file.open = true;
            if (size > 0) {
                /* Allocate an exFAT extent in one bitmap pass. Fragmented
                 * free space is supported by the ordinary allocation path. */
                if (file.file.obj.fs->fs_type == FS_EXFAT) {
                    const auto expanded = f_expand(std::addressof(file.file), static_cast<FSIZE_t>(size), 1);
                    if (expanded != FR_DENIED) {
                        return CloseFile(std::addressof(file), TranslateFatResult(expanded));
                    }
                }
                result = TranslateFatResult(f_lseek(std::addressof(file.file), static_cast<FSIZE_t>(size - 1)));
                if (R_SUCCEEDED(result)) {
                    if (f_tell(std::addressof(file.file)) != size - 1) {
                        return CloseFile(std::addressof(file), usb::ResultTransactionError());
                    }
                    const u8 zero = 0;
                    UINT written = 0;
                    result = TranslateFatResult(f_write(std::addressof(file.file), std::addressof(zero), 1, std::addressof(written)));
                    if (R_SUCCEEDED(result)) {
                        R_UNLESS(written == 1, usb::ResultTransactionError());
                    }
                }
            }
        }
        R_RETURN(CloseFile(std::addressof(file), result));
    }

    Result WritableVolume::ResizeFile(const char *path, u64 size) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(size <= static_cast<u64>(std::numeric_limits<FSIZE_t>::max()), usb::ResultInvalidParameter());

        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));

        ScopedFile file;
        R_TRY(m_impl->CloseAll());
        Result result = TranslateFatResult(f_open(std::addressof(file.file), fat_path, FA_OPEN_EXISTING | FA_WRITE));
        if (R_SUCCEEDED(result)) {
            file.open = true;
            result = TranslateFatResult(f_lseek(std::addressof(file.file), static_cast<FSIZE_t>(size)));
            if (R_SUCCEEDED(result)) {
                result = TranslateFatResult(f_truncate(std::addressof(file.file)));
            }
        }

        R_RETURN(CloseFile(std::addressof(file), result));
    }

    Result WritableVolume::DeleteFile(const char *path) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));
        R_TRY(m_impl->CloseAll());
        R_RETURN(TranslateFatResult(f_unlink(fat_path)));
    }

    Result WritableVolume::RenameFile(const char *old_path, const char *new_path) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        char fat_old_path[FatPathBufferSize]{};
        char fat_new_path[FatPathBufferSize]{};
        R_TRY(MakeFatPathPair(fat_old_path, fat_new_path, old_path, new_path));
        R_TRY(m_impl->CloseAll());
        R_RETURN(TranslateFatResult(f_rename(fat_old_path, fat_new_path)));
    }

    Result WritableVolume::GetFileInfo(const char *path, FileInfo *out) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));

        /* File sizes stay current: mutations invalidate the cache, and each
         * completed write synchronizes its FIL. Avoid a second directory walk
         * for the IFile::DryWrite/GetSize call preceding each chunk. */
        if (auto *file = m_impl->FindCached(fat_path); file != nullptr) {
            out->size = f_size(file);
            out->first_cluster = 0;
            out->directory = false;
            R_SUCCEED();
        }

        if (m_impl->metadata_cache.Find(fat_path, out)) {
            io_diagnostics::metadata_cache_hits.fetch_add(1, std::memory_order_relaxed);
            R_SUCCEED();
        }
        io_diagnostics::metadata_cache_misses.fetch_add(1, std::memory_order_relaxed);

        FILINFO info{};
        io_diagnostics::metadata_stat_calls.fetch_add(1, std::memory_order_relaxed);
        const auto fat_result = f_stat(fat_path, std::addressof(info));
        const auto result = TranslateFatResult(fat_result);
        if (R_FAILED(result)) {
            // Missing-file probes are common during game startup and cannot
            // modify other entries. Never cache the error, but retain valid
            // positive answers; unexpected failures still discard them.
            if (fat_result != FR_NO_FILE && fat_result != FR_NO_PATH) {
                m_impl->ClearMetadataCache();
            }
            R_RETURN(result);
        }
        out->size = info.fsize;
        out->first_cluster = 0;
        out->directory = (info.fattrib & AM_DIR) != 0;
        m_impl->metadata_cache.Store(fat_path, *out);
        R_SUCCEED();
    }

    Result WritableVolume::ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read) {
        pause_diagnostics::Scope pause_scope(pause_diagnostics::Stage::FileRead, size);
        const io_diagnostics::ScopedTimer timer(io_diagnostics::read_us);
        io_diagnostics::read_calls.fetch_add(1, std::memory_order_relaxed);
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(out_read != nullptr && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
        *out_read = 0;

        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));

        FIL *file = nullptr;
        Result result = m_impl->OpenCached(std::addressof(file), fat_path, false);
        if (R_SUCCEEDED(result)) {
            R_UNLESS(offset <= static_cast<u64>(f_size(file)), usb::ResultInvalidParameter());
            result = TranslateFatResult(f_lseek(file, static_cast<FSIZE_t>(offset)));
            size_t remaining = size;
            u8 *dst = static_cast<u8 *>(buffer);
            while (R_SUCCEEDED(result) && remaining > 0) {
                const UINT request = static_cast<UINT>(std::min<size_t>(remaining, std::numeric_limits<UINT>::max()));
                UINT read = 0;
                result = TranslateFatResult(f_read(file, dst, request, std::addressof(read)));
                if (R_SUCCEEDED(result)) {
                    if (read == 0) {
                        result = usb::ResultTransactionError();
                        break;
                    }
                    *out_read += read;
                    dst += read;
                    remaining -= read;
                }
            }
        }
        io_diagnostics::read_bytes.fetch_add(*out_read, std::memory_order_relaxed);
        result = m_impl->FinishIo(result);
        pause_scope.Finish(result);
        R_RETURN(result);
    }

    Result WritableVolume::WriteFile(const char *path, u64 offset, const void *buffer, size_t size, size_t *out_written) {
        pause_diagnostics::Scope pause_scope(pause_diagnostics::Stage::FileWrite, size);
        const io_diagnostics::ScopedTimer timer(io_diagnostics::write_us);
        io_diagnostics::write_calls.fetch_add(1, std::memory_order_relaxed);
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(out_written != nullptr && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
        R_UNLESS(offset <= static_cast<u64>(std::numeric_limits<FSIZE_t>::max()), usb::ResultInvalidParameter());
        *out_written = 0;

        startup_io_diagnostics::PhaseScope preparation(NexusStartupIo_Prepare);
        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));

        // A write may grow the file, including through a case/short-name alias.
        // Clear before touching FatFs so partial or failed mutations cannot
        // leave successful metadata from an earlier request cached.
        m_impl->ClearMetadataCache();

        FIL *file = nullptr;
        Result result = m_impl->OpenCached(std::addressof(file), fat_path, true);
        if (R_SUCCEEDED(result)) {
            result = TranslateFatResult(f_lseek(file, static_cast<FSIZE_t>(offset)));
            if (R_SUCCEEDED(result) && static_cast<u64>(f_tell(file)) != offset) {
                // FatFs can return FR_OK with a clipped seek on full media or
                // at the FAT32 size limit. Never write at the wrong offset.
                result = usb::ResultOperationDenied();
            }
            preparation.Finish();
            size_t remaining = size;
            const u8 *src = static_cast<const u8 *>(buffer);
            while (R_SUCCEEDED(result) && remaining > 0) {
                const UINT request = static_cast<UINT>(std::min<size_t>(remaining, std::numeric_limits<UINT>::max()));
                UINT written = 0;
                {
                    startup_io_diagnostics::PhaseScope data_write(NexusStartupIo_DataWrite);
                    result = TranslateFatResult(f_write(file, src, request, std::addressof(written)));
                }
                if (R_SUCCEEDED(result)) {
                    if (written == 0) {
                        /* FatFs reports exhausted allocation/file-size limits
                         * as FR_OK with a short write, not a transport error.
                         * Still fail the request, but keep the drive mounted. */
                        result = usb::ResultOperationDenied();
                        break;
                    }
                    *out_written += written;
                    src += written;
                    remaining -= written;
                }
            }
            /* Preserve R150's write-through semantics. Only the file cursor
             * is retained; data and allocation metadata are still synced at
             * the end of every request, not deferred until eviction. */
            const auto sync_result = [&] {
                const startup_io_diagnostics::PhaseScope sync(NexusStartupIo_Sync);
                const io_diagnostics::ScopedTimer sync_timer(io_diagnostics::sync_us);
                return TranslateFatResult(f_sync(file));
            }();
            io_diagnostics::write_syncs.fetch_add(1, std::memory_order_relaxed);
            if (R_SUCCEEDED(result)) {
                result = sync_result;
            }
        }
        io_diagnostics::written_bytes.fetch_add(*out_written, std::memory_order_relaxed);
        result = m_impl->FinishIo(result);
        pause_scope.Finish(result);
        R_RETURN(result);
    }

    Result WritableVolume::ReadDirectory(const char *path, DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(path != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(max_entries == 0 || out != nullptr, usb::ResultInvalidParameter());
        *out_count = 0;
        if (!include_directories && !include_files) {
            R_SUCCEED();
        }

        char fat_path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(fat_path, sizeof(fat_path), path));

        DIR directory{};
        Result result = TranslateFatResult(f_opendir(std::addressof(directory), fat_path));
        if (R_FAILED(result)) {
            R_RETURN(result);
        }

        size_t skipped = 0;
        size_t returned = 0;
        size_t total = 0;
        while (R_SUCCEEDED(result)) {
            FILINFO info{};
            result = TranslateFatResult(f_readdir(std::addressof(directory), std::addressof(info)));
            if (R_FAILED(result) || info.fname[0] == '\0') {
                break;
            }

            const bool is_directory = (info.fattrib & AM_DIR) != 0;
            if ((is_directory && include_directories) || (!is_directory && include_files)) {
                if (skipped < skip_entries) {
                    ++skipped;
                } else {
                    ++total;
                    if (max_entries == 0) {
                        continue;
                    }

                    std::strncpy(out[returned].name, info.fname, sizeof(out[returned].name) - 1);
                    out[returned].name[sizeof(out[returned].name) - 1] = '\0';
                    out[returned].size = info.fsize;
                    out[returned].directory = is_directory;
                    ++returned;
                    if (returned == max_entries) {
                        break;
                    }
                }
            }
        }

        const auto close_result = TranslateFatResult(f_closedir(std::addressof(directory)));
        if (R_SUCCEEDED(result)) {
            result = close_result;
        }
        if (R_SUCCEEDED(result)) {
            *out_count = max_entries == 0 ? total : returned;
        }
        R_RETURN(result);
    }

    Result WritableVolume::GetSpaceSize(u64 *out_free_size, u64 *out_total_size) {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(out_free_size != nullptr && out_total_size != nullptr, usb::ResultInvalidParameter());

        DWORD free_clusters = 0;
        FATFS *filesystem = nullptr;
        R_TRY(TranslateFatResult(f_getfree("0:", std::addressof(free_clusters), std::addressof(filesystem))));
        R_UNLESS(filesystem != nullptr, usb::ResultInternalStateError());

        #if FF_MAX_SS != FF_MIN_SS
            const u64 sector_size = filesystem->ssize;
        #else
            const u64 sector_size = FF_MAX_SS;
        #endif
        const u64 cluster_size = static_cast<u64>(filesystem->csize) * sector_size;
        R_UNLESS(cluster_size > 0, usb::ResultCorruptedFilesystem());
        R_UNLESS(static_cast<u64>(free_clusters) <= UINT64_MAX / cluster_size, usb::ResultCorruptedFilesystem());
        R_UNLESS(filesystem->n_fatent >= 2 && static_cast<u64>(filesystem->n_fatent - 2) <= UINT64_MAX / cluster_size, usb::ResultCorruptedFilesystem());
        *out_free_size = static_cast<u64>(free_clusters) * cluster_size;
        *out_total_size = static_cast<u64>(filesystem->n_fatent - 2) * cluster_size;
        R_SUCCEED();
    }

    Result WritableVolume::Flush() {
        R_UNLESS(this->IsInitialized(), usb::ResultNotInitialized());
        R_RETURN(m_impl->CloseAll());
    }

    Result WritableVolume::InvalidateReadCache() {
        R_UNLESS(IsInitialized(), usb::ResultNotInitialized());
        // FatFs accepts multiple spellings for the same file. Drop all read
        // cursors and clean metadata without closing or syncing the writer.
        m_impl->ClearMetadataCache();
        fatfs::DiscardCleanSectors();
        if (!m_impl->filesystem.wflag) {
            m_impl->filesystem.winsect = std::numeric_limits<LBA_t>::max();
        }
        R_RETURN(m_impl->CloseReaders());
    }

    Result WritableVolume::WriteSleepMarker(const sleep::VolumeMarker &marker, bool *armed) {
        R_UNLESS(IsInitialized() && armed && sleep::IsVolumeMarker(marker), usb::ResultInvalidParameter());
        *armed = false;
        // The caller has drained all clients and closed their cached files.
        // Reuse the cached FIL to avoid putting a full sector buffer on PSC's stack.
        auto &handle = m_impl->writer.handle;
        R_UNLESS(!handle.open, usb::ResultResourceBusy());
        char path[FatPathBufferSize]{};
        R_TRY(MakeFatPath(path, sizeof(path), sleep::MarkerPath));
        auto opened = f_open(&handle.file, path, FA_READ | FA_WRITE | FA_OPEN_EXISTING);
        const bool create = opened == FR_NO_FILE;
        if (create) opened = f_open(&handle.file, path, FA_CREATE_NEW | FA_WRITE);
        // Existing read-only/foreign entries and full/locked directories
        // can use retained-session wake, without replacing anything.
        if (opened == FR_DENIED || opened == FR_WRITE_PROTECTED || opened == FR_EXIST || opened == FR_LOCKED)
            R_SUCCEED();
        R_TRY(TranslateFatResult(opened));
        handle.open = true;
        if (!create) {
            if (f_size(&handle.file) != sizeof(marker)) R_RETURN(CloseFile(&handle, ResultSuccess()));
            sleep::VolumeMarker existing{};
            UINT count{};
            auto rc = TranslateFatResult(f_read(&handle.file, &existing, sizeof(existing), &count));
            if (R_FAILED(rc)) R_RETURN(CloseFile(&handle, rc));
            if (count != sizeof(existing) || !sleep::IsVolumeMarker(existing))
                R_RETURN(CloseFile(&handle, ResultSuccess()));
            rc = TranslateFatResult(f_lseek(&handle.file, 0));
            if (R_FAILED(rc)) R_RETURN(CloseFile(&handle, rc));
        }
        UINT written{};
        Result rc = TranslateFatResult(f_write(&handle.file, &marker, sizeof(marker), &written));
        if (R_SUCCEEDED(rc) && written != sizeof(marker)) rc = usb::ResultTransactionError();
        if (R_FAILED(rc)) m_impl->device->Quarantine();
        rc = CloseFile(&handle, rc);
        if (R_FAILED(rc)) {
            m_impl->device->Quarantine(); // Never replay a failed/ambiguous sync on this transport.
            R_RETURN(rc);
        }
        *armed = true;
        R_SUCCEED();
    }

}
