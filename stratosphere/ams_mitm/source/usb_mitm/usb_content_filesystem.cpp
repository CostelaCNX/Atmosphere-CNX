/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>
#include "usb_content_route_stats.hpp"
#include <stratosphere/fs/impl/fs_newable.hpp>
#include "usbmitm_module.hpp"

namespace ams::mitm::usb_storage {

    namespace {

        constexpr const char RegisteredDirectory[] = "/registered";
        constexpr const char PlaceHolderDirectory[] = "/placehld";
        constexpr size_t DirectoryBatchSize = 8;

        bool IsRootPath(const fs::Path &path) {
            return path.IsEmpty() || path == "/";
        }

        bool IsPathInTree(const char *path, const char *root) {
            const size_t root_length = std::strlen(root);
            return std::strncmp(path, root, root_length) == 0
                && (path[root_length] == '\0' || path[root_length] == '/');
        }

        bool IsUsbContentDataPath(const fs::Path &path) {
            const char *value = path.GetString();
            return IsPathInTree(value, RegisteredDirectory)
                || IsPathInTree(value, PlaceHolderDirectory);
        }

        const char *GetUsbContentRootPath(const fs::Path &path) {
            const char *value = path.GetString();
            if (IsPathInTree(value, RegisteredDirectory)) {
                return RegisteredDirectory;
            }
            if (IsPathInTree(value, PlaceHolderDirectory)) {
                return PlaceHolderDirectory;
            }
            return nullptr;
        }

        Result ConvertUsbFileSystemResult(Result result) {
            if (R_SUCCEEDED(result)) {
                R_SUCCEED();
            }
            if (usb::ResultFileNotFound::Includes(result)) {
                R_THROW(fs::ResultPathNotFound());
            }
            if (usb::ResultAlreadyInitialized::Includes(result)) {
                R_THROW(fs::ResultPathAlreadyExists());
            }
            if (usb::ResultOperationDenied::Includes(result)
                || usb::ResultResourceBusy::Includes(result)) {
                R_THROW(fs::ResultTargetLocked());
            }
            if (usb::ResultInvalidParameter::Includes(result)) {
                R_THROW(fs::ResultInvalidArgument());
            }
            if (usb::ResultNotInitialized::Includes(result)
                || usb::ResultMediaChanged::Includes(result)
                || usb::ResultInterrupted::Includes(result)) {
                R_THROW(fs::ResultSdCardNotPresent());
            }
            R_RETURN(result);
        }

        Result CopyPath(char *out, size_t out_size, const fs::Path &path) {
            const char *value = IsRootPath(path) ? "/" : path.GetString();
            const size_t length = std::strlen(value);
            R_UNLESS(length < out_size, fs::ResultTooLongPath());
            std::memcpy(out, value, length + 1);
            R_SUCCEED();
        }

        Result MakeChildPath(char *out, size_t out_size, const char *parent, const char *name) {
            const bool root = parent[0] == '/' && parent[1] == '\0';
            const int written = root
                ? util::SNPrintf(out, out_size, "/%s", name)
                : util::SNPrintf(out, out_size, "%s/%s", parent, name);
            R_UNLESS(written > 0 && static_cast<size_t>(written) < out_size, fs::ResultTooLongPath());
            R_SUCCEED();
        }

        bool IsVisibleUsbRootEntry(const char *name, bool include_placeholders) {
            return std::strcmp(name, RegisteredDirectory + 1) == 0
                || (include_placeholders && std::strcmp(name, PlaceHolderDirectory + 1) == 0);
        }

        Result GetUsbEntry(usb::storage::Service *service, const char *path, u64 generation,
                           usb::volume::FileInfo *out, bool *out_found) {
            const Result result = service->GetFileInfoAtGeneration(path, out, generation);
            if (usb::ResultFileNotFound::Includes(result)) {
                *out_found = false;
                R_SUCCEED();
            }
            R_TRY(ConvertUsbFileSystemResult(result));
            *out_found = true;
            R_SUCCEED();
        }

        Result EnsureUsbContentRootDirectory(usb::storage::Service *service,
                                             const fs::Path &path, u64 generation) {
            const char *root = GetUsbContentRootPath(path);
            R_UNLESS(root != nullptr, fs::ResultInvalidArgument());

            auto ensure = [&](const char *directory) -> Result {
                usb::volume::FileInfo info{};
                bool found = false;
                R_TRY(GetUsbEntry(service, directory, generation, std::addressof(info), std::addressof(found)));
                if (!found) {
                    const Result result = service->CreateDirectoryAtGeneration(directory, generation);
                    if (!usb::ResultAlreadyInitialized::Includes(result)) {
                        R_TRY(ConvertUsbFileSystemResult(result));
                    }
                    R_TRY(GetUsbEntry(service, directory, generation, std::addressof(info), std::addressof(found)));
                }
                R_UNLESS(found && info.directory, fs::ResultPathNotFound());
                R_SUCCEED();
            };
            R_TRY(ensure(root));
            /* NCM's EnsureParentDirectory can see an existing SD bucket in
             * the merged view. That does not mean the USB bucket exists.
             * Materialize every USB parent before a create or registration
             * rename; never create/overwrite the leaf here. */
            char parent[fs::EntryNameLengthMax + 1]{};
            R_TRY(CopyPath(parent, sizeof(parent), path));
            for (size_t i = std::strlen(root) + 1; i < std::strlen(parent); ++i) {
                if (parent[i] == '/') {
                    parent[i] = '\0';
                    const Result result = ensure(parent);
                    parent[i] = '/';
                    R_TRY(result);
                }
            }
            R_SUCCEED();
        }

        Result DeleteUsbDirectoryContents(usb::storage::Service *service, const char *path, u64 generation) {
            usb::storage::EjectLease operation;
            R_TRY(operation.Acquire(service,generation));
            while (true) {
                usb::volume::DirectoryEntryInfo entry{};
                size_t count = 0;
                R_TRY(ConvertUsbFileSystemResult(service->ReadDirectoryAtGeneration(
                    path, std::addressof(entry), 1, 0, true, true,
                    std::addressof(count), generation)));
                if (count == 0) {
                    break;
                }

                char child[fs::EntryNameLengthMax + 1]{};
                R_TRY(MakeChildPath(child, sizeof(child), path, entry.name));
                if (entry.directory) {
                    R_TRY(DeleteUsbDirectoryContents(service, child, generation));
                }
                R_TRY(ConvertUsbFileSystemResult(service->DeleteFileAtGeneration(child, generation)));
            }
            R_SUCCEED();
        }

        class UsbContentFile final : public fs::fsa::IFile, public fs::impl::Newable {
            NON_COPYABLE(UsbContentFile);
            NON_MOVEABLE(UsbContentFile);

            private:
                usb::storage::EjectLease m_eject_lease;
                usb::storage::Service *m_service;
                fs::OpenMode m_mode;
                u64 m_media_generation;
                char m_path[fs::EntryNameLengthMax + 1]{};

            public:
                Result AcquireLease() { R_RETURN(m_eject_lease.Acquire(m_service,m_media_generation)); }
                UsbContentFile(usb::storage::Service *service, const fs::Path &path,
                               fs::OpenMode mode, u64 generation)
                    : m_service(service), m_mode(mode), m_media_generation(generation) {
                    AMS_ASSERT(m_service != nullptr);
                    R_ABORT_UNLESS(CopyPath(m_path, sizeof(m_path), path));
                }

            private:
                Result CheckGeneration() const {
                    u64 generation = 0;
                    R_UNLESS(IsUsbContentFileSystemRouteReady(std::addressof(generation), false),
                        fs::ResultSdCardNotPresent());
                    R_UNLESS(generation == m_media_generation, fs::ResultSdCardNotPresent());
                    R_SUCCEED();
                }

                virtual Result DoRead(size_t *out, s64 offset, void *buffer, size_t size,
                                      const fs::ReadOption &option) override final {
                    /* P1: IFile returns the available bytes at EOF. The USB
                     * volume is an exact-read interface: forwarding an EOF
                     * over-read reports a transport error and invalidates all
                     * handles on the drive. Use the same DryRead contract as
                     * other IFile implementations before entering that layer.
                     * DoGetSize and the actual read both pin the generation. */
                    *out = 0;
                    size_t request = 0;
                    R_TRY(this->DryRead(std::addressof(request), offset, size, option, m_mode));
                    R_SUCCEED_IF(request == 0);
                    size_t read = 0;
                    R_TRY(ConvertUsbFileSystemResult(m_service->ReadFileAtGeneration(
                        m_path, static_cast<u64>(offset), buffer, request, std::addressof(read), m_media_generation)));
                    /* A short result inside the known file is not EOF. Do not
                     * report success with incomplete NCA/metadata bytes. */
                    R_UNLESS(read == request, fs::ResultUnexpectedInLocalFileSystemA());
                    *out = read;
                    R_SUCCEED();
                }

                virtual Result DoGetSize(s64 *out) override final {
                    R_TRY(this->CheckGeneration());
                    usb::volume::FileInfo info{};
                    R_TRY(ConvertUsbFileSystemResult(m_service->GetFileInfoAtGeneration(
                        m_path, std::addressof(info), m_media_generation)));
                    R_UNLESS(!info.directory && info.size <= static_cast<u64>(std::numeric_limits<s64>::max()),
                        fs::ResultOutOfRange());
                    *out = static_cast<s64>(info.size);
                    R_SUCCEED();
                }

                virtual Result DoFlush() override final {
                    R_TRY(this->CheckGeneration());
                    R_RETURN(ConvertUsbFileSystemResult(m_service->FlushAtGeneration(m_media_generation)));
                }

                virtual Result DoWrite(s64 offset, const void *buffer, size_t size,
                                       const fs::WriteOption &option) override final {
                    bool append = false;
                    R_TRY(this->DryWrite(std::addressof(append), offset, size, option, m_mode));
                    R_TRY(this->CheckGeneration());
                    if (append) {
                        R_TRY(ConvertUsbFileSystemResult(m_service->ResizeFileAtGeneration(
                            m_path, static_cast<u64>(offset) + size, m_media_generation)));
                    }
                    size_t written = 0;
                    const Result result = ConvertUsbFileSystemResult(m_service->WriteFileAtGeneration(
                        m_path, static_cast<u64>(offset), buffer, size,
                        std::addressof(written), m_media_generation));
                    content_stats::ResultRecorded(result.GetValue());
                    R_TRY(result);
                    R_UNLESS(written == size, fs::ResultUnexpectedInLocalFileSystemA());
                    content_stats::writes.fetch_add(1, std::memory_order_relaxed);
                    content_stats::bytes.fetch_add(written, std::memory_order_relaxed);
                    content_stats::changes.fetch_add(1, std::memory_order_release);
                    if (option.HasFlushFlag()) {
                        R_TRY(this->DoFlush());
                    }
                    R_SUCCEED();
                }

                virtual Result DoSetSize(s64 size) override final {
                    R_TRY(this->DrySetSize(size, m_mode));
                    R_TRY(this->CheckGeneration());
                    R_RETURN(ConvertUsbFileSystemResult(m_service->ResizeFileAtGeneration(
                        m_path, static_cast<u64>(size), m_media_generation)));
                }

                virtual Result DoOperateRange(void *, size_t, fs::OperationId, s64, s64,
                                              const void *, size_t) override final {
                    R_THROW(fs::ResultNotImplemented());
                }

            public:
                virtual sf::cmif::DomainObjectId GetDomainObjectId() const override {
                    AMS_ABORT();
                }
        };

        class UsbContentOverlayDirectory final : public fs::fsa::IDirectory, public fs::impl::Newable {
            NON_COPYABLE(UsbContentOverlayDirectory);
            NON_MOVEABLE(UsbContentOverlayDirectory);

            private:
                usb::storage::EjectLease m_eject_lease;
                usb::storage::Service *m_service;
                std::shared_ptr<fs::fsa::IFileSystem> m_stock_file_system;
                std::unique_ptr<fs::fsa::IDirectory> m_stock_directory;
                fs::OpenDirectoryMode m_mode;
                u64 m_media_generation;
                size_t m_usb_offset{0};
                bool m_usb_complete{false};
                bool m_usb_directory_exists{false};
                bool m_stock_directory_exists{false};
                bool m_root{false};
                char m_path[fs::EntryNameLengthMax + 1]{};

            public:
                UsbContentOverlayDirectory(usb::storage::Service *service,
                                           std::shared_ptr<fs::fsa::IFileSystem> stock_file_system,
                                           fs::OpenDirectoryMode mode, u64 generation)
                    : m_service(service), m_stock_file_system(std::move(stock_file_system)),
                      m_mode(mode), m_media_generation(generation) {
                    AMS_ASSERT(m_service != nullptr);
                }

                Result Initialize(const fs::Path &path) {
                    R_TRY(m_eject_lease.Acquire(m_service,m_media_generation));
                    R_TRY(CopyPath(m_path, sizeof(m_path), path));
                    m_root = IsRootPath(path);

                    usb::volume::FileInfo info{};
                    bool found = false;
                    if (m_root) {
                        m_usb_directory_exists = true;
                    } else {
                        R_TRY(GetUsbEntry(m_service, m_path, m_media_generation,
                            std::addressof(info), std::addressof(found)));
                        m_usb_directory_exists = found && info.directory;
                    }

                    const Result stock_result = m_stock_file_system ? m_stock_file_system->OpenDirectory(
                        std::addressof(m_stock_directory), path, m_mode) : static_cast<Result>(fs::ResultPathNotFound());
                    if (R_SUCCEEDED(stock_result)) {
                        m_stock_directory_exists = true;
                    } else if (!fs::ResultPathNotFound::Includes(stock_result)) {
                        R_RETURN(stock_result);
                    }

                    R_UNLESS(m_usb_directory_exists || m_stock_directory_exists, fs::ResultPathNotFound());
                    R_SUCCEED();
                }

            private:
                Result IncludeUsbEntry(bool *out, const usb::volume::DirectoryEntryInfo &entry) const {
                    *out = !m_root || IsVisibleUsbRootEntry(entry.name, m_stock_file_system == nullptr);
                    R_SUCCEED_IF(!*out || m_stock_file_system == nullptr);
                    // A real SD entry wins, including directory/file type
                    // collisions. Do not hide SD errors behind a USB copy.
                    char child[fs::EntryNameLengthMax + 1]{};
                    R_TRY(MakeChildPath(child, sizeof(child), m_path, entry.name));
                    fs::Path path;
                    R_TRY(path.Initialize(child));
                    fs::DirectoryEntryType type{};
                    const Result result = m_stock_file_system->GetEntryType(std::addressof(type), path);
                    R_SUCCEED_IF(fs::ResultPathNotFound::Includes(result));
                    R_TRY(result);
                    *out = false;
                    R_SUCCEED();
                }

                Result ReadUsb(s64 *out_count, fs::DirectoryEntry *out_entries, s64 max_entries) {
                    *out_count = 0;
                    while (*out_count < max_entries && !m_usb_complete) {
                        usb::volume::DirectoryEntryInfo entries[DirectoryBatchSize]{};
                        const size_t requested = std::min<size_t>(
                            static_cast<size_t>(max_entries - *out_count), util::size(entries));
                        size_t count = 0;
                        R_TRY(ConvertUsbFileSystemResult(m_service->ReadDirectoryAtGeneration(
                            m_path, entries, requested, m_usb_offset,
                            (m_mode & fs::OpenDirectoryMode_Directory) != 0,
                            (m_mode & fs::OpenDirectoryMode_File) != 0,
                            std::addressof(count), m_media_generation)));
                        if (count == 0) {
                            m_usb_complete = true;
                            break;
                        }
                        m_usb_offset += count;

                        for (size_t i = 0; i < count && *out_count < max_entries; ++i) {
                            bool include = false;
                            R_TRY(this->IncludeUsbEntry(std::addressof(include), entries[i]));
                            if (!include) {
                                continue;
                            }
                            auto &out = out_entries[*out_count];
                            util::Strlcpy(out.name, entries[i].name, sizeof(out.name));
                            out.type = entries[i].directory
                                ? fs::DirectoryEntryType_Directory : fs::DirectoryEntryType_File;
                            R_UNLESS(entries[i].size <= static_cast<u64>(std::numeric_limits<s64>::max()),
                                fs::ResultOutOfRange());
                            out.file_size = entries[i].directory ? 0 : static_cast<s64>(entries[i].size);
                            ++(*out_count);
                        }
                    }
                    R_SUCCEED();
                }

                virtual Result DoRead(s64 *out_count, fs::DirectoryEntry *out_entries,
                                      s64 max_entries) override final {
                    R_UNLESS(max_entries > 0, fs::ResultInvalidArgument());
                    *out_count = 0;

                    if (!m_stock_file_system) {
                        usb::volume::FileInfo info{};
                        R_TRY(ConvertUsbFileSystemResult(m_service->GetFileInfoAtGeneration(
                            m_path, &info, m_media_generation)));
                    }
                    if (m_usb_directory_exists && !m_usb_complete) {
                        s64 usb_count = 0;
                        R_TRY(this->ReadUsb(std::addressof(usb_count), out_entries, max_entries));
                        *out_count += usb_count;
                    }

                    while (*out_count < max_entries && m_stock_directory_exists) {
                        fs::DirectoryEntry stock_entry{};
                        s64 stock_count = 0;
                        R_TRY(m_stock_directory->Read(std::addressof(stock_count),
                            std::addressof(stock_entry), 1));
                        if (stock_count == 0) {
                            break;
                        }

                        out_entries[*out_count] = stock_entry;
                        ++(*out_count);
                    }
                    R_SUCCEED();
                }

                virtual Result DoGetEntryCount(s64 *out) override final {
                    s64 total = 0;
                    if (m_usb_directory_exists) {
                        size_t skip = 0;
                        while (true) {
                            usb::volume::DirectoryEntryInfo entries[DirectoryBatchSize]{};
                            size_t count = 0;
                            R_TRY(ConvertUsbFileSystemResult(m_service->ReadDirectoryAtGeneration(
                                m_path, entries, util::size(entries), skip,
                                (m_mode & fs::OpenDirectoryMode_Directory) != 0,
                                (m_mode & fs::OpenDirectoryMode_File) != 0,
                                std::addressof(count), m_media_generation)));
                            if (count == 0) {
                                break;
                            }
                            skip += count;
                            for (size_t i = 0; i < count; ++i) {
                                bool include = false;
                                R_TRY(this->IncludeUsbEntry(std::addressof(include), entries[i]));
                                if (include) {
                                    ++total;
                                }
                            }
                        }
                    }

                    if (m_stock_directory_exists) {
                        fs::Path path;
                        R_TRY(path.Initialize(m_path));
                        std::unique_ptr<fs::fsa::IDirectory> stock;
                        R_TRY(m_stock_file_system->OpenDirectory(std::addressof(stock), path, m_mode));
                        while (true) {
                            fs::DirectoryEntry entry{};
                            s64 count = 0;
                            R_TRY(stock->Read(std::addressof(count), std::addressof(entry), 1));
                            if (count == 0) {
                                break;
                            }
                            ++total;
                        }
                    }

                    *out = total;
                    R_SUCCEED();
                }

            public:
                virtual sf::cmif::DomainObjectId GetDomainObjectId() const override {
                    AMS_ABORT();
                }
        };

        class UsbContentOverlayFileSystem final : public fs::fsa::IFileSystem, public fs::impl::Newable {
            NON_COPYABLE(UsbContentOverlayFileSystem);
            NON_MOVEABLE(UsbContentOverlayFileSystem);

            private:
                usb::storage::Service *m_service;
                std::shared_ptr<fs::fsa::IFileSystem> m_stock;
                const u64 m_pinned_generation;

            public:
                UsbContentOverlayFileSystem(usb::storage::Service *service,
                                            std::shared_ptr<fs::fsa::IFileSystem> stock, u64 pinned_generation = 0)
                    : m_service(service), m_stock(std::move(stock)), m_pinned_generation(pinned_generation) {
                    AMS_ASSERT(m_service != nullptr && (m_stock != nullptr || m_pinned_generation != 0));
                }

            private:
                bool GetUsbRoute(u64 *out_generation, bool writable) const {
                    // Pinned handles never select another drive, even if the live
                    // route changes after CheckPinnedRoute. Every volume call
                    // checks this original generation again under its lock.
                    if (m_pinned_generation) { *out_generation = m_pinned_generation; return true; }
                    // Ordinary SD handles expose registered USB content for
                    // launches only. USB writes require the explicit pinned
                    // command-29 handle; mounting a drive cannot select it.
                    if (writable) return false;
                    return IsUsbContentFileSystemRouteReady(out_generation, writable);
                }

                bool IsUsbReadPath(const fs::Path &path) const {
                    // Placeholders belong to their installing handle. An old
                    // USB placeholder must never shadow a native SD install.
                    return m_pinned_generation ? IsUsbContentDataPath(path)
                        : IsPathInTree(path.GetString(), RegisteredDirectory);
                }

                Result HasNativeEntry(bool *out, const fs::Path &path) const {
                    *out = false;
                    R_SUCCEED_IF(m_pinned_generation || m_stock == nullptr);
                    fs::DirectoryEntryType type{};
                    const Result result = m_stock->GetEntryType(std::addressof(type), path);
                    R_SUCCEED_IF(fs::ResultPathNotFound::Includes(result));
                    R_TRY(result);
                    *out = true;
                    R_SUCCEED();
                }

                Result CheckPinnedRoute() const {
                    if (!m_pinned_generation) R_SUCCEED();
                    u64 current = 0;
                    R_UNLESS(IsUsbContentFileSystemRouteReady(&current, true) && current == m_pinned_generation,
                        fs::ResultSdCardNotPresent());
                    R_SUCCEED();
                }

                Result GetUsbEntryForPath(const fs::Path &path, u64 generation,
                                          usb::volume::FileInfo *out, bool *out_found) const {
                    char value[fs::EntryNameLengthMax + 1]{};
                    R_TRY(CopyPath(value, sizeof(value), path));
                    R_RETURN(GetUsbEntry(m_service, value, generation, out, out_found));
                }

                virtual Result DoCreateFile(const fs::Path &path, s64 size, int flags) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (IsUsbContentDataPath(path) && this->GetUsbRoute(std::addressof(generation), true)) {
                        content_stats::create_attempts.fetch_add(1, std::memory_order_relaxed);
                        const Result result = [&]() -> Result {
                            R_TRY(EnsureUsbContentRootDirectory(m_service, path, generation));
                            R_RETURN(ConvertUsbFileSystemResult(m_service->CreateFileAtGeneration(
                                path.GetString(), static_cast<u64>(size), generation)));
                        }();
                        if (R_SUCCEEDED(result)) { content_stats::creates.fetch_add(1, std::memory_order_relaxed); }
                        content_stats::ResultRecorded(result.GetValue());
                        R_TRY(result);
                        R_SUCCEED();
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->CreateFile(path, size, flags));
                }

                virtual Result DoDeleteFile(const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (IsUsbContentDataPath(path) && this->GetUsbRoute(std::addressof(generation), true)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        if (found) {
                            R_UNLESS(!info.directory, fs::ResultPathNotFound());
                            R_RETURN(ConvertUsbFileSystemResult(m_service->DeleteFileAtGeneration(
                                path.GetString(), generation)));
                        }
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->DeleteFile(path));
                }

                virtual Result DoCreateDirectory(const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (IsUsbContentDataPath(path) && this->GetUsbRoute(std::addressof(generation), true)) {
                        const char *root = GetUsbContentRootPath(path);
                        if (root != nullptr && std::strcmp(path.GetString(), root) != 0) {
                            R_TRY(EnsureUsbContentRootDirectory(m_service, path, generation));
                        }
                        R_RETURN(ConvertUsbFileSystemResult(m_service->CreateDirectoryAtGeneration(
                            path.GetString(), generation)));
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->CreateDirectory(path));
                }

                virtual Result DoDeleteDirectory(const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (IsUsbContentDataPath(path) && this->GetUsbRoute(std::addressof(generation), true)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        if (found) {
                            R_UNLESS(info.directory, fs::ResultPathNotFound());
                            R_RETURN(ConvertUsbFileSystemResult(m_service->DeleteFileAtGeneration(
                                path.GetString(), generation)));
                        }
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->DeleteDirectory(path));
                }

                virtual Result DoDeleteDirectoryRecursively(const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (IsUsbContentDataPath(path) && this->GetUsbRoute(std::addressof(generation), true)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        if (found) {
                            R_UNLESS(info.directory, fs::ResultPathNotFound());
                            R_TRY(DeleteUsbDirectoryContents(m_service, path.GetString(), generation));
                            R_RETURN(ConvertUsbFileSystemResult(m_service->DeleteFileAtGeneration(
                                path.GetString(), generation)));
                        }
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->DeleteDirectoryRecursively(path));
                }

                Result Rename(const fs::Path &old_path, const fs::Path &new_path, bool directory) {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (IsUsbContentDataPath(old_path) && IsUsbContentDataPath(new_path)
                        && this->GetUsbRoute(std::addressof(generation), true)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(old_path, generation,
                            std::addressof(info), std::addressof(found)));
                        if (found) {
                            R_UNLESS(info.directory == directory, fs::ResultPathNotFound());
                            R_TRY(EnsureUsbContentRootDirectory(m_service, new_path, generation));
                            R_TRY(ConvertUsbFileSystemResult(m_service->RenameFileAtGeneration(
                                old_path.GetString(), new_path.GetString(), generation)));
                            content_stats::renames.fetch_add(1, std::memory_order_relaxed);
                            content_stats::ResultRecorded(0);
                            R_SUCCEED();
                        }
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    if (!directory && IsUsbContentDataPath(old_path)
                        && IsPathInTree(new_path.GetString(), RegisteredDirectory)) {
                        // NCM's EnsureContentDirectory queries the merged read
                        // view. A USB-only /registered/<bucket> can therefore
                        // look present even though the real SD parent is absent.
                        // Register then renames a valid SD placeholder and gets
                        // PathNotFound (reported as NCM PlaceHolderNotFound).
                        // Validate the source and prepare the destination on
                        // the stock filesystem only; never copy from USB or
                        // replace an existing destination to make this succeed.
                        fs::DirectoryEntryType source_type{};
                        R_TRY(m_stock->GetEntryType(std::addressof(source_type), old_path));
                        R_UNLESS(source_type == fs::DirectoryEntryType_File, fs::ResultPathNotFound());
                        fs::Path parent;
                        R_TRY(parent.Initialize(new_path));
                        R_TRY(parent.RemoveChild());
                        R_TRY(fssystem::EnsureDirectory(m_stock.get(), parent));
                    }
                    R_RETURN(directory ? m_stock->RenameDirectory(old_path, new_path)
                                      : m_stock->RenameFile(old_path, new_path));
                }

                virtual Result DoRenameFile(const fs::Path &old_path, const fs::Path &new_path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    R_RETURN(this->Rename(old_path, new_path, false));
                }

                virtual Result DoRenameDirectory(const fs::Path &old_path, const fs::Path &new_path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    R_RETURN(this->Rename(old_path, new_path, true));
                }

                virtual Result DoGetEntryType(fs::DirectoryEntryType *out, const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    if (!m_pinned_generation) {
                        const Result result = m_stock->GetEntryType(out, path);
                        if (!fs::ResultPathNotFound::Includes(result)) R_RETURN(result);
                    }
                    if (m_pinned_generation && IsRootPath(path)) {
                        *out = fs::DirectoryEntryType_Directory;
                        R_SUCCEED();
                    }
                    u64 generation = 0;
                    if (this->IsUsbReadPath(path) && this->GetUsbRoute(std::addressof(generation), false)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        if (found) {
                            *out = info.directory ? fs::DirectoryEntryType_Directory
                                                  : fs::DirectoryEntryType_File;
                            R_SUCCEED();
                        }
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->GetEntryType(out, path));
                }

                virtual Result DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out_file,
                                          const fs::Path &path, fs::OpenMode mode) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    const bool writable = (mode & fs::OpenMode_Write) != 0;
                    if (!writable) {
                        bool native = false;
                        R_TRY(this->HasNativeEntry(std::addressof(native), path));
                        if (native) R_RETURN(m_stock->OpenFile(out_file, path, mode));
                    }
                    if (this->IsUsbReadPath(path) && this->GetUsbRoute(std::addressof(generation), writable)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        if (found) {
                            R_UNLESS(!info.directory, fs::ResultPathNotFound());
                            auto file = std::make_unique<UsbContentFile>(m_service, path, mode, generation);
                            R_UNLESS(file != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());
                            R_TRY(file->AcquireLease());
                            *out_file = std::move(file);
                            R_SUCCEED();
                        }
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->OpenFile(out_file, path, mode));
                }

                virtual Result DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *out_dir,
                                               const fs::Path &path, fs::OpenDirectoryMode mode) override final {
                    R_TRY(this->CheckPinnedRoute());
                    if (!m_pinned_generation) {
                        fs::DirectoryEntryType type{};
                        const Result result = m_stock->GetEntryType(std::addressof(type), path);
                        if (!fs::ResultPathNotFound::Includes(result)) {
                            R_TRY(result);
                            if (type != fs::DirectoryEntryType_Directory)
                                R_RETURN(m_stock->OpenDirectory(out_dir, path, mode));
                        }
                    }
                    u64 generation = 0;
                    if ((IsRootPath(path) || this->IsUsbReadPath(path)) && this->GetUsbRoute(std::addressof(generation), false)) {
                        auto directory = std::make_unique<UsbContentOverlayDirectory>(
                            m_service, m_stock, mode, generation);
                        R_UNLESS(directory != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());
                        R_TRY(directory->Initialize(path));
                        *out_dir = std::move(directory);
                        R_SUCCEED();
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->OpenDirectory(out_dir, path, mode));
                }

                virtual Result DoCommit() override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (this->GetUsbRoute(std::addressof(generation), true)) {
                        R_TRY(ConvertUsbFileSystemResult(m_service->FlushAtGeneration(generation)));
                    }
                    if (!m_stock) R_SUCCEED();
                    R_RETURN(m_stock->Commit());
                }

                virtual Result DoGetFreeSpaceSize(s64 *out, const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if ((IsRootPath(path) || IsUsbContentDataPath(path))
                        && this->GetUsbRoute(std::addressof(generation), true)) {
                        AMS_UNUSED(generation);
                        u64 free_size = 0;
                        u64 total_size = 0;
                        R_TRY(ConvertUsbFileSystemResult(m_service->GetSpaceSize(
                            std::addressof(free_size), std::addressof(total_size))));
                        R_UNLESS(free_size <= static_cast<u64>(std::numeric_limits<s64>::max()),
                            fs::ResultOutOfRange());
                        R_TRY(this->CheckPinnedRoute());
                        *out = static_cast<s64>(free_size);
                        R_SUCCEED();
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->GetFreeSpaceSize(out, path));
                }

                virtual Result DoGetTotalSpaceSize(s64 *out, const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if ((IsRootPath(path) || IsUsbContentDataPath(path))
                        && this->GetUsbRoute(std::addressof(generation), true)) {
                        AMS_UNUSED(generation);
                        u64 free_size = 0;
                        u64 total_size = 0;
                        R_TRY(ConvertUsbFileSystemResult(m_service->GetSpaceSize(
                            std::addressof(free_size), std::addressof(total_size))));
                        R_UNLESS(total_size <= static_cast<u64>(std::numeric_limits<s64>::max()),
                            fs::ResultOutOfRange());
                        R_TRY(this->CheckPinnedRoute());
                        *out = static_cast<s64>(total_size);
                        R_SUCCEED();
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->GetTotalSpaceSize(out, path));
                }

                virtual Result DoCleanDirectoryRecursively(const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (IsUsbContentDataPath(path) && this->GetUsbRoute(std::addressof(generation), true)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        if (found) {
                            R_UNLESS(info.directory, fs::ResultPathNotFound());
                            R_RETURN(DeleteUsbDirectoryContents(m_service, path.GetString(), generation));
                        }
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->CleanDirectoryRecursively(path));
                }

                virtual Result DoGetFileTimeStampRaw(fs::FileTimeStampRaw *out,
                                                     const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    bool native = false;
                    R_TRY(this->HasNativeEntry(std::addressof(native), path));
                    if (native) R_RETURN(m_stock->GetFileTimeStampRaw(out, path));
                    u64 generation = 0;
                    if (this->IsUsbReadPath(path) && this->GetUsbRoute(std::addressof(generation), false)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        R_UNLESS(!found, fs::ResultNotImplemented());
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->GetFileTimeStampRaw(out, path));
                }

                virtual Result DoQueryEntry(char *dst, size_t dst_size, const char *src,
                                            size_t src_size, fs::fsa::QueryId query,
                                            const fs::Path &path) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    // QueryEntry can change attributes/concatenation state.
                    // Treat all queries as destination-sensitive operations.
                    if (IsUsbContentDataPath(path) && this->GetUsbRoute(std::addressof(generation), true)) {
                        usb::volume::FileInfo info{};
                        bool found = false;
                        R_TRY(this->GetUsbEntryForPath(path, generation, std::addressof(info), std::addressof(found)));
                        R_UNLESS(!found, fs::ResultNotImplemented());
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->QueryEntry(dst, dst_size, src, src_size, query, path));
                }

                virtual Result DoCommitProvisionally(s64 counter) override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (this->GetUsbRoute(std::addressof(generation), true)) {
                        R_TRY(ConvertUsbFileSystemResult(m_service->FlushAtGeneration(generation)));
                    }
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->CommitProvisionally(counter));
                }

                virtual Result DoRollback() override final {
                    R_TRY(this->CheckPinnedRoute());
                    R_UNLESS(m_stock != nullptr, fs::ResultPathNotFound());
                    R_RETURN(m_stock->Rollback());
                }

                virtual Result DoFlush() override final {
                    R_TRY(this->CheckPinnedRoute());
                    u64 generation = 0;
                    if (this->GetUsbRoute(std::addressof(generation), true)) {
                        R_TRY(ConvertUsbFileSystemResult(m_service->FlushAtGeneration(generation)));
                    }
                    if (!m_stock) R_SUCCEED();
                    R_RETURN(m_stock->Flush());
                }
        };

    }

    Result CreatePinnedUsbContentFileSystem(std::unique_ptr<fs::fsa::IFileSystem> *out, u64 generation) {
        u64 current = 0;
        R_UNLESS(generation != 0 && IsUsbContentFileSystemRouteReady(&current, true) && current == generation,
            fs::ResultSdCardNotPresent());
        // No native SD object is opened or retained by this USB-only handle.
        auto file_system = std::make_unique<UsbContentOverlayFileSystem>(
            GetUsbContentStorageService(), nullptr, generation);
        R_UNLESS(file_system != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());
        *out = std::move(file_system);
        R_SUCCEED();
    }

    Result CreateUsbContentOverlayFileSystem(
        std::unique_ptr<fs::fsa::IFileSystem> *out,
        std::shared_ptr<fs::fsa::IFileSystem> stock_file_system) {
        R_UNLESS(out != nullptr && stock_file_system != nullptr, fs::ResultNullptrArgument());
        auto file_system = std::make_unique<UsbContentOverlayFileSystem>(
            GetUsbContentStorageService(), std::move(stock_file_system));
        R_UNLESS(file_system != nullptr, fs::ResultAllocationMemoryFailedMakeUnique());
        *out = std::move(file_system);
        R_SUCCEED();
    }

}
