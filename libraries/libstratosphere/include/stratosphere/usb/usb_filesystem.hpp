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
#pragma once

#include <vapours.hpp>
#include <stratosphere/os/os_sdk_mutex.hpp>
#include <stratosphere/usb/usb_storage.hpp>

namespace ams::fs::fsa {
    class IFileSystem;
}

namespace ams::usb::filesystem {

    /* Opt-in internal read-only mount name. */
    constexpr inline const char *MountName = "usbfs";

    struct Status {
        bool mounted;
        bool ready;
        u16 reserved;
        u32 last_result;
    };

    /*
     * Provides the request boundary used by future USB-backed consumers.
     * Mount only binds to an already-running storage worker; it never starts
     * USB access itself and it exposes read-only operations only.
     */
    class Service {
        NON_COPYABLE(Service);
        NON_MOVEABLE(Service);

        private:
            mutable os::SdkMutex m_mutex{};
            storage::Service *m_storage{nullptr};
            bool m_mounted{false};

        public:
            Service() = default;
            ~Service();

        public:
            Result Mount(storage::Service *storage);
            Result Unmount();
            Result GetStatus(Status *out) const;
            Result GetMediaGeneration(u64 *out) const;
            Result GetFileInfo(const char *path, volume::FileInfo *out);
            Result ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read);
            Result ReadDirectory(const char *path, volume::DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count);
            Result GetFileInfoAtGeneration(const char *path, volume::FileInfo *out, u64 media_generation);
            Result InvalidateReadCacheAtGeneration(u64 media_generation);
            Result ReadFileAtGeneration(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read, u64 media_generation);
            Result ReadDirectoryAtGeneration(const char *path, volume::DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count, u64 media_generation);
            Result GetSpaceSize(u64 *out_free_size, u64 *out_total_size);
            bool IsMounted() const;
            Result AcquireEjectLease(storage::EjectLease *lease, u64 generation);
    };

    Result CreateReadOnlyFileSystem(std::unique_ptr<fs::fsa::IFileSystem> *out, Service *service);

}
