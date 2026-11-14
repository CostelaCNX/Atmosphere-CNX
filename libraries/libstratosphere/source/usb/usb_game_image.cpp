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
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_game_image.hpp>

namespace ams::usb::game_image {

    namespace {

        constexpr size_t Pfs0HeaderSize = 0x10;
        constexpr size_t Pfs0EntrySize = 0x18;
        constexpr size_t Hfs0EntrySize = 0x40;
        constexpr size_t XciScanBufferSize = 16_KB;

        constexpr u16 LoadLittleEndian16(const u8 *src) {
            return static_cast<u16>(src[0]) |
                   static_cast<u16>(src[1] << 8);
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

        constexpr bool IsPfs0Magic(const u8 *src) {
            return std::memcmp(src, "PFS0", sizeof("PFS0") - 1) == 0;
        }

        constexpr bool IsHfs0Magic(const u8 *src) {
            return std::memcmp(src, "HFS0", sizeof("HFS0") - 1) == 0;
        }

        constexpr bool IsHeadMagic(const u8 *src) {
            return std::memcmp(src, "HEAD", sizeof("HEAD") - 1) == 0;
        }

        bool HasExtension(const char *path, const char *extension) {
            const size_t path_length = std::strlen(path);
            const size_t extension_length = std::strlen(extension);
            return path_length >= extension_length &&
                   std::memcmp(path + path_length - extension_length, extension, extension_length) == 0;
        }

    }

    void Reader::Close() {
        m_filesystem = nullptr;
        std::memset(std::addressof(m_info), 0, sizeof(m_info));
        std::memset(m_entries, 0, sizeof(m_entries));
    }

    Result Reader::Open(filesystem::Service *filesystem, const char *path) {
        R_UNLESS(filesystem != nullptr && path != nullptr && path[0] != '\0', usb::ResultInvalidParameter());
        R_UNLESS(filesystem->IsMounted(), usb::ResultNotInitialized());

        const size_t path_length = std::strlen(path);
        R_UNLESS(path_length <= fs::EntryNameLengthMax, usb::ResultInvalidParameter());

        this->Close();

        volume::FileInfo file_info{};
        R_TRY(filesystem->GetFileInfo(path, std::addressof(file_info)));
        R_UNLESS(!file_info.directory && file_info.size >= sizeof(u32), usb::ResultGameImageInvalid());

        u64 media_generation = 0;
        R_TRY(filesystem->GetMediaGeneration(std::addressof(media_generation)));

        m_filesystem = filesystem;
        m_info.image_size = file_info.size;
        m_info.media_generation = media_generation;
        std::memcpy(m_info.path, path, path_length);
        m_info.path[path_length] = '\0';

        u8 magic[sizeof(u32)]{};
        Result result = this->ReadBytes(0, magic, sizeof(magic));
        if (R_FAILED(result)) {
            this->Close();
            R_THROW(result);
        }

        if (IsPfs0Magic(magic)) {
            result = this->ParseFilesystem(Format::Pfs0, 0);
        } else if (IsHfs0Magic(magic)) {
            result = this->ParseFilesystem(Format::Hfs0, 0);
        } else if (IsHeadMagic(magic)) {
            result = this->ParseXci();
        } else if (HasExtension(path, ".nsz") || HasExtension(path, ".NSZ")) {
            result = usb::ResultGameImageUnsupported();
        } else {
            result = usb::ResultGameImageUnsupported();
        }

        if (R_FAILED(result)) {
            this->Close();
            R_THROW(result);
        }

        R_SUCCEED();
    }

    Result Reader::GetInfo(Info *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        R_TRY(this->CheckMediaGeneration());
        *out = m_info;
        R_SUCCEED();
    }

    Result Reader::GetEntryInfo(size_t index, EntryInfo *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        R_TRY(this->CheckMediaGeneration());
        R_UNLESS(index < m_info.entry_count, usb::ResultInvalidParameter());
        *out = m_entries[index];
        R_SUCCEED();
    }

    Result Reader::ReadAt(u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        R_UNLESS(out_read != nullptr && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
        R_TRY(this->ReadBytes(offset, buffer, size));
        *out_read = size;
        R_SUCCEED();
    }

    Result Reader::ReadBytes(u64 offset, void *buffer, size_t size) {
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        R_UNLESS(size == 0 || buffer != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(offset <= m_info.image_size && size <= m_info.image_size - offset, usb::ResultGameImageInvalid());

        R_TRY(this->CheckMediaGeneration());

        size_t out_read = 0;
        R_TRY(m_filesystem->ReadFile(m_info.path, offset, buffer, size, std::addressof(out_read)));
        R_UNLESS(out_read == size, usb::ResultTransactionError());
        R_SUCCEED();
    }

    Result Reader::CheckMediaGeneration() const {
        R_UNLESS(this->IsOpen(), usb::ResultNotInitialized());
        u64 current_media_generation = 0;
        R_TRY(m_filesystem->GetMediaGeneration(std::addressof(current_media_generation)));
        R_UNLESS(current_media_generation == m_info.media_generation, usb::ResultMediaChanged());
        R_SUCCEED();
    }

    Result Reader::ParseFilesystem(Format format, u64 filesystem_offset) {
        const size_t entry_size = format == Format::Pfs0 ? Pfs0EntrySize : Hfs0EntrySize;
        R_UNLESS(format == Format::Pfs0 || format == Format::Hfs0, usb::ResultInvalidParameter());
        R_UNLESS(filesystem_offset <= m_info.image_size && Pfs0HeaderSize <= m_info.image_size - filesystem_offset, usb::ResultGameImageInvalid());

        u8 header[Pfs0HeaderSize]{};
        R_TRY(this->ReadBytes(filesystem_offset, header, sizeof(header)));
        R_UNLESS((format == Format::Pfs0 && IsPfs0Magic(header)) || (format == Format::Hfs0 && IsHfs0Magic(header)), usb::ResultGameImageInvalid());

        const u32 entry_count = LoadLittleEndian32(header + 4);
        const u32 string_table_size = LoadLittleEndian32(header + 8);
        R_UNLESS(entry_count <= MaxEntryCount, usb::ResultGameImageTooManyEntries());
        R_UNLESS(string_table_size <= MaxStringTableSize, usb::ResultGameImageInvalid());

        const u64 entry_bytes = static_cast<u64>(entry_size) * entry_count;
        R_UNLESS(filesystem_offset <= m_info.image_size &&
                 Pfs0HeaderSize <= m_info.image_size - filesystem_offset &&
                 entry_bytes <= m_info.image_size - filesystem_offset - Pfs0HeaderSize &&
                 string_table_size <= m_info.image_size - filesystem_offset - Pfs0HeaderSize - entry_bytes,
                 usb::ResultGameImageInvalid());
        const u64 data_offset = filesystem_offset + Pfs0HeaderSize + entry_bytes + string_table_size;

        auto entries = std::make_unique<u8[]>(static_cast<size_t>(entry_bytes));
        auto strings = std::make_unique<u8[]>(static_cast<size_t>(string_table_size) + 1);
        R_UNLESS((entry_bytes == 0 || entries != nullptr) && strings != nullptr, usb::ResultMemAllocFailure());

        if (entry_bytes != 0) {
            R_TRY(this->ReadBytes(filesystem_offset + Pfs0HeaderSize, entries.get(), static_cast<size_t>(entry_bytes)));
        }
        if (string_table_size != 0) {
            R_TRY(this->ReadBytes(filesystem_offset + Pfs0HeaderSize + entry_bytes, strings.get(), string_table_size));
        }
        strings[string_table_size] = '\0';

        EntryInfo parsed_entries[MaxEntryCount]{};
        for (u32 i = 0; i < entry_count; ++i) {
            const u8 *entry = entries.get() + static_cast<size_t>(i) * entry_size;
            const u64 relative_offset = LoadLittleEndian64(entry + 0);
            const u64 size = LoadLittleEndian64(entry + 8);
            const u32 string_offset = LoadLittleEndian32(entry + 16);
            const u32 hash_size = format == Format::Hfs0 ? LoadLittleEndian32(entry + 20) : 0;

            R_UNLESS(relative_offset <= m_info.image_size - data_offset && size <= m_info.image_size - data_offset - relative_offset, usb::ResultGameImageInvalid());
            R_UNLESS(string_offset < string_table_size, usb::ResultGameImageInvalid());

            size_t name_length = 0;
            while (string_offset + name_length < string_table_size && strings[string_offset + name_length] != '\0') {
                ++name_length;
            }
            R_UNLESS(string_offset + name_length < string_table_size && name_length <= MaxEntryNameLength, usb::ResultGameImageInvalid());

            parsed_entries[i].offset = data_offset + relative_offset;
            parsed_entries[i].size = size;
            parsed_entries[i].hash_size = hash_size;
            std::memcpy(parsed_entries[i].name, strings.get() + string_offset, name_length);
            parsed_entries[i].name[name_length] = '\0';
        }

        m_info.format = format;
        m_info.entry_count = entry_count;
        m_info.filesystem_offset = filesystem_offset;
        m_info.data_offset = data_offset;
        m_info.string_table_size = string_table_size;
        std::memcpy(m_entries, parsed_entries, sizeof(parsed_entries));
        R_SUCCEED();
    }

    Result Reader::ParseXci() {
        Result result = this->ParseFilesystem(Format::Hfs0, XciRootFilesystemOffset);
        if (R_SUCCEEDED(result)) {
            m_info.format = Format::Xci;
            R_SUCCEED();
        }
        if (result.GetValue() != usb::ResultGameImageInvalid().GetValue() &&
            result.GetValue() != usb::ResultGameImageTooManyEntries().GetValue()) {
            R_THROW(result);
        }

        u64 root_offset = 0;
        R_TRY(this->LocateXciRootFilesystem(std::addressof(root_offset)));
        R_TRY(this->ParseFilesystem(Format::Hfs0, root_offset));
        m_info.format = Format::Xci;
        R_SUCCEED();
    }

    Result Reader::LocateXciRootFilesystem(u64 *out_offset) {
        R_UNLESS(out_offset != nullptr, usb::ResultInvalidParameter());
        const u64 scan_limit = std::min<u64>(m_info.image_size, XciScanLimit);
        alignas(0x1000) u8 buffer[XciScanBufferSize]{};

        for (u64 chunk_offset = 0; chunk_offset < scan_limit; chunk_offset += XciScanBufferSize) {
            const size_t chunk_size = static_cast<size_t>(std::min<u64>(XciScanBufferSize, scan_limit - chunk_offset));
            R_TRY(this->ReadBytes(chunk_offset, buffer, chunk_size));

            for (size_t i = 0; i + sizeof(u32) <= chunk_size; i += XciScanStep) {
                if (!IsHfs0Magic(buffer + i)) {
                    continue;
                }

                const u64 candidate = chunk_offset + i;
                const auto result = this->ParseFilesystem(Format::Hfs0, candidate);
                if (R_SUCCEEDED(result)) {
                    *out_offset = candidate;
                    R_SUCCEED();
                }
                if (result.GetValue() != usb::ResultGameImageInvalid().GetValue() &&
                    result.GetValue() != usb::ResultGameImageTooManyEntries().GetValue()) {
                    R_THROW(result);
                }
            }
        }

        R_THROW(usb::ResultGameImageInvalid());
    }

}
