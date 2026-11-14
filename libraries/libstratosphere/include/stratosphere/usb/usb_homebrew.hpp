/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <vapours.hpp>
#include <stratosphere/ro/ro_types.hpp>
#include <stratosphere/usb/usb_filesystem.hpp>

namespace ams::usb::homebrew {

    struct NroInfo {
        u32 version;
        u32 flags;
        u32 text_offset;
        u32 text_size;
        u32 ro_offset;
        u32 ro_size;
        u32 rw_offset;
        u32 rw_size;
        u32 bss_size;
        u32 reserved;
        u64 file_size;
        u64 image_size;
        u64 media_generation;
        char path[fs::EntryNameLengthMax + 1];
    };

    static_assert(sizeof(NroInfo) == 0x348);

    /*
     * Read-only validator and random reader for homebrew NRO files.
     * This deliberately does not create a process, modify an NRO, or route
     * anything through Nintendo's title/content services.
     */
    class NroReader {
        NON_COPYABLE(NroReader);
        NON_MOVEABLE(NroReader);

        private:
            filesystem::Service *m_filesystem{nullptr};
            NroInfo m_info{};
            storage::EjectLease m_eject_lease;

        public:
            NroReader() = default;
            ~NroReader() = default;

        public:
            Result Open(filesystem::Service *filesystem, const char *path);
            void Close();

            bool IsOpen() const { return m_filesystem != nullptr; }
            Result GetInfo(NroInfo *out) const;
            Result ReadAt(u64 offset, void *buffer, size_t size, size_t *out_read);

        private:
            Result CheckMediaGeneration() const;
            Result ReadBytes(u64 offset, void *buffer, size_t size);
            Result ValidateHeader(const ro::NroHeader &header, u64 file_size);
    };

}
