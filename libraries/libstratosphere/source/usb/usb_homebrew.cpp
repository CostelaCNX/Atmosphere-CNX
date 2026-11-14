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
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_homebrew.hpp>

namespace ams::usb::homebrew {

    namespace {

        bool HasNroExtension(const char *path) {
            const size_t path_length = std::strlen(path);
            constexpr const char Extension[] = ".nro";
            constexpr size_t ExtensionLength = sizeof(Extension) - 1;
            if (path_length < ExtensionLength) {
                return false;
            }

            const char *suffix = path + path_length - ExtensionLength;
            for (size_t i = 0; i < ExtensionLength; ++i) {
                char c = suffix[i];
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
                if (c != Extension[i]) {
                    return false;
                }
            }
            return true;
        }

        Result CheckRegion(u32 offset, u32 size, u64 image_size) {
            R_UNLESS(static_cast<u64>(offset) <= image_size, usb::ResultHomebrewInvalid());
            R_UNLESS(static_cast<u64>(size) <= image_size - offset, usb::ResultHomebrewInvalid());
            R_SUCCEED();
        }

    }

    void NroReader::Close() {
        m_eject_lease.Reset();
        m_filesystem = nullptr;
        std::memset(std::addressof(m_info), 0, sizeof(m_info));
    }

    Result NroReader::Open(filesystem::Service *filesystem, const char *path) {
        R_UNLESS(filesystem != nullptr && path != nullptr && path[0] != '\0', usb::ResultInvalidParameter());
        R_UNLESS(filesystem->IsMounted(), usb::ResultNotInitialized());
        R_UNLESS(HasNroExtension(path), usb::ResultHomebrewUnsupported());

        const size_t path_length = std::strlen(path);
        R_UNLESS(path_length <= fs::EntryNameLengthMax, usb::ResultInvalidParameter());

        this->Close();

        u64 media_generation = 0;
        R_TRY(filesystem->GetMediaGeneration(std::addressof(media_generation)));
        R_TRY(filesystem->AcquireEjectLease(&m_eject_lease,media_generation));
        ON_SCOPE_EXIT { if(!this->IsOpen()) m_eject_lease.Reset(); };

        volume::FileInfo file_info{};
        R_TRY(filesystem->GetFileInfoAtGeneration(path, std::addressof(file_info), media_generation));
        R_UNLESS(!file_info.directory && file_info.size >= sizeof(ro::NroHeader), usb::ResultHomebrewInvalid());

        m_filesystem = filesystem;
        m_info.file_size = file_info.size;
        m_info.media_generation = media_generation;
        std::memcpy(m_info.path, path, path_length);
        m_info.path[path_length] = '\0';

        ro::NroHeader header{};
        Result result = this->ReadBytes(0, std::addressof(header), sizeof(header));
        if (R_SUCCEEDED(result)) {
            result = this->ValidateHeader(header, file_info.size);
        }
        if (R_FAILED(result)) {
            this->Close();
            R_THROW(result);
        }

        R_SUCCEED();
    }

    Result NroReader::GetInfo(NroInfo *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        R_TRY(this->CheckMediaGeneration());
        *out = m_info;
        R_SUCCEED();
    }

    Result NroReader::ReadAt(u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        R_UNLESS(out_read != nullptr && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
        R_TRY(this->ReadBytes(offset, buffer, size));
        *out_read = size;
        R_SUCCEED();
    }

    Result NroReader::ReadBytes(u64 offset, void *buffer, size_t size) {
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        R_UNLESS(size == 0 || buffer != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(offset <= m_info.file_size && size <= m_info.file_size - offset, usb::ResultHomebrewInvalid());
        R_TRY(this->CheckMediaGeneration());

        size_t out_read = 0;
        R_TRY(m_filesystem->ReadFileAtGeneration(m_info.path, offset, buffer, size, std::addressof(out_read), m_info.media_generation));
        R_UNLESS(out_read == size, usb::ResultTransactionError());
        R_SUCCEED();
    }

    Result NroReader::CheckMediaGeneration() const {
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        u64 current_media_generation = 0;
        R_TRY(m_filesystem->GetMediaGeneration(std::addressof(current_media_generation)));
        R_UNLESS(current_media_generation == m_info.media_generation, usb::ResultMediaChanged());
        R_SUCCEED();
    }

    Result NroReader::ValidateHeader(const ro::NroHeader &header, u64 file_size) {
        R_UNLESS(header.IsMagicValid(), usb::ResultHomebrewInvalid());

        const u64 image_size = header.GetSize();
        R_UNLESS(image_size >= sizeof(ro::NroHeader) && image_size <= file_size, usb::ResultHomebrewInvalid());
        R_TRY(CheckRegion(header.GetTextOffset(), header.GetTextSize(), image_size));
        R_TRY(CheckRegion(header.GetRoOffset(), header.GetRoSize(), image_size));
        R_TRY(CheckRegion(header.GetRwOffset(), header.GetRwSize(), image_size));

        m_info.version = header.GetVersion();
        m_info.flags = header.GetFlags();
        m_info.text_offset = header.GetTextOffset();
        m_info.text_size = header.GetTextSize();
        m_info.ro_offset = header.GetRoOffset();
        m_info.ro_size = header.GetRoSize();
        m_info.rw_offset = header.GetRwOffset();
        m_info.rw_size = header.GetRwSize();
        m_info.bss_size = header.GetBssSize();
        m_info.image_size = image_size;
        R_SUCCEED();
    }

}
