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
#include <stratosphere.hpp>
#include "usb_partition_probe.hpp"

#if defined(ATMOSPHERE_OS_HORIZON)

namespace ams::usb::volume {

    namespace {

        constexpr u16 MbrSignature = 0xAA55;
        constexpr u32 MbrPartitionTableOffset = 446;
        constexpr u32 MbrPartitionEntrySize = 16;
        constexpr u8 MbrProtectiveGptType = 0xEE;

        constexpr u32 Fat32BootSignatureOffset = 510;
        constexpr u32 Fat32BootSignature = 0xAA55;
        constexpr u32 Fat32EndOfChain = 0x0FFFFFF8;
        constexpr u32 Fat32BadCluster = 0x0FFFFFF7;

        constexpr u8 FatDirectoryAttributeLongName = 0x0F;
        constexpr u8 FatDirectoryAttributeVolumeId = 0x08;
        constexpr u8 FatDirectoryEntryEnd = 0x00;
        constexpr u8 FatDirectoryEntryDeleted = 0xE5;

        constexpr size_t GptHeaderSize = 92;
        constexpr size_t GptPartitionEntrySize = 128;
        constexpr size_t GptPartitionNameOffset = 56;
        constexpr size_t GptPartitionNameSize = 72;

        constexpr u64 InvalidLba = UINT64_MAX;

        constexpr bool IsValidSectorSize(u32 size) {
            return size >= 0x200 && size <= 0x1000 && util::IsPowerOfTwo(size);
        }

        constexpr bool IsValidClusterNumber(u32 cluster, u32 cluster_count) {
            return cluster >= 2 && cluster < cluster_count + 2;
        }

        constexpr u16 LoadLittleEndian16(const u8 *src) {
            return static_cast<u16>(src[0]) | (static_cast<u16>(src[1]) << 8);
        }

        constexpr u32 LoadLittleEndian32(const u8 *src) {
            return static_cast<u32>(src[0]) |
                   (static_cast<u32>(src[1]) << 8) |
                   (static_cast<u32>(src[2]) << 16) |
                   (static_cast<u32>(src[3]) << 24);
        }

        constexpr u64 LoadLittleEndian64(const u8 *src) {
            return static_cast<u64>(LoadLittleEndian32(src)) |
                   (static_cast<u64>(LoadLittleEndian32(src + 4)) << 32);
        }

        using impl::IsSuperFloppyBootSector;

        constexpr bool IsZeroGuid(const u8 *guid) {
            for (size_t i = 0; i < 16; ++i) {
                if (guid[i] != 0) {
                    return false;
                }
            }
            return true;
        }

        void CopyAsciiName(char *dst, size_t dst_size, const u8 *src, size_t src_size) {
            if (dst_size == 0) {
                return;
            }

            size_t length = 0;
            for (size_t i = 0; i < src_size && length + 1 < dst_size; ++i) {
                const u16 code = (src_size - i >= 2) ? LoadLittleEndian16(src + i) : src[i];
                if (src_size - i >= 2) {
                    ++i;
                }
                if (code == 0 || code == 0xFFFF) {
                    break;
                }
                dst[length++] = code < 0x80 ? static_cast<char>(code) : '_';
            }
            dst[length] = '\0';
        }

        bool AsciiEqualInsensitive(const char *lhs, const char *rhs) {
            while (*lhs != '\0' && *rhs != '\0') {
                char l = *lhs++;
                char r = *rhs++;
                if (l >= 'A' && l <= 'Z') {
                    l = static_cast<char>(l - 'A' + 'a');
                }
                if (r >= 'A' && r <= 'Z') {
                    r = static_cast<char>(r - 'A' + 'a');
                }
                if (l != r) {
                    return false;
                }
            }
            return *lhs == '\0' && *rhs == '\0';
        }

        void ResetLongName(char *name, bool *out_valid) {
            std::memset(name, 0, MaxFileNameLength + 1);
            *out_valid = false;
        }

        void AddShortName(char *out, size_t out_size, const u8 *entry) {
            size_t pos = 0;
            for (size_t i = 0; i < 8 && pos + 1 < out_size; ++i) {
                if (entry[i] == ' ') {
                    break;
                }
                out[pos++] = static_cast<char>(entry[i]);
            }
            if (entry[8] != ' ' && pos + 1 < out_size) {
                out[pos++] = '.';
                for (size_t i = 8; i < 11 && pos + 1 < out_size; ++i) {
                    if (entry[i] == ' ') {
                        break;
                    }
                    out[pos++] = static_cast<char>(entry[i]);
                }
            }
            out[pos] = '\0';
        }

        void AddLongNamePart(char *name, size_t name_size, size_t sequence, const u8 *entry) {
            if (sequence == 0) {
                return;
            }

            const size_t base = (sequence - 1) * 13;
            const u8 offsets[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (size_t i = 0; i < util::size(offsets); ++i) {
                const u16 code = LoadLittleEndian16(entry + offsets[i]);
                if (code == 0 || code == 0xFFFF) {
                    continue;
                }
                const size_t pos = base + i;
                if (pos + 1 >= name_size) {
                    continue;
                }
                name[pos] = code < 0x80 ? static_cast<char>(code) : '_';
                name[pos + 1] = '\0';
            }
        }

        Result AppendPartition(Partition *out, size_t out_count, size_t *out_found, const Partition &partition) {
            if (*out_found < out_count) {
                out[*out_found] = partition;
            }
            ++(*out_found);
            R_SUCCEED();
        }

        Result DiscoverGptPartitions(mass_storage::Device *device, Partition *out, size_t out_count, size_t *out_found, u8 *sector, u32 sector_size) {
            R_TRY(device->ReadBlocks(1, 1, sector, sector_size));
            R_UNLESS(std::memcmp(sector, "EFI PART", 8) == 0, usb::ResultOperationDenied());

            const u32 header_size = LoadLittleEndian32(sector + 12);
            const u64 current_lba = LoadLittleEndian64(sector + 24);
            const u64 first_usable_lba = LoadLittleEndian64(sector + 40);
            const u64 last_usable_lba = LoadLittleEndian64(sector + 48);
            const u64 partition_entry_lba = LoadLittleEndian64(sector + 72);
            const u32 partition_entry_count = LoadLittleEndian32(sector + 80);
            const u32 partition_entry_size = LoadLittleEndian32(sector + 84);

            R_UNLESS(header_size >= GptHeaderSize && header_size <= sector_size, usb::ResultCorruptedFilesystem());
            R_UNLESS(current_lba == 1 && first_usable_lba <= last_usable_lba, usb::ResultCorruptedFilesystem());
            R_UNLESS(partition_entry_lba < device->GetGeometry().block_count, usb::ResultCorruptedFilesystem());
            R_UNLESS(partition_entry_count > 0 && partition_entry_count <= 4096, usb::ResultCorruptedFilesystem());
            R_UNLESS(partition_entry_size >= GptPartitionEntrySize && partition_entry_size <= sector_size && sector_size % partition_entry_size == 0 && util::IsPowerOfTwo(partition_entry_size), usb::ResultCorruptedFilesystem());

            u64 cached_entry_lba = InvalidLba;
            for (u32 i = 0; i < partition_entry_count; ++i) {
                const u64 entry_offset = static_cast<u64>(i) * partition_entry_size;
                const u64 entry_lba_offset = entry_offset / sector_size;
                R_UNLESS(partition_entry_lba <= UINT64_MAX - entry_lba_offset, usb::ResultCorruptedFilesystem());
                const u64 entry_lba = partition_entry_lba + entry_lba_offset;
                R_UNLESS(entry_lba < device->GetGeometry().block_count, usb::ResultCorruptedFilesystem());
                const u32 entry_offset_in_sector = static_cast<u32>(entry_offset % sector_size);
                R_UNLESS(entry_offset_in_sector + partition_entry_size <= sector_size, usb::ResultCorruptedFilesystem());

                if (entry_lba != cached_entry_lba) {
                    R_TRY(device->ReadBlocks(entry_lba, 1, sector, sector_size));
                    cached_entry_lba = entry_lba;
                }

                const u8 *entry = sector + entry_offset_in_sector;
                if (IsZeroGuid(entry)) {
                    continue;
                }

                const u64 first_lba = LoadLittleEndian64(entry + 32);
                const u64 last_lba = LoadLittleEndian64(entry + 40);
                R_UNLESS(first_lba <= last_lba && last_lba < device->GetGeometry().block_count, usb::ResultCorruptedFilesystem());

                Partition partition{};
                partition.first_lba = first_lba;
                partition.sector_count = last_lba - first_lba + 1;
                partition.type = 0;
                partition.bootable = false;
                partition.style = PartitionStyle::Gpt;
                CopyAsciiName(partition.name, sizeof(partition.name), entry + GptPartitionNameOffset, GptPartitionNameSize);
                R_TRY(AppendPartition(out, out_count, out_found, partition));
            }
            if (*out_found > 0) {
                R_SUCCEED();
            }

            R_SUCCEED();
        }

    }

    Result DiscoverPartitions(mass_storage::Device *device, Partition *out, size_t out_count, size_t *out_found) {
        R_UNLESS(device != nullptr && device->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(out != nullptr && out_found != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(out_count > 0, usb::ResultInvalidParameter());
        R_UNLESS(IsValidSectorSize(device->GetGeometry().block_size), usb::ResultOperationDenied());

        *out_found = 0;
        alignas(0x1000) u8 sector[0x1000]{};
        const u32 sector_size = device->GetGeometry().block_size;
        R_TRY(device->ReadBlocks(0, 1, sector, sector_size));

        /* A super-floppy has its filesystem boot sector at LBA 0 and no MBR
         * partition table. exFAT is especially easy to misidentify because
         * it also ends its boot sector with 0xAA55. Check it before treating
         * the signature as an MBR. */
        if (IsSuperFloppyBootSector(sector, sector_size)) {
            Partition partition{};
            partition.first_lba = 0;
            partition.sector_count = device->GetGeometry().block_count;
            partition.type = 0;
            partition.bootable = false;
            partition.style = PartitionStyle::SuperFloppy;
            std::snprintf(partition.name, sizeof(partition.name), "superfloppy");
            R_TRY(AppendPartition(out, out_count, out_found, partition));
            R_SUCCEED();
        }

        const bool has_mbr_signature = LoadLittleEndian16(sector + Fat32BootSignatureOffset) == MbrSignature;
        bool has_protective_gpt = false;
        if (has_mbr_signature) {
            for (size_t i = 0; i < 4; ++i) {
                const u8 *entry = sector + MbrPartitionTableOffset + i * MbrPartitionEntrySize;
                if (entry[4] == MbrProtectiveGptType) {
                    has_protective_gpt = true;
                    break;
                }
            }
        }

        if (has_protective_gpt) {
            R_TRY(DiscoverGptPartitions(device, out, out_count, out_found, sector, sector_size));
            R_SUCCEED();
        }

        if (has_mbr_signature) {
            for (size_t i = 0; i < 4; ++i) {
                const u8 *entry = sector + MbrPartitionTableOffset + i * MbrPartitionEntrySize;
                const u8 type = entry[4];
                const u64 first_lba = LoadLittleEndian32(entry + 8);
                const u64 sector_count = LoadLittleEndian32(entry + 12);
                if (type == 0 || sector_count == 0) {
                    continue;
                }
                /* Ignore bytes that merely resemble a partition entry in a
                 * filesystem boot sector. A malformed candidate must not
                 * prevent the later super-floppy fallback from running. */
                if (first_lba >= device->GetGeometry().block_count || sector_count > device->GetGeometry().block_count - first_lba) {
                    continue;
                }

                Partition partition{};
                partition.first_lba = first_lba;
                partition.sector_count = sector_count;
                partition.type = type;
                partition.bootable = entry[0] == 0x80;
                partition.style = PartitionStyle::Mbr;
                std::snprintf(partition.name, sizeof(partition.name), "mbr%zu", i);
                R_TRY(AppendPartition(out, out_count, out_found, partition));
            }
            if (*out_found > 0) {
                R_SUCCEED();
            }
        }

        if (has_mbr_signature && LoadLittleEndian16(sector + Fat32BootSignatureOffset) == Fat32BootSignature) {
            Partition partition{};
            partition.first_lba = 0;
            partition.sector_count = device->GetGeometry().block_count;
            partition.type = 0;
            partition.bootable = false;
            partition.style = PartitionStyle::SuperFloppy;
            std::snprintf(partition.name, sizeof(partition.name), "superfloppy");
            R_TRY(AppendPartition(out, out_count, out_found, partition));
        }

        R_UNLESS(*out_found > 0, usb::ResultFileNotFound());
        R_SUCCEED();
    }

    Fat32Volume::~Fat32Volume() {
        if (m_is_initialized) {
            static_cast<void>(this->Finalize());
        }
    }

    Result Fat32Volume::Initialize(mass_storage::Device *device, const Partition &partition) {
        R_UNLESS(!m_is_initialized, usb::ResultAlreadyInitialized());
        R_UNLESS(device != nullptr && device->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(partition.sector_count > 0, usb::ResultInvalidParameter());
        R_UNLESS(IsValidSectorSize(device->GetGeometry().block_size), usb::ResultOperationDenied());

        R_TRY(device->ReadBlocks(partition.first_lba, 1, m_sector_buffer, device->GetGeometry().block_size));
        R_UNLESS(LoadLittleEndian16(m_sector_buffer + Fat32BootSignatureOffset) == Fat32BootSignature, usb::ResultUnsupportedFilesystem());

        const u32 bytes_per_sector = LoadLittleEndian16(m_sector_buffer + 0x0B);
        const u32 sectors_per_cluster = m_sector_buffer[0x0D];
        const u32 reserved_sector_count = LoadLittleEndian16(m_sector_buffer + 0x0E);
        const u32 fat_count = m_sector_buffer[0x10];
        const u32 root_entry_count = LoadLittleEndian16(m_sector_buffer + 0x11);
        const u32 total_sectors_16 = LoadLittleEndian16(m_sector_buffer + 0x13);
        const u32 fat_size_16 = LoadLittleEndian16(m_sector_buffer + 0x16);
        const u64 total_sectors_32 = LoadLittleEndian32(m_sector_buffer + 0x20);
        const u32 fat_size_32 = LoadLittleEndian32(m_sector_buffer + 0x24);
        const u32 root_cluster = LoadLittleEndian32(m_sector_buffer + 0x2C);

        R_UNLESS(bytes_per_sector == device->GetGeometry().block_size && IsValidSectorSize(bytes_per_sector), usb::ResultUnsupportedFilesystem());
        R_UNLESS(sectors_per_cluster > 0 && util::IsPowerOfTwo(sectors_per_cluster), usb::ResultCorruptedFilesystem());
        R_UNLESS(reserved_sector_count > 0 && fat_count > 0 && root_entry_count == 0, usb::ResultCorruptedFilesystem());
        R_UNLESS(fat_size_16 == 0 && fat_size_32 > 0, usb::ResultUnsupportedFilesystem());

        const u64 total_sectors = total_sectors_16 != 0 ? total_sectors_16 : total_sectors_32;
        R_UNLESS(total_sectors > 0 && total_sectors <= partition.sector_count, usb::ResultCorruptedFilesystem());

        const u64 fat_area_sectors = static_cast<u64>(fat_count) * fat_size_32;
        R_UNLESS(reserved_sector_count <= total_sectors, usb::ResultCorruptedFilesystem());
        R_UNLESS(fat_area_sectors <= total_sectors - reserved_sector_count, usb::ResultCorruptedFilesystem());
        const u64 first_data_offset = static_cast<u64>(reserved_sector_count) + fat_area_sectors;
        R_UNLESS(first_data_offset < total_sectors, usb::ResultCorruptedFilesystem());
        const u64 first_fat_lba = partition.first_lba + reserved_sector_count;
        const u64 first_data_lba = partition.first_lba + first_data_offset;
        R_UNLESS(first_data_lba >= partition.first_lba && first_data_lba < partition.first_lba + total_sectors, usb::ResultCorruptedFilesystem());

        const u64 data_sectors = total_sectors - first_data_offset;
        const u64 cluster_count = data_sectors / sectors_per_cluster;
        R_UNLESS(cluster_count > 0 && cluster_count <= 0x0FFFFFF5, usb::ResultUnsupportedFilesystem());
        R_UNLESS(IsValidClusterNumber(root_cluster, static_cast<u32>(cluster_count)), usb::ResultCorruptedFilesystem());

        m_device = device;
        m_partition = partition;
        m_bytes_per_sector = bytes_per_sector;
        m_sectors_per_cluster = sectors_per_cluster;
        m_reserved_sector_count = reserved_sector_count;
        m_fat_count = fat_count;
        m_fat_size_sectors = fat_size_32;
        m_root_cluster = root_cluster;
        m_cluster_count = static_cast<u32>(cluster_count);
        m_first_fat_lba = first_fat_lba;
        m_first_data_lba = first_data_lba;
        m_is_initialized = true;
        R_SUCCEED();
    }

    Result Fat32Volume::Finalize() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        m_device = nullptr;
        m_partition = {};
        m_bytes_per_sector = 0;
        m_sectors_per_cluster = 0;
        m_reserved_sector_count = 0;
        m_fat_count = 0;
        m_fat_size_sectors = 0;
        m_root_cluster = 0;
        m_cluster_count = 0;
        m_first_fat_lba = 0;
        m_first_data_lba = 0;
        m_is_initialized = false;
        R_SUCCEED();
    }

    Result Fat32Volume::ReadRelativeSectors(u64 lba, u32 sector_count, void *buffer, size_t buffer_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(buffer != nullptr && sector_count > 0, usb::ResultInvalidParameter());
        R_UNLESS(sector_count <= SIZE_MAX / m_bytes_per_sector, usb::ResultInvalidParameter());
        R_UNLESS(buffer_size == static_cast<size_t>(sector_count) * m_bytes_per_sector, usb::ResultInvalidParameter());
        R_UNLESS(lba < m_partition.sector_count && sector_count <= m_partition.sector_count - lba, usb::ResultInvalidParameter());
        R_RETURN(m_device->ReadBlocks(m_partition.first_lba + lba, sector_count, buffer, buffer_size));
    }

    Result Fat32Volume::ReadFatEntry(u32 cluster, u32 *out_next_cluster) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_next_cluster != nullptr && IsValidClusterNumber(cluster, m_cluster_count), usb::ResultInvalidParameter());

        const u64 fat_offset = static_cast<u64>(cluster) * sizeof(u32);
        const u64 fat_sector = fat_offset / m_bytes_per_sector;
        const u32 fat_offset_in_sector = static_cast<u32>(fat_offset % m_bytes_per_sector);
        R_UNLESS(fat_offset_in_sector + sizeof(u32) <= m_bytes_per_sector, usb::ResultCorruptedFilesystem());
        R_TRY(m_device->ReadBlocks(m_first_fat_lba + fat_sector, 1, m_sector_buffer, m_bytes_per_sector));

        const u32 next_cluster = LoadLittleEndian32(m_sector_buffer + fat_offset_in_sector) & 0x0FFFFFFF;
        R_UNLESS(next_cluster != Fat32BadCluster, usb::ResultCorruptedFilesystem());
        if (next_cluster < Fat32EndOfChain) {
            R_UNLESS(IsValidClusterNumber(next_cluster, m_cluster_count), usb::ResultCorruptedFilesystem());
        }
        *out_next_cluster = next_cluster;
        R_SUCCEED();
    }

    Result Fat32Volume::GetClusterLba(u32 cluster, u64 *out_lba) const {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_lba != nullptr && IsValidClusterNumber(cluster, m_cluster_count), usb::ResultInvalidParameter());
        *out_lba = m_first_data_lba + static_cast<u64>(cluster - 2) * m_sectors_per_cluster;
        R_UNLESS(*out_lba >= m_partition.first_lba && *out_lba < m_partition.first_lba + m_partition.sector_count, usb::ResultCorruptedFilesystem());
        R_SUCCEED();
    }

    Result Fat32Volume::ReadDirectoryCluster(u32 cluster, const char *name, DirectoryEntry *out, bool *out_found) {
        R_UNLESS(name != nullptr && out != nullptr && out_found != nullptr, usb::ResultInvalidParameter());
        *out_found = false;

        u64 cluster_lba = 0;
        R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));

        char long_name[MaxFileNameLength + 1]{};
        bool long_name_valid = false;
        for (u32 sector = 0; sector < m_sectors_per_cluster; ++sector) {
            R_TRY(m_device->ReadBlocks(cluster_lba + sector, 1, m_sector_buffer, m_bytes_per_sector));
            for (u32 offset = 0; offset + 32 <= m_bytes_per_sector; offset += 32) {
                const u8 *entry = m_sector_buffer + offset;
                if (entry[0] == FatDirectoryEntryEnd) {
                    R_SUCCEED();
                }
                if (entry[0] == FatDirectoryEntryDeleted) {
                    ResetLongName(long_name, std::addressof(long_name_valid));
                    continue;
                }

                const u8 attributes = entry[11];
                if (attributes == FatDirectoryAttributeLongName) {
                    const u8 sequence = entry[0] & 0x1F;
                    if ((entry[0] & 0x40) != 0) {
                        ResetLongName(long_name, std::addressof(long_name_valid));
                        long_name_valid = sequence > 0;
                    }
                    if (long_name_valid && sequence > 0) {
                        AddLongNamePart(long_name, sizeof(long_name), sequence, entry);
                    }
                    continue;
                }

                if ((attributes & FatDirectoryAttributeVolumeId) != 0) {
                    ResetLongName(long_name, std::addressof(long_name_valid));
                    continue;
                }

                char short_name[MaxFileNameLength + 1]{};
                AddShortName(short_name, sizeof(short_name), entry);
                const char *entry_name = long_name_valid && long_name[0] != '\0' ? long_name : short_name;
                if (AsciiEqualInsensitive(entry_name, name)) {
                    std::strncpy(out->name, entry_name, sizeof(out->name) - 1);
                    out->name[sizeof(out->name) - 1] = '\0';
                    out->first_cluster = (static_cast<u32>(LoadLittleEndian16(entry + 20)) << 16) | LoadLittleEndian16(entry + 26);
                    out->size = LoadLittleEndian32(entry + 28);
                    out->directory = (attributes & 0x10) != 0;
                    *out_found = true;
                    return ResultSuccess();
                }
                ResetLongName(long_name, std::addressof(long_name_valid));
            }
        }
        R_SUCCEED();
    }

    Result Fat32Volume::FindInDirectory(u32 directory_cluster, const char *name, DirectoryEntry *out) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(name != nullptr && name[0] != '\0' && out != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(IsValidClusterNumber(directory_cluster, m_cluster_count), usb::ResultCorruptedFilesystem());

        u32 cluster = directory_cluster;
        u32 cluster_hops = 0;
        while (true) {
            R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
            bool found = false;
            R_TRY(this->ReadDirectoryCluster(cluster, name, out, std::addressof(found)));
            if (found) {
                R_SUCCEED();
            }

            u32 next_cluster = 0;
            R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
            R_UNLESS(next_cluster < Fat32EndOfChain, usb::ResultFileNotFound());
            cluster = next_cluster;
        }
    }

    Result Fat32Volume::ResolvePath(const char *path, DirectoryEntry *out) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(path != nullptr && out != nullptr, usb::ResultInvalidParameter());

        const char *cursor = path;
        while (*cursor == '/' || *cursor == '\\') {
            ++cursor;
        }
        R_UNLESS(*cursor != '\0', usb::ResultInvalidParameter());

        u32 directory_cluster = m_root_cluster;
        while (*cursor != '\0') {
            char component[MaxFileNameLength + 1]{};
            size_t length = 0;
            while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
                R_UNLESS(length + 1 < sizeof(component), usb::ResultInvalidParameter());
                component[length++] = *cursor++;
            }
            component[length] = '\0';
            while (*cursor == '/' || *cursor == '\\') {
                ++cursor;
            }
            if (length == 0) {
                continue;
            }

            DirectoryEntry entry{};
            R_TRY(this->FindInDirectory(directory_cluster, component, std::addressof(entry)));
            if (*cursor == '\0') {
                *out = entry;
                R_SUCCEED();
            }
            R_UNLESS(entry.directory && IsValidClusterNumber(entry.first_cluster, m_cluster_count), usb::ResultOperationDenied());
            directory_cluster = entry.first_cluster;
        }
        R_RETURN(usb::ResultFileNotFound());
    }

    Result Fat32Volume::GetFileInfo(const char *path, FileInfo *out) {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        DirectoryEntry entry{};
        R_TRY(this->ResolvePath(path, std::addressof(entry)));
        out->size = entry.size;
        out->first_cluster = entry.first_cluster;
        out->directory = entry.directory;
        R_SUCCEED();
    }

    Result Fat32Volume::ReadFileData(const DirectoryEntry &entry, u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_UNLESS(out_read != nullptr, usb::ResultInvalidParameter());
        *out_read = 0;
        R_UNLESS(!entry.directory && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
        R_UNLESS(offset <= entry.size, usb::ResultInvalidParameter());

        const u64 requested_size = std::min<u64>(size, entry.size - offset);
        if (requested_size == 0) {
            R_SUCCEED();
        }
        R_UNLESS(IsValidClusterNumber(entry.first_cluster, m_cluster_count), usb::ResultCorruptedFilesystem());

        const u64 cluster_size = static_cast<u64>(m_bytes_per_sector) * m_sectors_per_cluster;
        u32 cluster = entry.first_cluster;
        u32 cluster_hops = 0;
        u64 clusters_to_skip = offset / cluster_size;
        while (clusters_to_skip > 0) {
            R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
            u32 next_cluster = 0;
            R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
            R_UNLESS(next_cluster < Fat32EndOfChain, usb::ResultCorruptedFilesystem());
            cluster = next_cluster;
            --clusters_to_skip;
        }

        u64 current_offset = offset;
        u64 remaining = requested_size;
        u8 *dst = static_cast<u8 *>(buffer);
        while (remaining > 0) {
            u64 cluster_lba = 0;
            R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));
            const u32 sector_in_cluster = static_cast<u32>((current_offset % cluster_size) / m_bytes_per_sector);
            const u32 offset_in_sector = static_cast<u32>(current_offset % m_bytes_per_sector);
            const u32 sectors_left = m_sectors_per_cluster - sector_in_cluster;

            for (u32 i = 0; i < sectors_left && remaining > 0; ++i) {
                R_TRY(this->ReadRelativeSectors(cluster_lba + sector_in_cluster + i, 1, m_sector_buffer, m_bytes_per_sector));
                const u64 copy_size = std::min<u64>(remaining, m_bytes_per_sector - (i == 0 ? offset_in_sector : 0));
                std::memcpy(dst, m_sector_buffer + (i == 0 ? offset_in_sector : 0), static_cast<size_t>(copy_size));
                dst += copy_size;
                current_offset += copy_size;
                remaining -= copy_size;
                *out_read += static_cast<size_t>(copy_size);
            }

            if (remaining > 0) {
                R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
                u32 next_cluster = 0;
                R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
                R_UNLESS(next_cluster < Fat32EndOfChain, usb::ResultCorruptedFilesystem());
                cluster = next_cluster;
            }
        }
        R_SUCCEED();
    }

    Result Fat32Volume::ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        DirectoryEntry entry{};
        R_TRY(this->ResolvePath(path, std::addressof(entry)));
        R_RETURN(this->ReadFileData(entry, offset, buffer, size, out_read));
    }

    Result Fat32Volume::ReadDirectory(const char *path, DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(path != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(max_entries == 0 || out != nullptr, usb::ResultInvalidParameter());

        *out_count = 0;
        if (!include_directories && !include_files) {
            R_SUCCEED();
        }

        u32 directory_cluster = m_root_cluster;
        const char *cursor = path;
        while (*cursor == '/' || *cursor == '\\') {
            ++cursor;
        }
        if (*cursor != '\0') {
            DirectoryEntry directory{};
            R_TRY(this->ResolvePath(path, std::addressof(directory)));
            R_UNLESS(directory.directory && IsValidClusterNumber(directory.first_cluster, m_cluster_count), usb::ResultOperationDenied());
            directory_cluster = directory.first_cluster;
        }

        char long_name[MaxFileNameLength + 1]{};
        bool long_name_valid = false;
        size_t skipped = 0;
        size_t returned = 0;
        size_t total = 0;
        u32 cluster = directory_cluster;
        u32 cluster_hops = 0;
        while (true) {
            R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
            u64 cluster_lba = 0;
            R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));
            for (u32 sector = 0; sector < m_sectors_per_cluster; ++sector) {
                R_TRY(m_device->ReadBlocks(cluster_lba + sector, 1, m_sector_buffer, m_bytes_per_sector));
                for (u32 offset = 0; offset + 32 <= m_bytes_per_sector; offset += 32) {
                    const u8 *entry = m_sector_buffer + offset;
                    if (entry[0] == FatDirectoryEntryEnd) {
                        *out_count = max_entries == 0 ? total : returned;
                        R_SUCCEED();
                    }
                    if (entry[0] == FatDirectoryEntryDeleted) {
                        ResetLongName(long_name, std::addressof(long_name_valid));
                        continue;
                    }

                    const u8 attributes = entry[11];
                    if (attributes == FatDirectoryAttributeLongName) {
                        const u8 sequence = entry[0] & 0x1F;
                        if ((entry[0] & 0x40) != 0) {
                            ResetLongName(long_name, std::addressof(long_name_valid));
                            long_name_valid = sequence > 0;
                        }
                        if (long_name_valid && sequence > 0) {
                            AddLongNamePart(long_name, sizeof(long_name), sequence, entry);
                        }
                        continue;
                    }

                    if ((attributes & FatDirectoryAttributeVolumeId) != 0) {
                        ResetLongName(long_name, std::addressof(long_name_valid));
                        continue;
                    }

                    char short_name[MaxFileNameLength + 1]{};
                    AddShortName(short_name, sizeof(short_name), entry);
                    const char *entry_name = long_name_valid && long_name[0] != '\0' ? long_name : short_name;
                    const bool directory = (attributes & 0x10) != 0;
                    if ((directory && include_directories) || (!directory && include_files)) {
                        if (skipped < skip_entries) {
                            ++skipped;
                        } else {
                            ++total;
                            if (max_entries == 0) {
                                /* Count-only query. */
                            } else {
                                std::strncpy(out[returned].name, entry_name, sizeof(out[returned].name) - 1);
                                out[returned].name[sizeof(out[returned].name) - 1] = '\0';
                                out[returned].size = LoadLittleEndian32(entry + 28);
                                out[returned].directory = directory;
                                ++returned;
                                if (returned == max_entries) {
                                    *out_count = returned;
                                    R_SUCCEED();
                                }
                            }
                        }
                    }
                    ResetLongName(long_name, std::addressof(long_name_valid));
                }
            }

            u32 next_cluster = 0;
            R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
            if (next_cluster >= Fat32EndOfChain) {
                *out_count = max_entries == 0 ? total : returned;
                R_SUCCEED();
            }
            cluster = next_cluster;
        }
    }

    Result Fat32Volume::GetSpaceSize(u64 *out_free_size, u64 *out_total_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_free_size != nullptr && out_total_size != nullptr, usb::ResultInvalidParameter());

        const u64 cluster_size = static_cast<u64>(m_bytes_per_sector) * m_sectors_per_cluster;
        R_UNLESS(cluster_size > 0 && m_cluster_count <= UINT64_MAX / cluster_size, usb::ResultCorruptedFilesystem());

        const u64 total_size = static_cast<u64>(m_cluster_count) * cluster_size;
        const u64 entries_per_sector = m_bytes_per_sector / sizeof(u32);
        const u64 fat_entry_count = static_cast<u64>(m_cluster_count) + 2;
        const u64 fat_sector_count = (fat_entry_count + entries_per_sector - 1) / entries_per_sector;
        R_UNLESS(fat_sector_count <= m_fat_size_sectors, usb::ResultCorruptedFilesystem());

        u64 free_clusters = 0;
        for (u64 fat_sector = 0; fat_sector < fat_sector_count; ++fat_sector) {
            R_TRY(m_device->ReadBlocks(m_first_fat_lba + fat_sector, 1, m_sector_buffer, m_bytes_per_sector));

            const u64 first_cluster = fat_sector * entries_per_sector;
            const u64 last_cluster = std::min<u64>(fat_entry_count, first_cluster + entries_per_sector);
            for (u64 cluster = std::max<u64>(2, first_cluster); cluster < last_cluster; ++cluster) {
                const u32 entry_offset = static_cast<u32>((cluster - first_cluster) * sizeof(u32));
                if ((LoadLittleEndian32(m_sector_buffer + entry_offset) & 0x0FFFFFFF) == 0) {
                    ++free_clusters;
                }
            }
        }

        R_UNLESS(free_clusters <= UINT64_MAX / cluster_size, usb::ResultCorruptedFilesystem());
        *out_total_size = total_size;
        *out_free_size = free_clusters * cluster_size;
        R_SUCCEED();
    }

    namespace {

        constexpr u32 ExFatEndOfChain = 0xFFFFFFF8;
        constexpr u32 ExFatBadCluster = 0xFFFFFFF7;
        constexpr u8 ExFatFileEntry = 0x85;
        constexpr u8 ExFatStreamExtensionEntry = 0xC0;
        constexpr u8 ExFatFileNameEntry = 0xC1;
        constexpr u8 ExFatAllocationBitmapEntry = 0x81;

        void AppendExFatName(char *name, size_t name_size, size_t *in_out_length, const u8 *entry) {
            for (size_t i = 0; i < 15; ++i) {
                const u16 code = LoadLittleEndian16(entry + 2 + i * sizeof(u16));
                if (code == 0 || code == 0xFFFF) {
                    break;
                }
                if (*in_out_length + 1 >= name_size) {
                    break;
                }
                name[(*in_out_length)++] = code < 0x80 ? static_cast<char>(code) : '_';
                name[*in_out_length] = '\0';
            }
        }

    }

    ExFatVolume::~ExFatVolume() {
        if (m_is_initialized) {
            static_cast<void>(this->Finalize());
        }
    }

    Result ExFatVolume::Initialize(mass_storage::Device *device, const Partition &partition) {
        R_UNLESS(!m_is_initialized, usb::ResultAlreadyInitialized());
        R_UNLESS(device != nullptr && device->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(partition.sector_count > 0, usb::ResultInvalidParameter());
        R_UNLESS(IsValidSectorSize(device->GetGeometry().block_size), usb::ResultOperationDenied());

        R_TRY(device->ReadBlocks(partition.first_lba, 1, m_sector_buffer, device->GetGeometry().block_size));
        R_UNLESS(std::memcmp(m_sector_buffer + 3, "EXFAT   ", 8) == 0, usb::ResultUnsupportedFilesystem());
        R_UNLESS(LoadLittleEndian16(m_sector_buffer + 510) == Fat32BootSignature, usb::ResultCorruptedFilesystem());

        const u64 volume_length = LoadLittleEndian64(m_sector_buffer + 0x48);
        const u32 fat_offset = LoadLittleEndian32(m_sector_buffer + 0x50);
        const u32 fat_length = LoadLittleEndian32(m_sector_buffer + 0x54);
        const u32 cluster_heap_offset = LoadLittleEndian32(m_sector_buffer + 0x58);
        const u32 cluster_count = LoadLittleEndian32(m_sector_buffer + 0x5C);
        const u32 root_cluster = LoadLittleEndian32(m_sector_buffer + 0x60);
        const u8 bytes_per_sector_shift = m_sector_buffer[0x6C];
        const u8 sectors_per_cluster_shift = m_sector_buffer[0x6D];

        R_UNLESS(bytes_per_sector_shift >= 9 && bytes_per_sector_shift <= 12, usb::ResultUnsupportedFilesystem());
        R_UNLESS(sectors_per_cluster_shift < 31, usb::ResultUnsupportedFilesystem());
        const u32 bytes_per_sector = 1u << bytes_per_sector_shift;
        const u32 sectors_per_cluster = 1u << sectors_per_cluster_shift;
        R_UNLESS(bytes_per_sector == device->GetGeometry().block_size && IsValidSectorSize(bytes_per_sector), usb::ResultUnsupportedFilesystem());
        R_UNLESS(volume_length > 0 && volume_length <= partition.sector_count, usb::ResultCorruptedFilesystem());
        R_UNLESS(fat_offset > 0 && fat_length > 0 && fat_offset <= volume_length && fat_length <= volume_length - fat_offset, usb::ResultCorruptedFilesystem());
        R_UNLESS(cluster_heap_offset >= fat_offset && cluster_heap_offset - fat_offset >= fat_length && cluster_heap_offset < volume_length, usb::ResultCorruptedFilesystem());
        R_UNLESS(cluster_count > 0 && cluster_count <= (volume_length - cluster_heap_offset) / sectors_per_cluster, usb::ResultCorruptedFilesystem());
        R_UNLESS(IsValidClusterNumber(root_cluster, cluster_count), usb::ResultCorruptedFilesystem());

        m_device = device;
        m_partition = partition;
        m_bytes_per_sector = bytes_per_sector;
        m_sectors_per_cluster = sectors_per_cluster;
        m_fat_offset = fat_offset;
        m_fat_length = fat_length;
        m_cluster_heap_offset = cluster_heap_offset;
        m_root_cluster = root_cluster;
        m_cluster_count = cluster_count;
        m_is_initialized = true;

        const auto bitmap_result = this->FindAllocationBitmap();
        if (R_FAILED(bitmap_result)) {
            static_cast<void>(this->Finalize());
            R_RETURN(bitmap_result);
        }

        R_SUCCEED();
    }

    Result ExFatVolume::Finalize() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        m_device = nullptr;
        m_partition = {};
        m_bytes_per_sector = 0;
        m_sectors_per_cluster = 0;
        m_fat_offset = 0;
        m_fat_length = 0;
        m_cluster_heap_offset = 0;
        m_root_cluster = 0;
        m_cluster_count = 0;
        m_allocation_bitmap_cluster = 0;
        m_allocation_bitmap_size = 0;
        m_allocation_bitmap_no_fat_chain = false;
        m_is_initialized = false;
        R_SUCCEED();
    }

    Result ExFatVolume::ReadRelativeSectors(u64 lba, u32 sector_count, void *buffer, size_t buffer_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(buffer != nullptr && sector_count > 0, usb::ResultInvalidParameter());
        R_UNLESS(sector_count <= SIZE_MAX / m_bytes_per_sector, usb::ResultInvalidParameter());
        R_UNLESS(buffer_size == static_cast<size_t>(sector_count) * m_bytes_per_sector, usb::ResultInvalidParameter());
        R_UNLESS(lba < m_partition.sector_count && sector_count <= m_partition.sector_count - lba, usb::ResultInvalidParameter());
        R_RETURN(m_device->ReadBlocks(m_partition.first_lba + lba, sector_count, buffer, buffer_size));
    }

    Result ExFatVolume::ReadFatEntry(u32 cluster, u32 *out_next_cluster) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_next_cluster != nullptr && IsValidClusterNumber(cluster, m_cluster_count), usb::ResultInvalidParameter());

        const u64 fat_offset = static_cast<u64>(cluster) * sizeof(u32);
        const u64 fat_sector = fat_offset / m_bytes_per_sector;
        const u32 fat_offset_in_sector = static_cast<u32>(fat_offset % m_bytes_per_sector);
        R_UNLESS(fat_sector < m_fat_length && fat_offset_in_sector + sizeof(u32) <= m_bytes_per_sector, usb::ResultCorruptedFilesystem());
        R_TRY(this->ReadRelativeSectors(m_fat_offset + fat_sector, 1, m_sector_buffer, m_bytes_per_sector));

        const u32 next_cluster = LoadLittleEndian32(m_sector_buffer + fat_offset_in_sector);
        R_UNLESS(next_cluster != ExFatBadCluster, usb::ResultCorruptedFilesystem());
        if (next_cluster < ExFatEndOfChain) {
            R_UNLESS(IsValidClusterNumber(next_cluster, m_cluster_count), usb::ResultCorruptedFilesystem());
        }
        *out_next_cluster = next_cluster;
        R_SUCCEED();
    }

    Result ExFatVolume::FindAllocationBitmap() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());

        m_allocation_bitmap_cluster = 0;
        m_allocation_bitmap_size = 0;
        m_allocation_bitmap_no_fat_chain = false;

        u32 cluster = m_root_cluster;
        u32 cluster_hops = 0;
        while (true) {
            R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
            u64 cluster_lba = 0;
            R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));

            for (u32 sector = 0; sector < m_sectors_per_cluster; ++sector) {
                R_TRY(this->ReadRelativeSectors(cluster_lba - m_partition.first_lba + sector, 1, m_sector_buffer, m_bytes_per_sector));
                for (u32 offset = 0; offset + 32 <= m_bytes_per_sector; offset += 32) {
                    const u8 *entry = m_sector_buffer + offset;
                    if (entry[0] == 0x00) {
                        R_RETURN(usb::ResultCorruptedFilesystem());
                    }
                    if (entry[0] != ExFatAllocationBitmapEntry || (entry[1] & 0x01) != 0) {
                        continue;
                    }

                    m_allocation_bitmap_cluster = LoadLittleEndian32(entry + 20);
                    m_allocation_bitmap_size = LoadLittleEndian64(entry + 24);
                    m_allocation_bitmap_no_fat_chain = true;
                    R_UNLESS(IsValidClusterNumber(m_allocation_bitmap_cluster, m_cluster_count), usb::ResultCorruptedFilesystem());
                    R_SUCCEED();
                }
            }

            u32 next_cluster = 0;
            R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
            R_UNLESS(next_cluster < ExFatEndOfChain, usb::ResultCorruptedFilesystem());
            cluster = next_cluster;
        }
    }

    Result ExFatVolume::GetClusterLba(u32 cluster, u64 *out_lba) const {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_lba != nullptr && IsValidClusterNumber(cluster, m_cluster_count), usb::ResultInvalidParameter());
        *out_lba = m_cluster_heap_offset + static_cast<u64>(cluster - 2) * m_sectors_per_cluster;
        R_UNLESS(*out_lba < m_partition.sector_count && m_sectors_per_cluster <= m_partition.sector_count - *out_lba, usb::ResultCorruptedFilesystem());
        *out_lba += m_partition.first_lba;
        R_SUCCEED();
    }

    Result ExFatVolume::ReadDirectoryCluster(u32 cluster, const char *name, DirectoryEntry *out, bool *out_found) {
        R_UNLESS(name != nullptr && out != nullptr && out_found != nullptr, usb::ResultInvalidParameter());
        *out_found = false;

        struct SearchState {
            bool active{false};
            bool has_stream{false};
            u32 remaining{0};
            u32 first_cluster{0};
            u64 size{0};
            bool directory{false};
            bool no_fat_chain{false};
            size_t name_length{0};
            size_t expected_name_length{0};
            char name[MaxFileNameLength + 1]{};
        } state;

        u64 cluster_lba = 0;
        R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));
        for (u32 sector = 0; sector < m_sectors_per_cluster; ++sector) {
            R_TRY(this->ReadRelativeSectors(cluster_lba - m_partition.first_lba + sector, 1, m_sector_buffer, m_bytes_per_sector));
            for (u32 offset = 0; offset + 32 <= m_bytes_per_sector; offset += 32) {
                const u8 *entry = m_sector_buffer + offset;
                const u8 type = entry[0];
                if (type == 0x00) {
                    R_SUCCEED();
                }

                if (type == ExFatFileEntry) {
                    state = {};
                    state.active = true;
                    state.remaining = entry[1];
                    state.directory = (LoadLittleEndian16(entry + 4) & 0x10) != 0;
                    continue;
                }

                if (!state.active || state.remaining == 0) {
                    continue;
                }

                if (type == ExFatStreamExtensionEntry) {
                    state.has_stream = true;
                    state.no_fat_chain = (entry[1] & 0x02) != 0;
                    state.expected_name_length = std::min<size_t>(entry[3], MaxFileNameLength);
                    state.name_length = 0;
                    state.first_cluster = LoadLittleEndian32(entry + 20);
                    state.size = LoadLittleEndian64(entry + 24);
                } else if (type == ExFatFileNameEntry) {
                    AppendExFatName(state.name, sizeof(state.name), std::addressof(state.name_length), entry);
                }

                --state.remaining;
                if (state.remaining == 0) {
                    if (state.has_stream && AsciiEqualInsensitive(state.name, name)) {
                        std::strncpy(out->name, state.name, sizeof(out->name) - 1);
                        out->name[sizeof(out->name) - 1] = '\0';
                        out->first_cluster = state.first_cluster;
                        out->size = state.size;
                        out->directory = state.directory;
                        out->no_fat_chain = state.no_fat_chain;
                        *out_found = true;
                        return ResultSuccess();
                    }
                    state.active = false;
                }
            }
        }
        R_SUCCEED();
    }

    Result ExFatVolume::FindInDirectory(u32 directory_cluster, const char *name, DirectoryEntry *out) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(name != nullptr && name[0] != '\0' && out != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(IsValidClusterNumber(directory_cluster, m_cluster_count), usb::ResultCorruptedFilesystem());

        u32 cluster = directory_cluster;
        u32 cluster_hops = 0;
        while (true) {
            R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
            bool found = false;
            R_TRY(this->ReadDirectoryCluster(cluster, name, out, std::addressof(found)));
            if (found) {
                R_SUCCEED();
            }

            u32 next_cluster = 0;
            R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
            R_UNLESS(next_cluster < ExFatEndOfChain, usb::ResultFileNotFound());
            cluster = next_cluster;
        }
    }

    Result ExFatVolume::ResolvePath(const char *path, DirectoryEntry *out) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(path != nullptr && out != nullptr, usb::ResultInvalidParameter());

        const char *cursor = path;
        while (*cursor == '/' || *cursor == '\\') {
            ++cursor;
        }
        R_UNLESS(*cursor != '\0', usb::ResultInvalidParameter());

        u32 directory_cluster = m_root_cluster;
        while (*cursor != '\0') {
            char component[MaxFileNameLength + 1]{};
            size_t length = 0;
            while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
                R_UNLESS(length + 1 < sizeof(component), usb::ResultInvalidParameter());
                component[length++] = *cursor++;
            }
            component[length] = '\0';
            while (*cursor == '/' || *cursor == '\\') {
                ++cursor;
            }
            if (length == 0) {
                continue;
            }

            DirectoryEntry entry{};
            R_TRY(this->FindInDirectory(directory_cluster, component, std::addressof(entry)));
            if (*cursor == '\0') {
                *out = entry;
                R_SUCCEED();
            }
            R_UNLESS(entry.directory && IsValidClusterNumber(entry.first_cluster, m_cluster_count), usb::ResultOperationDenied());
            directory_cluster = entry.first_cluster;
        }
        R_RETURN(usb::ResultFileNotFound());
    }

    Result ExFatVolume::GetFileInfo(const char *path, FileInfo *out) {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        DirectoryEntry entry{};
        R_TRY(this->ResolvePath(path, std::addressof(entry)));
        out->size = entry.size;
        out->first_cluster = entry.first_cluster;
        out->directory = entry.directory;
        R_SUCCEED();
    }

    Result ExFatVolume::ReadFileData(const DirectoryEntry &entry, u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_UNLESS(out_read != nullptr, usb::ResultInvalidParameter());
        *out_read = 0;
        R_UNLESS(!entry.directory && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
        R_UNLESS(offset <= entry.size, usb::ResultInvalidParameter());

        const u64 requested_size = std::min<u64>(size, entry.size - offset);
        if (requested_size == 0) {
            R_SUCCEED();
        }
        R_UNLESS(IsValidClusterNumber(entry.first_cluster, m_cluster_count), usb::ResultCorruptedFilesystem());

        const u64 cluster_size = static_cast<u64>(m_bytes_per_sector) * m_sectors_per_cluster;
        u32 cluster = entry.first_cluster;
        u32 cluster_hops = 0;
        u64 clusters_to_skip = offset / cluster_size;
        while (clusters_to_skip > 0) {
            if (entry.no_fat_chain) {
                R_UNLESS(cluster < m_cluster_count + 1, usb::ResultCorruptedFilesystem());
                ++cluster;
            } else {
                R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
                u32 next_cluster = 0;
                R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
                R_UNLESS(next_cluster < ExFatEndOfChain, usb::ResultCorruptedFilesystem());
                cluster = next_cluster;
            }
            --clusters_to_skip;
        }

        u64 current_offset = offset;
        u64 remaining = requested_size;
        u8 *dst = static_cast<u8 *>(buffer);
        while (remaining > 0) {
            u64 cluster_lba = 0;
            R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));
            const u32 sector_in_cluster = static_cast<u32>((current_offset % cluster_size) / m_bytes_per_sector);
            const u32 offset_in_sector = static_cast<u32>(current_offset % m_bytes_per_sector);
            const u32 sectors_left = m_sectors_per_cluster - sector_in_cluster;

            for (u32 i = 0; i < sectors_left && remaining > 0; ++i) {
                R_TRY(this->ReadRelativeSectors(cluster_lba - m_partition.first_lba + sector_in_cluster + i, 1, m_sector_buffer, m_bytes_per_sector));
                const u64 copy_size = std::min<u64>(remaining, m_bytes_per_sector - (i == 0 ? offset_in_sector : 0));
                std::memcpy(dst, m_sector_buffer + (i == 0 ? offset_in_sector : 0), static_cast<size_t>(copy_size));
                dst += copy_size;
                current_offset += copy_size;
                remaining -= copy_size;
                *out_read += static_cast<size_t>(copy_size);
            }

            if (remaining > 0) {
                if (entry.no_fat_chain) {
                    R_UNLESS(cluster < m_cluster_count + 1, usb::ResultCorruptedFilesystem());
                    ++cluster;
                } else {
                    R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
                    u32 next_cluster = 0;
                    R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
                    R_UNLESS(next_cluster < ExFatEndOfChain, usb::ResultCorruptedFilesystem());
                    cluster = next_cluster;
                }
            }
        }
        R_SUCCEED();
    }

    Result ExFatVolume::ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        DirectoryEntry entry{};
        R_TRY(this->ResolvePath(path, std::addressof(entry)));
        R_RETURN(this->ReadFileData(entry, offset, buffer, size, out_read));
    }

    Result ExFatVolume::ReadDirectory(const char *path, DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(path != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(max_entries == 0 || out != nullptr, usb::ResultInvalidParameter());

        *out_count = 0;
        if (!include_directories && !include_files) {
            R_SUCCEED();
        }

        u32 directory_cluster = m_root_cluster;
        bool directory_no_fat_chain = false;
        const char *cursor = path;
        while (*cursor == '/' || *cursor == '\\') {
            ++cursor;
        }
        if (*cursor != '\0') {
            DirectoryEntry directory{};
            R_TRY(this->ResolvePath(path, std::addressof(directory)));
            R_UNLESS(directory.directory && IsValidClusterNumber(directory.first_cluster, m_cluster_count), usb::ResultOperationDenied());
            directory_cluster = directory.first_cluster;
            directory_no_fat_chain = directory.no_fat_chain;
        }

        struct SearchState {
            bool active{false};
            bool has_stream{false};
            u32 remaining{0};
            u32 first_cluster{0};
            u64 size{0};
            bool directory{false};
            bool no_fat_chain{false};
            size_t name_length{0};
            char name[MaxFileNameLength + 1]{};
        } state;

        size_t skipped = 0;
        size_t returned = 0;
        size_t total = 0;
        u32 cluster = directory_cluster;
        u32 cluster_hops = 0;
        while (true) {
            R_UNLESS(directory_no_fat_chain || cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
            u64 cluster_lba = 0;
            R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));
            for (u32 sector = 0; sector < m_sectors_per_cluster; ++sector) {
                R_TRY(this->ReadRelativeSectors(cluster_lba - m_partition.first_lba + sector, 1, m_sector_buffer, m_bytes_per_sector));
                for (u32 offset = 0; offset + 32 <= m_bytes_per_sector; offset += 32) {
                    const u8 *entry = m_sector_buffer + offset;
                    const u8 type = entry[0];
                    if (type == 0x00) {
                        *out_count = max_entries == 0 ? total : returned;
                        R_SUCCEED();
                    }

                    if (type == ExFatFileEntry) {
                        state = {};
                        state.active = true;
                        state.remaining = entry[1];
                        state.directory = (LoadLittleEndian16(entry + 4) & 0x10) != 0;
                        continue;
                    }

                    if (!state.active || state.remaining == 0) {
                        continue;
                    }

                    if (type == ExFatStreamExtensionEntry) {
                        state.has_stream = true;
                        state.no_fat_chain = (entry[1] & 0x02) != 0;
                        state.first_cluster = LoadLittleEndian32(entry + 20);
                        state.size = LoadLittleEndian64(entry + 24);
                    } else if (type == ExFatFileNameEntry) {
                        AppendExFatName(state.name, sizeof(state.name), std::addressof(state.name_length), entry);
                    }

                    --state.remaining;
                    if (state.remaining == 0) {
                        if (state.has_stream && state.name[0] != '\0' && ((state.directory && include_directories) || (!state.directory && include_files))) {
                            if (skipped < skip_entries) {
                                ++skipped;
                            } else {
                                ++total;
                                if (max_entries == 0) {
                                    /* Count-only query. */
                                } else {
                                    std::strncpy(out[returned].name, state.name, sizeof(out[returned].name) - 1);
                                    out[returned].name[sizeof(out[returned].name) - 1] = '\0';
                                    out[returned].size = state.size;
                                    out[returned].directory = state.directory;
                                    ++returned;
                                    if (returned == max_entries) {
                                        *out_count = returned;
                                        R_SUCCEED();
                                    }
                                }
                            }
                        }
                        state.active = false;
                    }
                }
            }

            if (directory_no_fat_chain) {
                R_UNLESS(cluster < m_cluster_count + 1, usb::ResultCorruptedFilesystem());
                ++cluster;
            } else {
                u32 next_cluster = 0;
                R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
                if (next_cluster >= ExFatEndOfChain) {
                    *out_count = max_entries == 0 ? total : returned;
                    R_SUCCEED();
                }
                cluster = next_cluster;
            }
        }
    }

    Result ExFatVolume::GetSpaceSize(u64 *out_free_size, u64 *out_total_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_free_size != nullptr && out_total_size != nullptr, usb::ResultInvalidParameter());

        const u64 cluster_size = static_cast<u64>(m_bytes_per_sector) * m_sectors_per_cluster;
        R_UNLESS(cluster_size > 0 && m_cluster_count <= UINT64_MAX / cluster_size, usb::ResultCorruptedFilesystem());

        const u64 bitmap_bytes = (static_cast<u64>(m_cluster_count) + 7) / 8;
        R_UNLESS(m_allocation_bitmap_cluster != 0 && m_allocation_bitmap_size >= bitmap_bytes, usb::ResultCorruptedFilesystem());

        u64 remaining = bitmap_bytes;
        u64 processed = 0;
        u64 free_clusters = 0;
        u32 cluster = m_allocation_bitmap_cluster;
        u32 cluster_hops = 0;

        while (remaining > 0) {
            u64 cluster_lba = 0;
            R_TRY(this->GetClusterLba(cluster, std::addressof(cluster_lba)));
            for (u32 sector = 0; sector < m_sectors_per_cluster && remaining > 0; ++sector) {
                R_TRY(this->ReadRelativeSectors(cluster_lba - m_partition.first_lba + sector, 1, m_sector_buffer, m_bytes_per_sector));

                const u32 bytes = static_cast<u32>(std::min<u64>(remaining, m_bytes_per_sector));
                for (u32 i = 0; i < bytes; ++i) {
                    const u8 value = m_sector_buffer[i];
                    const u64 first_bit = (processed + i) * 8;
                    const u64 valid_bits = std::min<u64>(8, static_cast<u64>(m_cluster_count) - std::min<u64>(m_cluster_count, first_bit));
                    for (u64 bit = 0; bit < valid_bits; ++bit) {
                        if ((value & (1u << bit)) == 0) {
                            ++free_clusters;
                        }
                    }
                }

                processed += bytes;
                remaining -= bytes;
            }

            if (remaining == 0) {
                break;
            }

            if (m_allocation_bitmap_no_fat_chain) {
                R_UNLESS(cluster < m_cluster_count + 1, usb::ResultCorruptedFilesystem());
                ++cluster;
            } else {
                R_UNLESS(cluster_hops++ < m_cluster_count, usb::ResultCorruptedFilesystem());
                u32 next_cluster = 0;
                R_TRY(this->ReadFatEntry(cluster, std::addressof(next_cluster)));
                R_UNLESS(next_cluster < ExFatEndOfChain, usb::ResultCorruptedFilesystem());
                cluster = next_cluster;
            }
        }

        R_UNLESS(free_clusters <= UINT64_MAX / cluster_size, usb::ResultCorruptedFilesystem());
        *out_total_size = static_cast<u64>(m_cluster_count) * cluster_size;
        *out_free_size = free_clusters * cluster_size;
        R_SUCCEED();
    }

}

#endif
