/* Sector-zero classification shared with the host regression tests. */
#pragma once
#include <cstdint>
#include <cstring>

namespace ams::usb::volume::impl {

    inline bool IsSuperFloppyBootSector(const std::uint8_t *sector, std::uint32_t sector_size) {
        if (sector == nullptr || sector_size < 512 || sector_size > 4096 || (sector_size & (sector_size - 1)) != 0) {
            return false;
        }
        /* exFAT has a boot sector at LBA 0 on unpartitioned media. Its boot
         * code can overlap the MBR table area; do this check before MBR parsing. */
        if (std::memcmp(sector + 3, "EXFAT   ", 8) == 0) {
            return true;
        }
        if (sector[0] != 0xEB && sector[0] != 0xE9) {
            return false;
        }
        const auto le16 = [&](unsigned offset) -> std::uint16_t {
            return sector[offset] | (static_cast<std::uint16_t>(sector[offset + 1]) << 8);
        };
        const auto le32 = [&](unsigned offset) -> std::uint32_t {
            return le16(offset) | (static_cast<std::uint32_t>(le16(offset + 2)) << 16);
        };
        const auto cluster_sectors = sector[0x0D];
        if (le16(0x0B) != sector_size || cluster_sectors == 0 || (cluster_sectors & (cluster_sectors - 1)) != 0) {
            return false;
        }
        const auto root_entries = le16(0x11);
        const auto fat_size_16 = le16(0x16);
        const auto fat_size_32 = le32(0x24);
        return le16(510) == 0xAA55 && le16(0x0E) > 0 && sector[0x10] > 0
            && (fat_size_16 > 0 || fat_size_32 > 0) && (root_entries > 0 || fat_size_32 > 0);
    }

}
