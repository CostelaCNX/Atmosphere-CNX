/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <vapours.hpp>
#include <stratosphere/usb/usb_filesystem.hpp>

namespace ams::usb::game_image {

    constexpr inline size_t MaxEntryCount = 128;
    constexpr inline size_t MaxEntryNameLength = 0x100;
    constexpr inline size_t MaxStringTableSize = 64_KB;
    constexpr inline u64 XciRootFilesystemOffset = 0xF000;
    constexpr inline u64 XciScanLimit = 16_MB;
    constexpr inline u64 XciScanStep = 0x200;

    enum class Format : u8 {
        Unknown,
        Pfs0,
        Hfs0,
        Xci,
    };

    struct EntryInfo {
        u64 offset;
        u64 size;
        u32 hash_size;
        char name[MaxEntryNameLength + 1];
    };

    struct Info {
        Format format;
        u8 reserved[3];
        u32 entry_count;
        u64 image_size;
        u64 filesystem_offset;
        u64 data_offset;
        u64 string_table_size;
        u64 media_generation;
        char path[fs::EntryNameLengthMax + 1];
    };

    /*
     * Read-only metadata and random-access reader for independently parsed
     * PFS0/HFS0 containers. This class deliberately stops at container
     * structure: it does not decrypt content, register a game-card device,
     * or integrate with Horizon title loading.
     */
    class Reader {
        NON_COPYABLE(Reader);
        NON_MOVEABLE(Reader);

        private:
            filesystem::Service *m_filesystem{nullptr};
            Info m_info{};
            EntryInfo m_entries[MaxEntryCount]{};

        public:
            Reader() = default;
            ~Reader() = default;

        public:
            Result Open(filesystem::Service *filesystem, const char *path);
            void Close();

            bool IsOpen() const { return m_filesystem != nullptr; }
            Result GetInfo(Info *out) const;
            Result GetEntryInfo(size_t index, EntryInfo *out) const;
            Result ReadAt(u64 offset, void *buffer, size_t size, size_t *out_read);

        private:
            Result CheckMediaGeneration() const;
            Result ReadBytes(u64 offset, void *buffer, size_t size);
            Result ParseFilesystem(Format format, u64 filesystem_offset);
            Result ParseXci();
            Result LocateXciRootFilesystem(u64 *out_offset);
    };

}
