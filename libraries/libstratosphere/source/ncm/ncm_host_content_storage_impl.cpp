/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>
#include <stratosphere/ncm/ncm_usb_title_map.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include "ncm_host_content_storage_impl.hpp"
#include "ncm_placeholder_accessor.hpp"

namespace ams::ncm {

    namespace {

        constexpr const char *UsbMountName = "usbcontent";
        constexpr const char *RegisteredDirectory = "/registered";
        /* Match the SD-card NCM layout exactly.  The stock placeholder
         * directory is intentionally named `placehld`; Horizon and the
         * stock ContentStorageImpl use this spelling. */
        constexpr const char *PlaceHolderDirectory = "/placehld";
        constexpr size_t DirectoryBatchSize = 32;
        /* Keep the IPC/write granularity large enough to avoid turning an
         * SSD install into thousands of small service transactions.  The
         * underlying mass-storage path already supports multi-megabyte I/O;
         * 1 MiB is a conservative MapAlias-sized step for NCM's proxy. */
        constexpr size_t UsbProxyTransferSize = 1_MB;
        constexpr size_t UsbReadyPollCount = 20;
        constexpr size_t UsbRouteReadyPollCount = 50;
        constexpr TimeSpan UsbReadyPollInterval = TimeSpan::FromMilliSeconds(100);

        /* NCM receives placeholder data through an IPC MapAlias buffer.  That
         * borrowed mapping cannot safely be forwarded as another MapAlias to
         * ams:usb, so copy each transfer through memory owned by NCM.  Keep the
         * buffer in .bss: NCM's 1 MiB heap and small worker stacks must not be
         * charged for installation-sized transfer storage. */
        alignas(0x1000) u8 g_usb_proxy_transfer_buffer[UsbProxyTransferSize];

        class UsbContentProxy {
            NON_COPYABLE(UsbContentProxy);
            NON_MOVEABLE(UsbContentProxy);

            private:
                os::SdkMutex m_mutex{};
                ::Service m_service{};
                bool m_initialized{false};
                bool m_started{false};

            private:
                Result EnsureInitializedLocked() {
                    R_SUCCEED_IF(m_initialized && serviceIsActive(std::addressof(m_service)));

                    os::NativeHandle handle;
                    R_TRY(sm::GetServiceHandle(std::addressof(handle), sm::ServiceName::Encode(usb::content::ServiceName)));
                    serviceCreate(std::addressof(m_service), handle);
                    m_initialized = true;
                    R_SUCCEED();
                }

                Result GetPathSize(size_t *out, const char *path) const {
                    R_UNLESS(out != nullptr && path != nullptr, usb::ResultInvalidParameter());
                    const size_t size = strnlen(path, fs::EntryNameLengthMax);
                    R_UNLESS(size > 0 && size < fs::EntryNameLengthMax, usb::ResultInvalidParameter());
                    *out = size + 1;
                    R_SUCCEED();
                }

            public:
                UsbContentProxy() = default;

                ~UsbContentProxy() {
                    if (m_initialized && serviceIsActive(std::addressof(m_service))) {
                        serviceClose(std::addressof(m_service));
                    }
                }

                Result Start() {
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    if (!m_started) {
                        R_TRY(serviceDispatch(std::addressof(m_service), usb::content::CommandId_Start));
                        m_started = true;
                    }
                    R_SUCCEED();
                }

                bool IsRunning() {
                    usb::storage::Status status{};
                    return R_SUCCEEDED(this->GetStatus(std::addressof(status))) && status.running;
                }

                Result GetStatus(usb::storage::Status *out) {
                    R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
                    std::scoped_lock lk(m_mutex);
                    R_UNLESS(m_started, usb::ResultNotInitialized());
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchOut(std::addressof(m_service), usb::content::CommandId_GetStorageStatus, *out));
                }

                Result GetFileInfoAtGeneration(const char *path, usb::volume::FileInfo *out, u64 generation) {
                    R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    const usb::content::GenerationRequest request{generation};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchInOut(std::addressof(m_service), usb::content::CommandId_GetFileInfo, request, *out,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                        .buffers = { { path, path_size } }));
                }

                Result ReadFileAtGeneration(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read, u64 generation) {
                    R_UNLESS(out_read != nullptr && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());

                    size_t total_read = 0;
                    while (total_read < size) {
                        R_UNLESS(offset <= std::numeric_limits<u64>::max() - total_read, usb::ResultInvalidParameter());
                        const size_t chunk_size = std::min(size - total_read, UsbProxyTransferSize);
                        const usb::content::IoRequest request{offset + total_read, generation};
                        u64 read = 0;
                        R_TRY(serviceDispatchInOut(std::addressof(m_service), usb::content::CommandId_ReadFile, request, read,
                            .buffer_attrs = {
                                SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                                SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                            },
                            .buffers = {
                                { g_usb_proxy_transfer_buffer, chunk_size },
                                { path, path_size },
                            }));
                        R_UNLESS(read <= chunk_size, usb::ResultTransactionError());
                        if (read > 0) {
                            std::memcpy(static_cast<u8 *>(buffer) + total_read, g_usb_proxy_transfer_buffer, static_cast<size_t>(read));
                        }
                        total_read += static_cast<size_t>(read);
                        if (read < chunk_size) {
                            break;
                        }
                    }

                    *out_read = total_read;
                    R_SUCCEED();
                }

                Result ReadDirectoryAtGeneration(const char *path, usb::volume::DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count, u64 generation) {
                    R_UNLESS(out != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    const usb::content::DirectoryRequest request{skip_entries, generation, include_directories ? u8{1} : u8{0}, include_files ? u8{1} : u8{0}, {}};
                    u64 count = 0;
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_TRY(serviceDispatchInOut(std::addressof(m_service), usb::content::CommandId_ReadDirectory, request, count,
                        .buffer_attrs = {
                            SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                        },
                        .buffers = {
                            { out, max_entries * sizeof(*out) },
                            { path, path_size },
                        }));
                    R_UNLESS(count <= max_entries, usb::ResultTransactionError());
                    *out_count = static_cast<size_t>(count);
                    R_SUCCEED();
                }

                Result GetSpaceSize(u64 *out_free_size, u64 *out_total_size) {
                    R_UNLESS(out_free_size != nullptr && out_total_size != nullptr, usb::ResultInvalidParameter());
                    usb::content::SpaceInfo info{};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_TRY(serviceDispatchOut(std::addressof(m_service), usb::content::CommandId_GetSpaceSize, info));
                    *out_free_size = info.free_size;
                    *out_total_size = info.total_size;
                    R_SUCCEED();
                }

                Result GetNcaRightsId(const char *path, fs::RightsId *out_rights_id) {
                    R_UNLESS(path != nullptr && out_rights_id != nullptr, usb::ResultInvalidParameter());
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchOut(std::addressof(m_service), usb::content::CommandId_GetNcaRightsId, *out_rights_id,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                        .buffers = { { path, path_size } }));
                }

                Result GetNcaProgramId(const char *path, ncm::ProgramId *out_program_id) {
                    R_UNLESS(path != nullptr && out_program_id != nullptr, usb::ResultInvalidParameter());
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchOut(std::addressof(m_service), usb::content::CommandId_GetNcaProgramId, *out_program_id,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                        .buffers = { { path, path_size } }));
                }

                Result CreateFileAtGeneration(const char *path, u64 size, u64 generation) {
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    const usb::content::SizeRequest request{size, generation};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchIn(std::addressof(m_service), usb::content::CommandId_CreateFile, request,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                        .buffers = { { path, path_size } }));
                }

                Result ResizeFileAtGeneration(const char *path, u64 size, u64 generation) {
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    const usb::content::SizeRequest request{size, generation};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchIn(std::addressof(m_service), usb::content::CommandId_ResizeFile, request,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                        .buffers = { { path, path_size } }));
                }

                Result DeleteFileAtGeneration(const char *path, u64 generation) {
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    const usb::content::GenerationRequest request{generation};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchIn(std::addressof(m_service), usb::content::CommandId_DeleteFile, request,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                        .buffers = { { path, path_size } }));
                }

                Result RenameFileAtGeneration(const char *old_path, const char *new_path, u64 generation) {
                    size_t old_path_size = 0;
                    size_t new_path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(old_path_size), old_path));
                    R_TRY(this->GetPathSize(std::addressof(new_path_size), new_path));
                    const usb::content::GenerationRequest request{generation};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchIn(std::addressof(m_service), usb::content::CommandId_RenameFile, request,
                        .buffer_attrs = {
                            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                        },
                        .buffers = {
                            { old_path, old_path_size },
                            { new_path, new_path_size },
                        }));
                }

                Result WriteFileAtGeneration(const char *path, u64 offset, const void *buffer, size_t size, size_t *out_written, u64 generation) {
                    R_UNLESS(out_written != nullptr && (size == 0 || buffer != nullptr), usb::ResultInvalidParameter());
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());

                    size_t total_written = 0;
                    while (total_written < size) {
                        R_UNLESS(offset <= std::numeric_limits<u64>::max() - total_written, usb::ResultInvalidParameter());
                        const size_t chunk_size = std::min(size - total_written, UsbProxyTransferSize);
                        const usb::content::IoRequest request{offset + total_written, generation};
                        u64 written = 0;
                        std::memcpy(g_usb_proxy_transfer_buffer, static_cast<const u8 *>(buffer) + total_written, chunk_size);
                        R_TRY(serviceDispatchInOut(std::addressof(m_service), usb::content::CommandId_WriteFile, request, written,
                            .buffer_attrs = {
                                SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                                SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                            },
                            .buffers = {
                                { g_usb_proxy_transfer_buffer, chunk_size },
                                { path, path_size },
                            }));
                        R_UNLESS(written == chunk_size, usb::ResultTransactionError());
                        total_written += static_cast<size_t>(written);
                    }

                    *out_written = total_written;
                    R_SUCCEED();
                }

                Result CreateDirectoryAtGeneration(const char *path, u64 generation) {
                    size_t path_size = 0;
                    R_TRY(this->GetPathSize(std::addressof(path_size), path));
                    const usb::content::GenerationRequest request{generation};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchIn(std::addressof(m_service), usb::content::CommandId_CreateDirectory, request,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                        .buffers = { { path, path_size } }));
                }

                Result FlushAtGeneration(u64 generation) {
                    const usb::content::GenerationRequest request{generation};
                    std::scoped_lock lk(m_mutex);
                    R_TRY(this->EnsureInitializedLocked());
                    R_RETURN(serviceDispatchIn(std::addressof(m_service), usb::content::CommandId_Flush, request));
                }
        };

        struct UsbContentContext {
            UsbContentProxy storage;
            bool filesystem_registered{false};
        };

        /* Keep the large USB worker state out of NCM's boot-time image.  The
         * context is allocated only after the Host backend is actually used
         * with the feature enabled. */
        constinit std::unique_ptr<UsbContentContext> g_usb_context;
        constinit os::SdkMutex g_usb_start_mutex;

        UsbContentContext *GetUsbContext() {
            AMS_ASSERT(g_usb_context != nullptr);
            return g_usb_context.get();
        }

        UsbContentProxy &GetUsbStorage() {
            return GetUsbContext()->storage;
        }

        bool &GetUsbFilesystemRegistered() {
            return GetUsbContext()->filesystem_registered;
        }

        void MakeContentRelativePath(PathString *out, ContentId content_id) {
            MakeSha256HierarchicalContentFilePath_ForFat16KCluster(out, content_id, RegisteredDirectory);
        }

        void MakePlaceHolderRelativePath(PathString *out, PlaceHolderId placeholder_id) {
            MakeSha256HierarchicalPlaceHolderFilePath_ForFat16KCluster(out, placeholder_id, PlaceHolderDirectory);
        }

        void MakeMountedPath(Path *out, const PathString &relative_path) {
            char path[fs::EntryNameLengthMax]{};
            util::TSNPrintf(path, sizeof(path), "%s:%s", UsbMountName, relative_path.Get());
            *out = Path::Encode(path);
        }

        void MakeStockVisiblePath(Path *out, const PathString &relative_path) {
            /* Keep the logical SD content-storage identity that Horizon
             * normally returns from NCM.  Returning the physical SD mount
             * (@Sdcard:/Nintendo/Contents/...) lets simple callers find the
             * filename, but AM 22.5.0 rejects that non-canonical identity
             * before it hands the launch to PGL.  The FS/Loader compatibility
             * layer recognizes @SdCardContent: and redirects only mapped
             * content bytes to USB, so tickets, metadata and ProgramLocation
             * remain on Horizon's normal SD path. */
            if (IsUsbContentNativePathEnabled()) {
                MakeMountedPath(out, relative_path);
                return;
            }

            char path[fs::EntryNameLengthMax]{};
            /* Match fs::ConvertToFsCommonPath's SD content spelling.  The
             * extra separator is intentional and matches stock NCM output:
             * @SdCardContent://registered/.... */
            util::TSNPrintf(path, sizeof(path), "@SdCardContent:/%s", relative_path.Get());
            *out = Path::Encode(path);
        }

        Result ConvertUsbResult(Result result) {
            if (usb::ResultFileNotFound::Includes(result)) {
                R_RETURN(ncm::ResultContentNotFound());
            }
            if (usb::ResultAlreadyInitialized::Includes(result)) {
                R_RETURN(ncm::ResultContentAlreadyExists());
            }
            R_RETURN(result);
        }

        Result EnsureUsbServiceStarted() {
            std::scoped_lock lk(g_usb_start_mutex);

            if (g_usb_context == nullptr) {
                auto context = std::unique_ptr<UsbContentContext>(new (std::nothrow) UsbContentContext());
                R_UNLESS(context != nullptr, usb::ResultMemAllocFailure());
                g_usb_context = std::move(context);
            }

            if (!GetUsbStorage().IsRunning()) {
                R_TRY(GetUsbStorage().Start());
            }

            R_SUCCEED();
        }

        Result EnsureUsbReady(bool writable, u64 *out_generation);

        Result EnsureUsbFilesystemRegistered() {
            /* The filesystem is mounted in ams.mitm, which owns usb:hs.
             * NCM performs content operations through the proxy and keeps
             * this flag only as status information for the Host backend. */
            R_TRY(EnsureUsbReady(false, nullptr));
            GetUsbFilesystemRegistered() = true;
            R_SUCCEED();
        }

        Result EnsureUsbReady(bool writable, u64 *out_generation) {
            /* Content operations use the USB volume API directly. Do not
             * require the optional read-only fssystem registration here:
             * it is only needed when Horizon asks for a mounted Path, and a
             * registration failure must not make NCM silently fall back to
             * the SD card while the USB volume is actually writable. */
            R_TRY(EnsureUsbServiceStarted());

            for (size_t i = 0; i < UsbReadyPollCount; ++i) {
                usb::storage::Status status{};
                R_TRY(GetUsbStorage().GetStatus(std::addressof(status)));
                if (status.state == usb::storage::State::Ready && (!writable || status.writable != 0)) {
                    if (out_generation != nullptr) {
                        *out_generation = status.media_generation;
                    }
                    R_SUCCEED();
                }
                /* A missing/read-only/failed drive is not an invitation to
                 * block NCM's single IPC worker for 20 seconds per query. */
                R_UNLESS(status.state == usb::storage::State::Starting || status.state == usb::storage::State::Mounting,
                    usb::ResultNotInitialized());
                os::SleepThread(UsbReadyPollInterval);
            }

            R_THROW(usb::ResultNotInitialized());
        }

        bool IsUsbContentStorageReadyForRoutingImpl() {
            if (!IsUsbContentEnabled()) {
                return false;
            }

            if (R_FAILED(EnsureUsbServiceStarted())) {
                return false;
            }

            /* Start() is asynchronous. The old v16 route sampled the worker
             * once immediately after starting it, so the first install almost
             * always observed WaitingForDevice and silently fell back to SD.
             * Wait for a short, bounded interval at the real content-open
             * boundary. This runs only after AMS explicitly arms the route
             * post-boot; it is never part of NCM's early initialization. */
            for (size_t i = 0; i < UsbRouteReadyPollCount; ++i) {
                usb::storage::Status status{};
                if (R_FAILED(GetUsbStorage().GetStatus(std::addressof(status)))) {
                    return false;
                }
                if (status.state == usb::storage::State::Ready && status.writable != 0) {
                    return true;
                }
                if (status.state == usb::storage::State::Error) {
                    return false;
                }
                os::SleepThread(UsbReadyPollInterval);
            }

            return false;
        }

        Result EnsureUsbDirectoryTree(const char *path, u64 generation);

        Result EnsureUsbDirectories(u64 generation) {
            for (const char *path : {RegisteredDirectory, PlaceHolderDirectory}) {
                R_TRY(EnsureUsbDirectoryTree(path, generation));
            }
            R_SUCCEED();
        }

        Result GetUsbFileInfo(const PathString &path, usb::volume::FileInfo *out, u64 *out_generation) {
            u64 generation = 0;
            R_TRY(EnsureUsbReady(false, std::addressof(generation)));
            R_TRY(ConvertUsbResult(GetUsbStorage().GetFileInfoAtGeneration(path.Get(), out, generation)));
            if (out_generation != nullptr) {
                *out_generation = generation;
            }
            R_SUCCEED();
        }

        Result IsUsbFilePresent(const PathString &path, bool *out) {
            usb::volume::FileInfo info{};
            u64 generation = 0;
            R_TRY(EnsureUsbReady(false, std::addressof(generation)));
            const auto result = GetUsbStorage().GetFileInfoAtGeneration(path.Get(), std::addressof(info), generation);
            if (usb::ResultFileNotFound::Includes(result)) {
                *out = false;
                R_SUCCEED();
            }
            R_TRY(ConvertUsbResult(result));
            *out = !info.directory;
            R_SUCCEED();
        }

        Result EnsureUsbDirectoryExists(const char *path, u64 generation) {
            usb::volume::FileInfo info{};
            const auto info_result = GetUsbStorage().GetFileInfoAtGeneration(path, std::addressof(info), generation);
            if (R_SUCCEEDED(info_result)) {
                R_UNLESS(info.directory, usb::ResultTransactionError());
                R_SUCCEED();
            }

            /* A missing directory is the normal creation path. Converting the
             * result first maps FileNotFound to NCM ContentNotFound and returns
             * before CreateDirectory can run, which prevented a clean USB
             * volume from ever receiving /placehld or /registered. */
            if (usb::ResultFileNotFound::Includes(info_result)) {
                R_RETURN(ConvertUsbResult(GetUsbStorage().CreateDirectoryAtGeneration(path, generation)));
            }

            R_RETURN(ConvertUsbResult(info_result));
        }

        /* Create every parent directory needed by a stock hierarchical NCM
         * path. The USB volume API deliberately does not create parents
         * implicitly, unlike the filesystem helpers used by SD NCM. */
        Result EnsureUsbDirectoryTree(const char *path, u64 generation) {
            char partial[fs::EntryNameLengthMax]{};
            util::TSNPrintf(partial, sizeof(partial), "%s", path);
            const size_t length = std::strlen(partial);

            for (size_t i = 1; i <= length; ++i) {
                if (i != length && partial[i] != '/') {
                    continue;
                }

                const char saved = partial[i];
                partial[i] = '\0';
                R_TRY(EnsureUsbDirectoryExists(partial, generation));
                partial[i] = saved;
            }

            R_SUCCEED();
        }

        Result EnsureUsbParentDirectory(const PathString &path, u64 generation) {
            const char *last_separator = std::strrchr(path.Get(), '/');
            R_UNLESS(last_separator != nullptr, usb::ResultInvalidParameter());

            char parent[fs::EntryNameLengthMax]{};
            const size_t parent_length = static_cast<size_t>(last_separator - path.Get());
            if (parent_length == 0) {
                util::TSNPrintf(parent, sizeof(parent), "/");
            } else {
                util::TSNPrintf(parent, sizeof(parent), "%.*s", static_cast<int>(parent_length), path.Get());
            }
            R_RETURN(EnsureUsbDirectoryTree(parent, generation));
        }

        Result GetPlaceHolderName(PlaceHolderId *out, const usb::volume::DirectoryEntryInfo &entry) {
            R_UNLESS(!entry.directory, ncm::ResultContentNotFound());
            R_RETURN(PlaceHolderAccessor::GetPlaceHolderIdFromFileName(out, entry.name));
        }

        Result GetContentName(ContentId *out, const usb::volume::DirectoryEntryInfo &entry) {
            R_UNLESS(!entry.directory, ncm::ResultContentNotFound());
            const size_t length = std::strlen(entry.name);
            R_UNLESS(length == ContentIdStringLength + 4 && std::memcmp(entry.name + ContentIdStringLength, ".nca", 4) == 0, ncm::ResultContentNotFound());
            const auto content_id = GetContentIdFromString(entry.name, ContentIdStringLength);
            R_UNLESS(content_id.has_value(), ncm::ResultContentNotFound());
            *out = *content_id;
            R_SUCCEED();
        }

        template<typename F>
        Result TraverseUsbDirectory(const char *directory, u64 generation, F f) {
            size_t skip = 0;
            while (true) {
                usb::volume::DirectoryEntryInfo entries[DirectoryBatchSize]{};
                size_t count = 0;
                R_TRY(ConvertUsbResult(GetUsbStorage().ReadDirectoryAtGeneration(directory, entries, util::size(entries), skip, true, true, std::addressof(count), generation)));
                if (count == 0) {
                    break;
                }

                for (size_t i = 0; i < count; ++i) {
                    PathString path;
                    path.AssignFormat("%s/%s", directory, entries[i].name);
                    if (entries[i].directory) {
                        R_TRY(TraverseUsbDirectory(path.Get(), generation, f));
                    } else {
                        R_TRY(f(entries[i]));
                    }
                }
                skip += count;
            }

            R_SUCCEED();
        }

        Result DeleteUsbDirectoryContents(const char *directory, u64 generation) {
            while (true) {
                usb::volume::DirectoryEntryInfo entry{};
                size_t count = 0;
                R_TRY(ConvertUsbResult(GetUsbStorage().ReadDirectoryAtGeneration(directory, std::addressof(entry), 1, 0, true, true, std::addressof(count), generation)));
                if (count == 0) {
                    break;
                }

                PathString path;
                path.AssignFormat("%s/%s", directory, entry.name);
                if (entry.directory) {
                    R_TRY(DeleteUsbDirectoryContents(path.Get(), generation));
                }
                R_TRY(ConvertUsbResult(GetUsbStorage().DeleteFileAtGeneration(path.Get(), generation)));
            }
            R_SUCCEED();
        }

        Result GetRightsIdFromPath(ncm::RightsId *out_rights_id, const Path &path, fs::ContentAttributes attr) {
            constexpr const char UsbContentPrefix[] = "usbcontent:";
            if (std::strncmp(path.str, UsbContentPrefix, sizeof(UsbContentPrefix) - 1) == 0) {
                AMS_UNUSED(attr);
                R_TRY(GetUsbStorage().GetNcaRightsId(path.str, std::addressof(out_rights_id->id)));
                out_rights_id->key_generation = 0;
                R_SUCCEED();
            }

            if (hos::GetVersion() >= hos::Version_3_0_0) {
                R_TRY(fs::GetRightsId(std::addressof(out_rights_id->id), std::addressof(out_rights_id->key_generation), path.str, attr));
            } else {
                R_TRY(fs::GetRightsId(std::addressof(out_rights_id->id), path.str, attr));
                out_rights_id->key_generation = 0;
            }
            R_SUCCEED();
        }

        Result GetRightsIdForPlaceHolder(ncm::RightsId *out_rights_id, PlaceHolderId placeholder_id, fs::ContentAttributes attr) {
            PathString path;
            MakePlaceHolderRelativePath(std::addressof(path), placeholder_id);
            R_TRY(EnsureUsbReady(false, nullptr));
            R_TRY(EnsureUsbFilesystemRegistered());
            Path mounted_path;
            MakeMountedPath(std::addressof(mounted_path), path);
            R_RETURN(GetRightsIdFromPath(out_rights_id, mounted_path, attr));
        }

    }

    bool IsUsbContentStorageReadyForRouting() {
        return IsUsbContentStorageReadyForRoutingImpl();
    }

    Result GetUsbContentStatusImpl(UsbContentStatus *out) {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());

        *out = {};
        out->enabled = IsUsbContentEnabled() ? 1 : 0;

        /* Status is deliberately independent from the route permission. The
         * monitor starts this after Horizon has settled, so the overlay can
         * show a ready drive while DBI and cartridge checks continue to use
         * stock storage. Only content operations are allowed to consult the
         * enabled flag when selecting Host. */
        const auto start_result = EnsureUsbServiceStarted();
        if (R_FAILED(start_result)) {
            out->last_result = static_cast<Result>(start_result).GetValue();
            R_SUCCEED();
        }

        usb::storage::Status storage_status{};
        const auto status_result = GetUsbStorage().GetStatus(std::addressof(storage_status));
        if (R_FAILED(status_result)) {
            out->last_result = static_cast<Result>(status_result).GetValue();
            R_SUCCEED();
        }
        out->running = storage_status.running ? 1 : 0;
        out->ready = storage_status.state == usb::storage::State::Ready ? 1 : 0;
        out->writable = storage_status.writable ? 1 : 0;
        out->mounted = out->ready;
        out->registered = out->ready || GetUsbFilesystemRegistered() ? 1 : 0;
        out->filesystem_type = static_cast<u8>(storage_status.filesystem_type);
        out->state = static_cast<u8>(storage_status.state);
        out->last_result = storage_status.last_result;
        out->last_io_result = storage_status.last_io_result;
        out->media_generation = storage_status.media_generation;
        out->probe_stage = storage_status.probe_stage;
        out->probe_result = storage_status.probe_result;
        out->available_interface_count = storage_status.available_interface_count;
        out->acquired_interface_count = storage_status.acquired_interface_count;
        out->probe_attempts = storage_status.probe_attempts;
        std::memcpy(out->partition_name, storage_status.partition_name, sizeof(out->partition_name));
        std::memcpy(out->vendor, storage_status.vendor, sizeof(out->vendor));
        std::memcpy(out->product, storage_status.product, sizeof(out->product));
        std::memcpy(out->revision, storage_status.revision, sizeof(out->revision));

        if (out->ready) {
            u64 free_space = 0;
            u64 total_space = 0;
            if (R_SUCCEEDED(GetUsbStorage().GetSpaceSize(std::addressof(free_space), std::addressof(total_space)))) {
                out->free_space = free_space;
                out->total_space = total_space;
            }
        }
        R_SUCCEED();
    }

    Result RefreshUsbContentStatusImpl() {
        /* Do not start the AMS USB worker from a user-triggered status IPC.
         * On some Horizon/USB states this call can block the system while the
         * worker is acquiring usb:hs. The worker is now started only by the
         * guarded content-placement path, and status polling remains read
         * only. */
        R_RETURN(usb::ResultOperationDenied());
    }

    Result EnsureUsbContentServiceStarted() {
        R_RETURN(EnsureUsbServiceStarted());
    }

    Result HostContentStorageImpl::GeneratePlaceHolderId(sf::Out<PlaceHolderId> out) {
        R_TRY(this->EnsureEnabled());
        out.SetValue({util::GenerateUuid()});
        R_SUCCEED();
    }

    Result HostContentStorageImpl::CreatePlaceHolder(PlaceHolderId placeholder_id, ContentId content_id, s64 size) {
        AMS_UNUSED(content_id);
        R_UNLESS(size >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureEnabled());

        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        PathString path;
        MakePlaceHolderRelativePath(std::addressof(path), placeholder_id);
        R_TRY(EnsureUsbParentDirectory(path, generation));
        R_RETURN(ConvertUsbResult(GetUsbStorage().CreateFileAtGeneration(path.Get(), static_cast<u64>(size), generation)));
    }

    Result HostContentStorageImpl::DeletePlaceHolder(PlaceHolderId placeholder_id) {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        PathString path;
        MakePlaceHolderRelativePath(std::addressof(path), placeholder_id);
        R_RETURN(ConvertUsbResult(GetUsbStorage().DeleteFileAtGeneration(path.Get(), generation)));
    }

    Result HostContentStorageImpl::HasPlaceHolder(sf::Out<bool> out, PlaceHolderId placeholder_id) {
        R_TRY(this->EnsureEnabled());
        PathString path;
        MakePlaceHolderRelativePath(std::addressof(path), placeholder_id);
        bool present = false;
        R_TRY(IsUsbFilePresent(path, std::addressof(present)));
        out.SetValue(present);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::WritePlaceHolder(PlaceHolderId placeholder_id, s64 offset, const sf::InBuffer &data) {
        R_UNLESS(offset >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        PathString path;
        MakePlaceHolderRelativePath(std::addressof(path), placeholder_id);
        size_t written = 0;
        R_TRY(ConvertUsbResult(GetUsbStorage().WriteFileAtGeneration(path.Get(), static_cast<u64>(offset), data.GetPointer(), data.GetSize(), std::addressof(written), generation)));
        R_UNLESS(written == data.GetSize(), usb::ResultTransactionError());
        R_SUCCEED();
    }

    Result HostContentStorageImpl::Register(PlaceHolderId placeholder_id, ContentId content_id) {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        R_TRY(EnsureUsbDirectories(generation));
        PathString placeholder_path;
        PathString content_path;
        MakePlaceHolderRelativePath(std::addressof(placeholder_path), placeholder_id);
        MakeContentRelativePath(std::addressof(content_path), content_id);
        R_TRY(EnsureUsbParentDirectory(content_path, generation));

        /* Registration is the point where an installed NCA becomes visible
         * as registered content. Flush and re-open the result before telling
         * NCM that the operation succeeded; otherwise a successful rename can
         * still leave a stale FAT/exFAT directory entry at launch time. */
        const Result registration_result = [&]() -> Result {
            R_TRY(ConvertUsbResult(GetUsbStorage().RenameFileAtGeneration(placeholder_path.Get(), content_path.Get(), generation)));
            R_TRY(ConvertUsbResult(GetUsbStorage().FlushAtGeneration(generation)));

            usb::volume::FileInfo info{};
            R_TRY(ConvertUsbResult(GetUsbStorage().GetFileInfoAtGeneration(content_path.Get(), std::addressof(info), generation)));
            R_UNLESS(!info.directory, usb::ResultTransactionError());
            /* This local SD/RAM registry is also used by public NCM clients
             * (DBI/CyberFoil), which do not use Atmosphere's InstallTaskBase. */
            R_TRY(RegisterUsbContentLocation(content_id));
            R_SUCCEED();
        }();
        RecordUsbContentRegistration(registration_result);
        R_RETURN(registration_result);
    }

    Result HostContentStorageImpl::Delete(ContentId content_id) {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        PathString path;
        MakeContentRelativePath(std::addressof(path), content_id);
        R_TRY(ConvertUsbResult(GetUsbStorage().DeleteFileAtGeneration(path.Get(), generation)));
        R_RETURN(UnregisterUsbContentLocation(content_id));
    }

    Result HostContentStorageImpl::Has(sf::Out<bool> out, ContentId content_id) {
        R_TRY(this->EnsureEnabled());
        PathString path;
        MakeContentRelativePath(std::addressof(path), content_id);
        bool present = false;
        R_TRY(IsUsbFilePresent(path, std::addressof(present)));
        out.SetValue(present);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetPath(sf::Out<Path> out, ContentId content_id) {
        R_TRY(this->EnsureEnabled());
        PathString relative_path;
        MakeContentRelativePath(std::addressof(relative_path), content_id);
        Path path;
        /* Expose a stock-looking SD content path to Horizon. The matching
         * content bytes remain on USB and are opened through the loader/FS
         * redirection path. */
        MakeStockVisiblePath(std::addressof(path), relative_path);
        out.SetValue(path);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetPlaceHolderPath(sf::Out<Path> out, PlaceHolderId placeholder_id) {
        R_TRY(this->EnsureEnabled());
        PathString relative_path;
        MakePlaceHolderRelativePath(std::addressof(relative_path), placeholder_id);
        R_TRY(EnsureUsbReady(false, nullptr));
        R_TRY(EnsureUsbFilesystemRegistered());
        Path path;
        MakeMountedPath(std::addressof(path), relative_path);
        out.SetValue(path);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::CleanupAllPlaceHolder() {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        R_TRY(EnsureUsbDirectories(generation));
        R_TRY(DeleteUsbDirectoryContents(PlaceHolderDirectory, generation));
        R_SUCCEED();
    }

    Result HostContentStorageImpl::ListPlaceHolder(sf::Out<s32> out_count, const sf::OutArray<PlaceHolderId> &out_buf) {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(false, std::addressof(generation)));
        s32 written = 0;
        R_TRY(TraverseUsbDirectory(PlaceHolderDirectory, generation, [&](const usb::volume::DirectoryEntryInfo &entry) -> Result {
            if (static_cast<size_t>(written) >= out_buf.GetSize()) {
                R_SUCCEED();
            }
            R_TRY(GetPlaceHolderName(std::addressof(out_buf[written]), entry));
            ++written;
            R_SUCCEED();
        }));

        out_count.SetValue(written);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetContentCount(sf::Out<s32> out_count) {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(false, std::addressof(generation)));
        s32 count = 0;
        R_TRY(TraverseUsbDirectory(RegisteredDirectory, generation, [&](const usb::volume::DirectoryEntryInfo &entry) -> Result {
            ContentId content_id;
            R_TRY(GetContentName(std::addressof(content_id), entry));
            AMS_UNUSED(content_id);
            R_UNLESS(count < std::numeric_limits<s32>::max(), ncm::ResultBufferInsufficient());
            ++count;
            R_SUCCEED();
        }));
        out_count.SetValue(count);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::ListContentId(sf::Out<s32> out_count, const sf::OutArray<ContentId> &out_buf, s32 offset) {
        R_UNLESS(offset >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(false, std::addressof(generation)));
        s32 current = 0;
        s32 written = 0;
        R_TRY(TraverseUsbDirectory(RegisteredDirectory, generation, [&](const usb::volume::DirectoryEntryInfo &entry) -> Result {
            ContentId content_id;
            R_TRY(GetContentName(std::addressof(content_id), entry));
            if (current++ < offset) {
                R_SUCCEED();
            }
            if (static_cast<size_t>(written) >= out_buf.GetSize()) {
                R_SUCCEED();
            }
            out_buf[written++] = content_id;
            R_SUCCEED();
        }));

        out_count.SetValue(written);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetSizeFromContentId(sf::Out<s64> out_size, ContentId content_id) {
        R_TRY(this->EnsureEnabled());
        PathString path;
        MakeContentRelativePath(std::addressof(path), content_id);
        usb::volume::FileInfo info{};
        R_TRY(GetUsbFileInfo(path, std::addressof(info), nullptr));
        R_UNLESS(!info.directory && info.size <= static_cast<u64>(std::numeric_limits<s64>::max()), ncm::ResultContentNotFound());
        out_size.SetValue(static_cast<s64>(info.size));
        R_SUCCEED();
    }

    Result HostContentStorageImpl::DisableForcibly() {
        m_disabled = true;
        R_SUCCEED();
    }

    Result HostContentStorageImpl::RevertToPlaceHolder(PlaceHolderId placeholder_id, ContentId old_content_id, ContentId new_content_id) {
        AMS_UNUSED(new_content_id);
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        PathString old_path;
        PathString placeholder_path;
        MakeContentRelativePath(std::addressof(old_path), old_content_id);
        MakePlaceHolderRelativePath(std::addressof(placeholder_path), placeholder_id);
        R_TRY(EnsureUsbParentDirectory(placeholder_path, generation));
        R_RETURN(ConvertUsbResult(GetUsbStorage().RenameFileAtGeneration(old_path.Get(), placeholder_path.Get(), generation)));
    }

    Result HostContentStorageImpl::SetPlaceHolderSize(PlaceHolderId placeholder_id, s64 size) {
        R_UNLESS(size >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        PathString path;
        MakePlaceHolderRelativePath(std::addressof(path), placeholder_id);
        R_RETURN(ConvertUsbResult(GetUsbStorage().ResizeFileAtGeneration(path.Get(), static_cast<u64>(size), generation)));
    }

    Result HostContentStorageImpl::ReadContentIdFile(const sf::OutBuffer &buf, ContentId content_id, s64 offset) {
        R_UNLESS(offset >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(false, std::addressof(generation)));
        PathString path;
        MakeContentRelativePath(std::addressof(path), content_id);
        size_t read = 0;
        R_TRY(ConvertUsbResult(GetUsbStorage().ReadFileAtGeneration(path.Get(), static_cast<u64>(offset), buf.GetPointer(), buf.GetSize(), std::addressof(read), generation)));
        R_UNLESS(read == buf.GetSize(), fs::ResultOutOfRange());
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetRightsIdFromPlaceHolderIdDeprecated(sf::Out<ams::fs::RightsId> out_rights_id, PlaceHolderId placeholder_id) {
        ncm::RightsId rights_id;
        R_TRY(this->GetRightsIdFromPlaceHolderIdDeprecated2(std::addressof(rights_id), placeholder_id));
        out_rights_id.SetValue(rights_id.id);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetRightsIdFromPlaceHolderIdDeprecated2(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id) {
        R_RETURN(this->GetRightsIdFromPlaceHolderId(out_rights_id, placeholder_id, fs::ContentAttributes_None));
    }

    Result HostContentStorageImpl::GetRightsIdFromPlaceHolderId(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id, fs::ContentAttributes attr) {
        R_TRY(this->EnsureEnabled());
        ncm::RightsId rights_id;
        R_TRY(GetRightsIdForPlaceHolder(std::addressof(rights_id), placeholder_id, attr));
        out_rights_id.SetValue(rights_id);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetRightsIdFromContentIdDeprecated(sf::Out<ams::fs::RightsId> out_rights_id, ContentId content_id) {
        ncm::RightsId rights_id;
        R_TRY(this->GetRightsIdFromContentIdDeprecated2(std::addressof(rights_id), content_id));
        out_rights_id.SetValue(rights_id.id);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetRightsIdFromContentIdDeprecated2(sf::Out<ncm::RightsId> out_rights_id, ContentId content_id) {
        R_RETURN(this->GetRightsIdFromContentId(out_rights_id, content_id, fs::ContentAttributes_None));
    }

    Result HostContentStorageImpl::GetRightsIdFromContentId(sf::Out<ncm::RightsId> out_rights_id, ContentId content_id, fs::ContentAttributes attr) {
        R_TRY(this->EnsureEnabled());
        PathString relative_path;
        MakeContentRelativePath(std::addressof(relative_path), content_id);
        R_TRY(EnsureUsbReady(false, nullptr));
        R_TRY(EnsureUsbFilesystemRegistered());
        Path path;
        MakeMountedPath(std::addressof(path), relative_path);

        ncm::RightsId rights_id;
        R_TRY_CATCH(GetRightsIdFromPath(std::addressof(rights_id), path, attr)) {
            R_CATCH(fs::ResultTargetNotFound) {
                out_rights_id.SetValue({});
                R_SUCCEED();
            }
        } R_END_TRY_CATCH;
        out_rights_id.SetValue(rights_id);
        R_SUCCEED();
    }

    Result HostContentStorageImpl::WriteContentForDebug(ContentId content_id, s64 offset, const sf::InBuffer &data) {
        AMS_ABORT_UNLESS(spl::IsDevelopment());
        R_UNLESS(offset >= 0, ncm::ResultInvalidOffset());
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        PathString path;
        MakeContentRelativePath(std::addressof(path), content_id);
        size_t written = 0;
        R_TRY(ConvertUsbResult(GetUsbStorage().WriteFileAtGeneration(path.Get(), static_cast<u64>(offset), data.GetPointer(), data.GetSize(), std::addressof(written), generation)));
        R_UNLESS(written == data.GetSize(), usb::ResultTransactionError());
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetFreeSpaceSize(sf::Out<s64> out_size) {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        /* Installation selection must only consider media that can accept
         * writes. A filesystem may be readable while FatFs could not mount
         * it for writing; selecting it in that state would fail later during
         * placeholder creation instead of allowing the normal SD fallback. */
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        AMS_UNUSED(generation);
        u64 free_size = 0;
        u64 total_size = 0;
        R_TRY(ConvertUsbResult(GetUsbStorage().GetSpaceSize(std::addressof(free_size), std::addressof(total_size))));
        R_UNLESS(free_size <= static_cast<u64>(std::numeric_limits<s64>::max()), fs::ResultOutOfRange());
        out_size.SetValue(static_cast<s64>(free_size));
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetTotalSpaceSize(sf::Out<s64> out_size) {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(false, std::addressof(generation)));
        AMS_UNUSED(generation);
        u64 free_size = 0;
        u64 total_size = 0;
        R_TRY(ConvertUsbResult(GetUsbStorage().GetSpaceSize(std::addressof(free_size), std::addressof(total_size))));
        R_UNLESS(total_size <= static_cast<u64>(std::numeric_limits<s64>::max()), fs::ResultOutOfRange());
        out_size.SetValue(static_cast<s64>(total_size));
        R_SUCCEED();
    }

    Result HostContentStorageImpl::FlushPlaceHolder() {
        R_TRY(this->EnsureEnabled());
        u64 generation = 0;
        R_TRY(EnsureUsbReady(true, std::addressof(generation)));
        R_RETURN(ConvertUsbResult(GetUsbStorage().FlushAtGeneration(generation)));
    }

    Result HostContentStorageImpl::GetSizeFromPlaceHolderId(sf::Out<s64> out, PlaceHolderId placeholder_id) {
        R_TRY(this->EnsureEnabled());
        PathString path;
        MakePlaceHolderRelativePath(std::addressof(path), placeholder_id);
        usb::volume::FileInfo info{};
        R_TRY(GetUsbFileInfo(path, std::addressof(info), nullptr));
        R_UNLESS(!info.directory && info.size <= static_cast<u64>(std::numeric_limits<s64>::max()), ncm::ResultContentNotFound());
        out.SetValue(static_cast<s64>(info.size));
        R_SUCCEED();
    }

    Result HostContentStorageImpl::RepairInvalidFileAttribute() {
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetRightsIdFromPlaceHolderIdWithCacheDeprecated(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id, ContentId cache_content_id) {
        R_RETURN(this->GetRightsIdFromPlaceHolderIdWithCache(out_rights_id, placeholder_id, cache_content_id, fs::ContentAttributes_None));
    }

    Result HostContentStorageImpl::GetRightsIdFromPlaceHolderIdWithCache(sf::Out<ncm::RightsId> out_rights_id, PlaceHolderId placeholder_id, ContentId cache_content_id, fs::ContentAttributes attr) {
        AMS_UNUSED(cache_content_id);
        R_RETURN(this->GetRightsIdFromPlaceHolderId(out_rights_id, placeholder_id, attr));
    }

    Result HostContentStorageImpl::RegisterPath(const ContentId &content_id, const Path &path) {
        AMS_ABORT_UNLESS(spl::IsDevelopment());
        R_RETURN(m_registered_content->RegisterPath(content_id, path));
    }

    Result HostContentStorageImpl::ClearRegisteredPath() {
        AMS_ABORT_UNLESS(spl::IsDevelopment());
        m_registered_content->ClearPaths();
        R_SUCCEED();
    }

    Result HostContentStorageImpl::GetProgramId(sf::Out<ncm::ProgramId> out, ContentId content_id, fs::ContentAttributes attr) {
        AMS_UNUSED(attr);
        R_TRY(this->EnsureEnabled());

        /* Launch-time ProgramId queries must not call back into ams_mitm when
         * this exact content ID was already committed to the durable USB
         * title map. That callback can form a service cycle while Horizon is
         * assembling the application. The exact map snapshot was primed in
         * NCM before its IPC threads started, so this successful branch does
         * no FS or USB IPC. Keep the readiness gate, but retain the provider query only for install-time/unmapped
         * content not committed to the cache yet. */
        ncm::ProgramId program_id{};
        if (R_SUCCEEDED(ResolveUsbContentProgramId(std::addressof(program_id), content_id))) {
            out.SetValue(program_id);
            R_SUCCEED();
        }

        R_TRY(EnsureUsbReady(false, nullptr));
        PathString relative_path;
        MakeContentRelativePath(std::addressof(relative_path), content_id);
        Path path;
        MakeMountedPath(std::addressof(path), relative_path);
        R_TRY(GetUsbStorage().GetNcaProgramId(path.str, std::addressof(program_id)));
        out.SetValue(program_id);
        R_SUCCEED();
    }

}
