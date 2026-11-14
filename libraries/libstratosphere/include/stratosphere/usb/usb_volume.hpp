/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 2 or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <vapours.hpp>
#include <stratosphere/usb/usb_mass_storage.hpp>
#include <stratosphere/usb/usb_resume_marker.hpp>

namespace ams::usb::volume {

    constexpr inline size_t MaxPartitionCount = 16;
    constexpr inline size_t MaxFileNameLength = 255;

    enum class PartitionStyle : u8 {
        Mbr,
        Gpt,
        SuperFloppy,
    };

    struct Partition {
        u64 first_lba;
        u64 sector_count;
        u8 type;
        bool bootable;
        PartitionStyle style;
        char name[37];
    };

    Result DiscoverPartitions(mass_storage::Device *device, Partition *out, size_t out_count, size_t *out_found);

    struct FileInfo {
        u64 size;
        u32 first_cluster;
        bool directory;
    };

    struct DirectoryEntryInfo {
        char name[MaxFileNameLength + 1];
        u64 size;
        bool directory;
    };

    class Fat32Volume {
        NON_COPYABLE(Fat32Volume);
        NON_MOVEABLE(Fat32Volume);

        private:
            mass_storage::Device *m_device{nullptr};
            Partition m_partition{};
            bool m_is_initialized{false};

            u32 m_bytes_per_sector{0};
            u32 m_sectors_per_cluster{0};
            u32 m_reserved_sector_count{0};
            u32 m_fat_count{0};
            u32 m_fat_size_sectors{0};
            u32 m_root_cluster{0};
            u32 m_cluster_count{0};
            u64 m_first_fat_lba{0};
            u64 m_first_data_lba{0};

            alignas(0x1000) u8 m_sector_buffer[0x1000]{};

        public:
            Fat32Volume() = default;
            ~Fat32Volume();

        public:
            Result Initialize(mass_storage::Device *device, const Partition &partition);
            Result Finalize();

            bool IsInitialized() const { return m_is_initialized; }
            const Partition &GetPartition() const { return m_partition; }
            u32 GetBytesPerSector() const { return m_bytes_per_sector; }
            u32 GetSectorsPerCluster() const { return m_sectors_per_cluster; }
            u32 GetClusterCount() const { return m_cluster_count; }

            Result GetFileInfo(const char *path, FileInfo *out);
            Result ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read);
            Result ReadDirectory(const char *path, DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count);
            Result GetSpaceSize(u64 *out_free_size, u64 *out_total_size);

        private:
            struct DirectoryEntry {
                char name[MaxFileNameLength + 1];
                u32 first_cluster;
                u64 size;
                bool directory;
            };

            Result ReadRelativeSectors(u64 lba, u32 sector_count, void *buffer, size_t buffer_size);
            Result ReadFatEntry(u32 cluster, u32 *out_next_cluster);
            Result GetClusterLba(u32 cluster, u64 *out_lba) const;
            Result FindInDirectory(u32 directory_cluster, const char *name, DirectoryEntry *out);
            Result ResolvePath(const char *path, DirectoryEntry *out);
            Result ReadDirectoryCluster(u32 cluster, const char *name, DirectoryEntry *out, bool *out_found);
            Result ReadFileData(const DirectoryEntry &entry, u64 offset, void *buffer, size_t size, size_t *out_read);
    };

    class ExFatVolume {
        NON_COPYABLE(ExFatVolume);
        NON_MOVEABLE(ExFatVolume);

        private:
            mass_storage::Device *m_device{nullptr};
            Partition m_partition{};
            bool m_is_initialized{false};

            u32 m_bytes_per_sector{0};
            u32 m_sectors_per_cluster{0};
            u32 m_fat_offset{0};
            u32 m_fat_length{0};
            u32 m_cluster_heap_offset{0};
            u32 m_root_cluster{0};
            u32 m_cluster_count{0};
            u32 m_allocation_bitmap_cluster{0};
            u64 m_allocation_bitmap_size{0};
            bool m_allocation_bitmap_no_fat_chain{false};

            alignas(0x1000) u8 m_sector_buffer[0x1000]{};

        public:
            ExFatVolume() = default;
            ~ExFatVolume();

        public:
            Result Initialize(mass_storage::Device *device, const Partition &partition);
            Result Finalize();

            bool IsInitialized() const { return m_is_initialized; }
            const Partition &GetPartition() const { return m_partition; }
            u32 GetBytesPerSector() const { return m_bytes_per_sector; }
            u32 GetSectorsPerCluster() const { return m_sectors_per_cluster; }
            u32 GetClusterCount() const { return m_cluster_count; }

            Result GetFileInfo(const char *path, FileInfo *out);
            Result ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read);
            Result ReadDirectory(const char *path, DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count);
            Result GetSpaceSize(u64 *out_free_size, u64 *out_total_size);

        private:
            struct DirectoryEntry {
                char name[MaxFileNameLength + 1];
                u32 first_cluster;
                u64 size;
                bool directory;
                bool no_fat_chain;
            };

            Result ReadRelativeSectors(u64 lba, u32 sector_count, void *buffer, size_t buffer_size);
            Result ReadFatEntry(u32 cluster, u32 *out_next_cluster);
            Result GetClusterLba(u32 cluster, u64 *out_lba) const;
            Result FindInDirectory(u32 directory_cluster, const char *name, DirectoryEntry *out);
            Result ResolvePath(const char *path, DirectoryEntry *out);
            Result ReadDirectoryCluster(u32 cluster, const char *name, DirectoryEntry *out, bool *out_found);
            Result ReadFileData(const DirectoryEntry &entry, u64 offset, void *buffer, size_t size, size_t *out_read);
            Result FindAllocationBitmap();
    };

    /*
     * Writable FAT32/exFAT access used by the future host content-storage
     * backend.  The existing Fat32Volume and ExFatVolume readers deliberately
     * stay read-only so the usbfs/homebrew path cannot accidentally mutate a
     * disk.  This class is a separate opt-in primitive for content download
     * transactions.
     */
    class WritableVolume {
        NON_COPYABLE(WritableVolume);
        NON_MOVEABLE(WritableVolume);

        private:
            struct Impl;
            Impl *m_impl{nullptr};

        public:
            WritableVolume() = default;
            ~WritableVolume();

        public:
            Result Initialize(mass_storage::Device *device, const Partition &partition);
            Result Finalize();

            bool IsInitialized() const { return m_impl != nullptr; }
            const Partition &GetPartition() const;

            Result CreateDirectory(const char *path);
            Result CreateFile(const char *path, u64 size);
            Result ResizeFile(const char *path, u64 size);
            Result DeleteFile(const char *path);
            Result RenameFile(const char *old_path, const char *new_path);
            Result GetFileInfo(const char *path, FileInfo *out);
            Result ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read);
            Result WriteFile(const char *path, u64 offset, const void *buffer, size_t size, size_t *out_written);
            Result ReadDirectory(const char *path, DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count);
            Result GetSpaceSize(u64 *out_free_size, u64 *out_total_size);
            Result Flush();
            Result InvalidateReadCache();
            Result WriteSleepMarker(const sleep::VolumeMarker &marker, bool *armed);
    };

}
