/*
 * Copyright (c) Atmosphère-NX
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once
#include <stratosphere/fs/fs_file.hpp>

namespace ams::kvdb {

    namespace impl {
        /* The same 12-byte wire headers used by ArchiveReader/ArchiveWriter. */
        struct FileArchiveHeader {
            u8 magic[4];
            u32 first;
            u32 second;
        };
        static_assert(sizeof(FileArchiveHeader) == 0xC);
    }

    /* These adapters borrow their file handles. They use no dynamic allocation
     * and transfer values directly between the file and resident database. */
    class FileArchiveReader {
        private:
            fs::FileHandle m_file;
            s64 m_size;
            s64 m_offset;
        public:
            FileArchiveReader(fs::FileHandle file, s64 size) : m_file(file), m_size(size), m_offset(0) { /* ... */ }

            bool CanRead(size_t first, size_t second = 0) const {
                if (m_offset < 0 || m_offset > m_size) {
                    return false;
                }
                const u64 remaining = static_cast<u64>(m_size - m_offset);
                return first <= remaining && second <= remaining - first;
            }

            bool AtEnd() const { return m_offset == m_size; }

            Result Read(void *out, size_t size) {
                R_UNLESS(this->CanRead(size), kvdb::ResultInvalidKeyValue());
                auto *dst = static_cast<u8 *>(out);
                while (size != 0) {
                    const size_t chunk = std::min(size, size_t{64 * 1024});
                    size_t actual = 0;
                    R_TRY(fs::ReadFile(std::addressof(actual), m_file, m_offset, dst, chunk));
                    R_UNLESS(actual == chunk, kvdb::ResultInvalidKeyValue());
                    m_offset += chunk;
                    dst += chunk;
                    size -= chunk;
                }
                R_SUCCEED();
            }

            Result ReadEntryCount(size_t *out) {
                R_UNLESS(m_offset == 0, kvdb::ResultInvalidKeyValue());
                impl::FileArchiveHeader header;
                R_TRY(this->Read(std::addressof(header), sizeof(header)));
                R_UNLESS(std::memcmp(header.magic, "IMKV", sizeof(header.magic)) == 0, kvdb::ResultInvalidKeyValue());
                *out = header.second;
                R_SUCCEED();
            }

            Result ReadEntrySize(size_t *out_key_size, size_t *out_value_size) {
                R_UNLESS(m_offset >= static_cast<s64>(sizeof(impl::FileArchiveHeader)), kvdb::ResultInvalidKeyValue());
                impl::FileArchiveHeader header;
                R_TRY(this->Read(std::addressof(header), sizeof(header)));
                R_UNLESS(std::memcmp(header.magic, "IMEN", sizeof(header.magic)) == 0, kvdb::ResultInvalidKeyValue());
                *out_key_size = header.first;
                *out_value_size = header.second;
                R_SUCCEED();
            }
    };

    class FileArchiveWriter {
        private:
            fs::FileHandle m_file;
            s64 m_offset;

            Result Write(const void *value, size_t size) {
                R_UNLESS(size <= static_cast<u64>(INT64_MAX - m_offset), kvdb::ResultInvalidKeyValue());
                const auto *src = static_cast<const u8 *>(value);
                while (size != 0) {
                    const size_t chunk = std::min(size, size_t{64 * 1024});
                    R_TRY(fs::WriteFile(m_file, m_offset, src, chunk, fs::WriteOption::None));
                    m_offset += chunk;
                    src += chunk;
                    size -= chunk;
                }
                R_SUCCEED();
            }
        public:
            static constexpr size_t HeaderSize = sizeof(impl::FileArchiveHeader);
            explicit FileArchiveWriter(fs::FileHandle file) : m_file(file), m_offset(0) { /* ... */ }

            Result WriteHeader(size_t entry_count) {
                R_UNLESS(m_offset == 0 && entry_count <= UINT32_MAX, kvdb::ResultInvalidKeyValue());
                impl::FileArchiveHeader header{};
                std::memcpy(header.magic, "IMKV", sizeof(header.magic));
                header.second = static_cast<u32>(entry_count);
                R_RETURN(this->Write(std::addressof(header), sizeof(header)));
            }

            Result WriteEntry(const void *key, size_t key_size, const void *value, size_t value_size) {
                R_UNLESS(m_offset >= static_cast<s64>(HeaderSize) && key_size <= UINT32_MAX && value_size <= UINT32_MAX, kvdb::ResultInvalidKeyValue());
                impl::FileArchiveHeader header{};
                std::memcpy(header.magic, "IMEN", sizeof(header.magic));
                header.first = static_cast<u32>(key_size);
                header.second = static_cast<u32>(value_size);
                R_TRY(this->Write(std::addressof(header), sizeof(header)));
                R_TRY(this->Write(key, key_size));
                R_RETURN(this->Write(value, value_size));
            }
    };
}
