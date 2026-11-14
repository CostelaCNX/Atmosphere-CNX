/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 or later.
 */
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_io_diagnostics.hpp>
#include <stratosphere/usb/usb_sector_cache.hpp>
#include <stratosphere/usb/usb_write_cache_diagnostics.hpp>
#include <stratosphere/usb/usb_write_transfer.hpp>
#include <stratosphere/usb/usb_completion_probe.hpp>
#include <stratosphere/usb/usb_host_write.hpp>
#include "usb_fatfs.hpp"

#include "ff.h"
#include "diskio.h"

static_assert(FF_LBA64 == 1 && sizeof(LBA_t) == sizeof(u64), "USB requires end-to-end 64-bit sector addressing");

namespace {

    /* FatFs often asks for large contiguous runs while writing NCA files.
     * A 4 KiB bounce buffer made every request a separate BOT transaction and
     * was the main software-side speed limiter. Keep the buffer bounded and
     * DMA-aligned while allowing the USB layer to use larger transfers. */
    constexpr size_t ScratchSize = 0x40000;

    constexpr bool IsValidSectorSize(u32 size) {
        return size >= 0x200 && size <= 0x1000 && size != 0 && (size & (size - 1)) == 0;
    }

    struct Context {
        ams::usb::mass_storage::Device *device{nullptr};
        u64 first_lba{0};
        u64 sector_count{0};
        u32 sector_size{0};
        alignas(0x1000) u8 scratch[ScratchSize]{};
    };

    Context g_context;
    ams::usb::CleanSectorCache g_clean_sectors;

    void ClearCleanSectors() {
        ams::usb::fatfs::DiscardCleanSectors();
        ams::usb::write_transfer::Invalidate();
        ams::usb::completion_probe::Invalidate();
        ams::usb::host_write::Invalidate();
    }

    void PublishCacheState() {
        ams::usb::write_cache_diagnostics::State(g_context.sector_size,g_clean_sectors.Count());
    }

    bool IsValidDrive(BYTE pdrv) {
        return pdrv == 0 && g_context.device != nullptr && g_context.device->IsInitialized();
    }

    bool IsValidRange(LBA_t sector, UINT count) {
        return count > 0 && static_cast<u64>(sector) < g_context.sector_count &&
               static_cast<u64>(count) <= g_context.sector_count - static_cast<u64>(sector);
    }

}

namespace ams::usb::fatfs {

    void DiscardCleanSectors() {
        g_clean_sectors.Clear();
        write_cache_diagnostics::Clear(g_context.sector_size);
    }

    Result Attach(mass_storage::Device *device, const volume::Partition &partition) {
        R_UNLESS(g_context.device == nullptr, usb::ResultAlreadyInitialized());
        R_UNLESS(device != nullptr && device->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(IsValidSectorSize(device->GetGeometry().block_size), usb::ResultUnsupportedFilesystem());
        R_UNLESS(partition.sector_count > 0, usb::ResultUnsupportedFilesystem());
        R_UNLESS(partition.first_lba < device->GetGeometry().block_count && partition.sector_count <= device->GetGeometry().block_count - partition.first_lba, usb::ResultInvalidParameter());

        g_context.device = device;
        g_context.first_lba = partition.first_lba;
        g_context.sector_count = partition.sector_count;
        g_context.sector_size = device->GetGeometry().block_size;
        g_clean_sectors.Configure(g_context.sector_size);
        write_cache_diagnostics::Clear(g_context.sector_size);
        write_transfer::Invalidate();
        completion_probe::Invalidate();
        host_write::Invalidate();
        std::memset(g_context.scratch, 0, sizeof(g_context.scratch));
        R_SUCCEED();
    }

    void Detach() {
        g_context.device = nullptr;
        g_context.first_lba = 0;
        g_context.sector_count = 0;
        g_context.sector_size = 0;
        g_clean_sectors.Configure(0);
        write_cache_diagnostics::Clear(0);
        write_transfer::Invalidate();
        completion_probe::Invalidate();
        host_write::Invalidate();
    }

}

extern "C" {

    DSTATUS disk_initialize(BYTE pdrv) {
        return IsValidDrive(pdrv) ? 0 : STA_NOINIT;
    }

    DSTATUS disk_status(BYTE pdrv) {
        return IsValidDrive(pdrv) ? 0 : STA_NOINIT;
    }

    DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
        if (!IsValidDrive(pdrv) || buff == nullptr || !IsValidRange(sector, count)) {
            ClearCleanSectors();
            return RES_PARERR;
        }

        const u32 sectors_per_transfer = static_cast<u32>(ScratchSize / g_context.sector_size);
        while (count > 0) {
            const u32 transfer_count = std::min<u32>(count, sectors_per_transfer);
            const u64 lba = g_context.first_lba + static_cast<u64>(sector);
            const size_t transfer_size = static_cast<size_t>(transfer_count) * g_context.sector_size;
            const auto result = [&] {
                const ams::usb::io_diagnostics::ScopedTimer timer(ams::usb::io_diagnostics::disk_read_us);
                ams::usb::io_diagnostics::disk_read_calls.fetch_add(1, std::memory_order_relaxed);
                return g_context.device->ReadBlocks(lba, transfer_count, g_context.scratch, transfer_size);
            }();
            if (R_FAILED(result)) {
                ClearCleanSectors();
                return RES_ERROR;
            }
            ams::usb::io_diagnostics::disk_read_bytes.fetch_add(transfer_size, std::memory_order_relaxed);
            // Seed retains only the bounded tail of a large read. Invalidate
            // its whole observed range first so an older prefix can never
            // contradict bytes just returned by the device.
            g_clean_sectors.Invalidate(lba,transfer_count);
            g_clean_sectors.Seed(lba,transfer_count,g_context.scratch);
            PublishCacheState();
            std::memcpy(buff, g_context.scratch, transfer_size);
            buff += transfer_size;
            sector += transfer_count;
            count -= transfer_count;
        }
        return RES_OK;
    }

    DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
        if (!IsValidDrive(pdrv) || buff == nullptr || !IsValidRange(sector, count)) {
            ClearCleanSectors();
            return RES_PARERR;
        }

        ams::usb::write_cache_diagnostics::LogicalWrite(count);
        // Sample exactly once. Expiry changes the next request, never the
        // ordering or boundaries of this already-running disk_write.
        const u32 transfer_bytes = ams::usb::write_transfer::SelectedBytes();
        // Only an exact, complete single-sector request can be elided. Large
        // requests retain their original contiguous transfers and ordering.
        if (count == 1 && g_clean_sectors.Matches(g_context.first_lba+static_cast<u64>(sector),buff)) {
            ams::usb::write_cache_diagnostics::Elided(g_context.sector_size);
            return RES_OK;
        }

        const u32 sectors_per_transfer = transfer_bytes / g_context.sector_size;
        while (count > 0) {
            const u32 transfer_count = std::min<u32>(count, sectors_per_transfer);
            const u64 lba = g_context.first_lba + static_cast<u64>(sector);
            const size_t transfer_size = static_cast<size_t>(transfer_count) * g_context.sector_size;
            // Invalidate before USB: a failed transfer may have written some
            // bytes, so stale pre-write data must never authorize a later hit.
            g_clean_sectors.Invalidate(lba,transfer_count);
            PublishCacheState();
            std::memcpy(g_context.scratch, buff, transfer_size);
            const auto result = [&] {
                const ams::usb::io_diagnostics::ScopedTimer timer(ams::usb::io_diagnostics::disk_write_us);
                ams::usb::io_diagnostics::disk_write_calls.fetch_add(1, std::memory_order_relaxed);
                return g_context.device->WriteBlocks(lba, transfer_count, g_context.scratch, transfer_size);
            }();
            if (R_FAILED(result)) {
                ClearCleanSectors();
                return RES_ERROR;
            }
            ams::usb::io_diagnostics::disk_write_bytes.fetch_add(transfer_size, std::memory_order_relaxed);
            g_clean_sectors.Seed(lba,transfer_count,g_context.scratch);
            PublishCacheState();
            buff += transfer_size;
            sector += transfer_count;
            count -= transfer_count;
        }
        return RES_OK;
    }

    DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
        if (!IsValidDrive(pdrv)) {
            return RES_NOTRDY;
        }

        switch (cmd) {
            case CTRL_SYNC:
                return RES_OK;
            case GET_SECTOR_COUNT:
                if (buff == nullptr) {
                    return RES_PARERR;
                }
                *static_cast<LBA_t *>(buff) = g_context.sector_count;
                return RES_OK;
            case GET_SECTOR_SIZE:
                if (buff == nullptr) {
                    return RES_PARERR;
                }
                *static_cast<WORD *>(buff) = static_cast<WORD>(g_context.sector_size);
                return RES_OK;
            case GET_BLOCK_SIZE:
                if (buff == nullptr) {
                    return RES_PARERR;
                }
                *static_cast<DWORD *>(buff) = 1;
                return RES_OK;
            default:
                return RES_PARERR;
        }
    }

}
