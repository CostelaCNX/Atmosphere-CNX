/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>
#include <stratosphere/ncm/ncm_usb_title_map.hpp>
#include "ncm_usb_limits.hpp"

namespace ams::ncm {

    namespace {

        constexpr const char TitleMapMountName[] = "#ncm-usb-map";
        constexpr const char TitleMapDirectory[] = "#ncm-usb-map:/atmosphere/usb_content";
        constexpr const char TitleMapPath[]       = "#ncm-usb-map:/atmosphere/usb_content/title_map.bin";
        constexpr const char TitleMapTempPath[]  = "#ncm-usb-map:/atmosphere/usb_content/title_map.bin.tmp";
        constexpr const char TitleMapBackupPath[] = "#ncm-usb-map:/atmosphere/usb_content/title_map.bin.bak";
        constexpr const char TitleContentMapPath[]      = "#ncm-usb-map:/atmosphere/usb_content/title_content_map.bin";
        constexpr const char TitleContentMapTempPath[] = "#ncm-usb-map:/atmosphere/usb_content/title_content_map.bin.tmp";
        constexpr const char TitleContentMapBackupPath[] = "#ncm-usb-map:/atmosphere/usb_content/title_content_map.bin.bak";
        constexpr const char RepairPendingPath[] = "#ncm-usb-map:/atmosphere/usb_content-repair.pending";

        constexpr u32 TitleMapMagic   = 0x54425355; /* "USBT" */
        constexpr u32 TitleMapVersion = 1;
        constexpr u32 TitleMapMaxEntries = usb_limits::Titles;
        constexpr u32 TitleContentMapMagic = 0x43425355; /* "USBC" */
        constexpr u32 TitleContentMapVersion = 2;
        constexpr u32 TitleContentMapMaxEntries = usb_limits::Titles;

        struct TitleMapHeader {
            u32 magic;
            u32 version;
            u32 count;
            u32 reserved;
        };

        struct TitleMapEntry {
            u64 program_id;
            u64 alias_program_id;
        };

        struct TitleContentMapHeader {
            u32 magic;
            u32 version;
            u32 count;
            u32 reserved;
        };

        struct LegacyTitleContentMapEntry {
            u64 program_id;
            ContentId control_content_id;
            ContentId program_content_id;
        };

        struct TitleContentMapEntry {
            u64 program_id;
            ContentId meta_content_id;
            ContentId control_content_id;
            ContentId program_content_id;
        };

        static_assert(sizeof(TitleMapHeader) == 0x10);
        static_assert(sizeof(TitleMapEntry) == 0x10);
        static_assert(sizeof(TitleContentMapHeader) == 0x10);
        static_assert(sizeof(LegacyTitleContentMapEntry) == 0x28);
        static_assert(sizeof(TitleContentMapEntry) == 0x38);

        constinit os::SdkMutex g_title_map_mutex;
        constinit os::SdkMutex g_program_id_cache_mutex;
        constinit bool g_title_map_storage_mounted;

        /* All callers hold g_title_map_mutex. Keep the map scratch space off
         * NCM's 16 KiB IPC stack (a v2 content map alone occupies 14 KiB). */
        TitleMapEntry *g_title_map_scratch = nullptr;
        TitleContentMapEntry *g_title_content_map_scratch = nullptr;

        /* Launch-time GetProgramId must not enter fsp-srv or ams:usb. Keep its
         * reverse map in RAM, but do not reserve another fixed 18 KiB in NCM's
         * KIP: R87-R89 all failed the hardware boot test after that static
         * growth. These buffers are allocated only when the post-mount status
         * handoff primes them, or when an install commits a durable map. */
        TitleMapEntry *g_program_id_title_cache = nullptr;
        TitleContentMapEntry *g_program_id_content_cache = nullptr;
        u32 g_program_id_title_cache_count = 0;
        u32 g_program_id_content_cache_count = 0;
        bool g_program_id_title_cache_valid = false;
        bool g_program_id_content_cache_valid = false;
        u64 g_program_id_cache_media_generation = 0;

        constexpr const char ContentLocationsPath[] = "#ncm-usb-map:/atmosphere/usb_content/content_locations.bin";
        constexpr const char ContentLocationsTempPath[] = "#ncm-usb-map:/atmosphere/usb_content/content_locations.bin.tmp";
        constexpr const char ContentLocationsBackupPath[] = "#ncm-usb-map:/atmosphere/usb_content/content_locations.bin.bak";
        constexpr u32 ContentLocationsMagic = 0x44425355; /* "USBD" */
        constexpr u32 ContentLocationsMaxEntries = usb_limits::Content;
        ContentId *g_content_locations = nullptr;
        bool *g_content_registered_this_boot = nullptr;
        u32 g_content_location_count = 0;
        bool g_content_locations_loaded = false;

        Result EnsureTitleMapStorageMounted() {
            R_SUCCEED_IF(g_title_map_storage_mounted);
            R_TRY(fs::MountSdCard(TitleMapMountName));
            g_title_map_storage_mounted = true;
            R_SUCCEED();
        }

        Result CheckNoPendingRepair() {
            R_TRY(EnsureTitleMapStorageMounted());
            fs::FileHandle file;
            const Result result = fs::OpenFile(std::addressof(file), RepairPendingPath, fs::OpenMode_Read);
            if (R_SUCCEEDED(result)) {
                fs::CloseFile(file);
                R_RETURN(ncm::ResultInvalidOperation());
            }
            R_UNLESS(fs::ResultPathNotFound::Includes(result), result);
            R_SUCCEED();
        }

        Result OpenMapForRead(fs::FileHandle *out, const char *path, const char *backup) {
            const Result result = fs::OpenFile(out, path, fs::OpenMode_Read);
            if (fs::ResultPathNotFound::Includes(result)) {
                R_RETURN(fs::OpenFile(out, backup, fs::OpenMode_Read));
            }
            R_RETURN(result);
        }

        Result VerifyMapBytes(const char *path, const void *header, size_t header_size,
                              const void *entries, size_t entries_size) {
            fs::FileHandle file;
            R_TRY(fs::OpenFile(std::addressof(file), path, fs::OpenMode_Read));
            ON_SCOPE_EXIT { fs::CloseFile(file); };
            s64 size = 0;
            R_TRY(fs::GetFileSize(std::addressof(size), file));
            R_UNLESS(size == static_cast<s64>(header_size + entries_size), ncm::ResultInvalidOperation());
            u8 buffer[256];
            size_t offset = 0;
            for (u32 part = 0; part < 2; ++part) {
                const auto *data = static_cast<const u8 *>(part == 0 ? header : entries);
                const size_t length = part == 0 ? header_size : entries_size;
                for (size_t done = 0; done < length;) {
                    const size_t count = std::min(sizeof(buffer), length - done);
                    size_t read = 0;
                    R_TRY(fs::ReadFile(std::addressof(read), file, offset, buffer, count));
                    R_UNLESS(read == count && std::memcmp(buffer, data + done, count) == 0, ncm::ResultInvalidOperation());
                    done += count;
                    offset += count;
                }
            }
            R_SUCCEED();
        }

        Result PublishMap(const char *path, const char *temp, const char *backup,
                          const void *header, size_t header_size, const void *entries, size_t entries_size) {
            R_TRY(CheckNoPendingRepair());
            R_TRY(fs::EnsureDirectory(TitleMapDirectory));
            const Result remove_temp = fs::DeleteFile(temp);
            R_UNLESS(R_SUCCEEDED(remove_temp) || fs::ResultPathNotFound::Includes(remove_temp), remove_temp);
            R_TRY(fs::CreateFile(temp, header_size + entries_size));
            {
                fs::FileHandle file;
                R_TRY(fs::OpenFile(std::addressof(file), temp, fs::OpenMode_Write));
                ON_SCOPE_EXIT { fs::CloseFile(file); };
                R_TRY(fs::WriteFile(file, 0, header, header_size, fs::WriteOption::Flush));
                if (entries_size != 0) {
                    R_TRY(fs::WriteFile(file, header_size, entries, entries_size, fs::WriteOption::Flush));
                }
            }
            R_TRY(VerifyMapBytes(temp, header, header_size, entries, entries_size));
            R_TRY(CheckNoPendingRepair());
            fs::FileHandle previous;
            const Result open_result = fs::OpenFile(std::addressof(previous), path, fs::OpenMode_Read);
            bool moved_previous = false;
            if (R_SUCCEEDED(open_result)) {
                fs::CloseFile(previous);
                const Result remove_backup = fs::DeleteFile(backup);
                R_UNLESS(R_SUCCEEDED(remove_backup) || fs::ResultPathNotFound::Includes(remove_backup), remove_backup);
                R_TRY(fs::RenameFile(path, backup));
                moved_previous = true;
            } else {
                R_UNLESS(fs::ResultPathNotFound::Includes(open_result), open_result);
            }
            const Result publish = fs::RenameFile(temp, path);
            if (R_FAILED(publish)) {
                if (moved_previous) { AMS_UNUSED(fs::RenameFile(backup, path)); }
                R_RETURN(publish);
            }
            // A failed readback is not success. Preserve both files for recovery.
            R_RETURN(VerifyMapBytes(path, header, header_size, entries, entries_size));
        }

        bool EnsureProgramIdTitleCacheAllocated() {
            if (g_program_id_title_cache == nullptr) {
                g_program_id_title_cache = new (std::nothrow) TitleMapEntry[TitleMapMaxEntries];
            }
            return g_program_id_title_cache != nullptr;
        }

        bool EnsureProgramIdContentCacheAllocated() {
            if (g_program_id_content_cache == nullptr) {
                g_program_id_content_cache = new (std::nothrow) TitleContentMapEntry[TitleContentMapMaxEntries];
            }
            return g_program_id_content_cache != nullptr;
        }

        void UpdateProgramIdTitleCache(const TitleMapEntry *entries, u32 count) {
            AMS_ASSERT(entries != nullptr || count == 0);
            AMS_ASSERT(count <= TitleMapMaxEntries);
            std::scoped_lock lk(g_program_id_cache_mutex);
            if (count > 0 && !EnsureProgramIdTitleCacheAllocated()) {
                g_program_id_title_cache_count = 0;
                g_program_id_title_cache_valid = false;
                return;
            }
            if (count > 0) {
                std::memcpy(g_program_id_title_cache, entries, static_cast<size_t>(count) * sizeof(TitleMapEntry));
            }
            g_program_id_title_cache_count = count;
            g_program_id_title_cache_valid = true;
        }

        void UpdateProgramIdContentCache(const TitleContentMapEntry *entries, u32 count) {
            AMS_ASSERT(entries != nullptr || count == 0);
            AMS_ASSERT(count <= TitleContentMapMaxEntries);
            std::scoped_lock lk(g_program_id_cache_mutex);
            if (count > 0 && !EnsureProgramIdContentCacheAllocated()) {
                g_program_id_content_cache_count = 0;
                g_program_id_content_cache_valid = false;
                return;
            }
            if (count > 0) {
                std::memcpy(g_program_id_content_cache, entries, static_cast<size_t>(count) * sizeof(TitleContentMapEntry));
            }
            g_program_id_content_cache_count = count;
            g_program_id_content_cache_valid = true;
        }

        bool ReadTitleMap(TitleMapEntry *&out_entries, u32 *out_count, bool allow_missing = false) {
            if(!out_entries)out_entries=new (std::nothrow) TitleMapEntry[TitleMapMaxEntries]{};
            if(!out_entries){*out_count=0;return false;}
            AMS_ASSERT(out_entries != nullptr);
            AMS_ASSERT(out_count != nullptr);

            *out_count = 0;

            if (R_FAILED(EnsureTitleMapStorageMounted())) {
                return false;
            }

            fs::FileHandle file;
            const Result open_result = OpenMapForRead(std::addressof(file), TitleMapPath, TitleMapBackupPath);
            if (R_FAILED(open_result)) {
                return allow_missing && fs::ResultPathNotFound::Includes(open_result);
            }
            ON_SCOPE_EXIT { fs::CloseFile(file); };

            s64 file_size = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(file_size), file))) {
                return false;
            }

            TitleMapHeader header{};
            size_t read_size = 0;
            if (file_size < static_cast<s64>(sizeof(header))
                || R_FAILED(fs::ReadFile(std::addressof(read_size), file, 0, std::addressof(header), sizeof(header)))
                || read_size != sizeof(header)
                || header.magic != TitleMapMagic
                || header.version != TitleMapVersion
                || header.reserved != 0
                || header.count > TitleMapMaxEntries) {
                return false;
            }

            const s64 expected_size = static_cast<s64>(sizeof(header)) + static_cast<s64>(header.count) * sizeof(TitleMapEntry);
            if (file_size != expected_size) {
                return false;
            }

            if (header.count > 0) {
                read_size = 0;
                if (R_FAILED(fs::ReadFile(std::addressof(read_size), file, sizeof(header), out_entries, static_cast<size_t>(header.count) * sizeof(TitleMapEntry)))
                    || read_size != static_cast<size_t>(header.count) * sizeof(TitleMapEntry)) {
                    return false;
                }
            }

            for (u32 i = 0; i < header.count; ++i) {
                if (out_entries[i].program_id == 0) { return false; }
                for (u32 j = 0; j < i; ++j) {
                    if (out_entries[i].program_id == out_entries[j].program_id) { return false; }
                }
            }
            *out_count = header.count;
            return true;
        }

        Result WriteTitleMap(const TitleMapEntry *entries, u32 count) {
            AMS_ASSERT(entries != nullptr || count == 0);
            R_UNLESS(count <= TitleMapMaxEntries, ncm::ResultBufferInsufficient());

            const TitleMapHeader header{TitleMapMagic, TitleMapVersion, count, 0};
            // Preserve last-known-good routing on failure: invalidating it would
            // send unrelated cached IDs into Host GetProgramId's USB fallback.
            // An idempotent retry refreshes RAM from the validated disk snapshot.
            R_TRY(PublishMap(TitleMapPath, TitleMapTempPath, TitleMapBackupPath,
                std::addressof(header), sizeof(header), entries, static_cast<size_t>(count) * sizeof(TitleMapEntry)));
            UpdateProgramIdTitleCache(entries, count);
            R_SUCCEED();
        }

        bool ReadTitleContentMap(TitleContentMapEntry *&out_entries, u32 *out_count, bool allow_missing = false) {
            if(!out_entries)out_entries=new (std::nothrow) TitleContentMapEntry[TitleContentMapMaxEntries]{};
            if(!out_entries){*out_count=0;return false;}
            AMS_ASSERT(out_entries != nullptr);
            AMS_ASSERT(out_count != nullptr);

            *out_count = 0;

            if (R_FAILED(EnsureTitleMapStorageMounted())) {
                return false;
            }

            fs::FileHandle file;
            const Result open_result = OpenMapForRead(std::addressof(file), TitleContentMapPath, TitleContentMapBackupPath);
            if (R_FAILED(open_result)) {
                return allow_missing && fs::ResultPathNotFound::Includes(open_result);
            }
            ON_SCOPE_EXIT { fs::CloseFile(file); };

            s64 file_size = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(file_size), file))) {
                return false;
            }

            TitleContentMapHeader header{};
            size_t read_size = 0;
            if (file_size < static_cast<s64>(sizeof(header))
                || R_FAILED(fs::ReadFile(std::addressof(read_size), file, 0, std::addressof(header), sizeof(header)))
                || read_size != sizeof(header)
                || header.magic != TitleContentMapMagic
                || (header.version != 1 && header.version != TitleContentMapVersion)
                || header.reserved != 0
                || header.count > TitleContentMapMaxEntries) {
                return false;
            }

            const size_t entry_size = header.version == 1 ? sizeof(LegacyTitleContentMapEntry) : sizeof(TitleContentMapEntry);
            const s64 expected_size = static_cast<s64>(sizeof(header)) + static_cast<s64>(header.count) * entry_size;
            if (file_size != expected_size) {
                return false;
            }

            if (header.count > 0) {
                read_size = 0;
                if (header.version == 1) {
                    for (u32 i = 0; i < header.count; ++i) {
                        LegacyTitleContentMapEntry legacy{};
                        if (R_FAILED(fs::ReadFile(std::addressof(read_size), file,
                                sizeof(header) + i * sizeof(legacy), std::addressof(legacy), sizeof(legacy)))
                            || read_size != sizeof(legacy)) {
                            return false;
                        }
                        out_entries[i] = {
                            .program_id = legacy.program_id,
                            .meta_content_id = InvalidContentId,
                            .control_content_id = legacy.control_content_id,
                            .program_content_id = legacy.program_content_id,
                        };
                    }
                } else {
                    const size_t entries_size = static_cast<size_t>(header.count) * sizeof(TitleContentMapEntry);
                    if (R_FAILED(fs::ReadFile(std::addressof(read_size), file, sizeof(header), out_entries, entries_size))
                        || read_size != entries_size) {
                        return false;
                    }
                }
            }

            for (u32 i = 0; i < header.count; ++i) {
                if (out_entries[i].program_id == 0) { return false; }
                for (u32 j = 0; j < i; ++j) {
                    if (out_entries[i].program_id == out_entries[j].program_id) { return false; }
                }
            }
            *out_count = header.count;
            return true;
        }

        Result PrimeProgramIdCache() {
            u32 title_count = 0;
            const bool title_valid = ReadTitleMap(g_title_map_scratch, std::addressof(title_count));

            u32 content_count = 0;
            const bool content_valid = ReadTitleContentMap(g_title_content_map_scratch, std::addressof(content_count));

            /* Publish a complete snapshot only after all filesystem I/O has
             * finished. Allocation is post-mount and therefore cannot change
             * NCM's boot-time KIP memory layout. */
            std::scoped_lock lk(g_program_id_cache_mutex);
            const bool title_allocated = title_count == 0 || EnsureProgramIdTitleCacheAllocated();
            const bool content_allocated = content_count == 0 || EnsureProgramIdContentCacheAllocated();
            if (!title_valid || !content_valid || !title_allocated || !content_allocated) {
                g_program_id_title_cache_count = 0;
                g_program_id_content_cache_count = 0;
                g_program_id_title_cache_valid = false;
                g_program_id_content_cache_valid = false;
                R_RETURN(ncm::ResultContentNotFound());
            }

            if (title_valid) {
                if (title_count > 0) {
                    std::memcpy(g_program_id_title_cache, g_title_map_scratch, static_cast<size_t>(title_count) * sizeof(TitleMapEntry));
                }
                g_program_id_title_cache_count = title_count;
                g_program_id_title_cache_valid = true;
            } else {
                g_program_id_title_cache_count = 0;
                g_program_id_title_cache_valid = false;
            }

            if (content_valid) {
                if (content_count > 0) {
                    std::memcpy(g_program_id_content_cache, g_title_content_map_scratch, static_cast<size_t>(content_count) * sizeof(TitleContentMapEntry));
                }
                g_program_id_content_cache_count = content_count;
                g_program_id_content_cache_valid = true;
            } else {
                g_program_id_content_cache_count = 0;
                g_program_id_content_cache_valid = false;
            }

            R_SUCCEED();
        }

        Result WriteTitleContentMap(const TitleContentMapEntry *entries, u32 count) {
            AMS_ASSERT(entries != nullptr || count == 0);
            R_UNLESS(count <= TitleContentMapMaxEntries, ncm::ResultBufferInsufficient());

            const size_t entries_size = static_cast<size_t>(count) * sizeof(TitleContentMapEntry);
            const TitleContentMapHeader header{TitleContentMapMagic, TitleContentMapVersion, count, 0};
            // A failed publication does not validate new IDs or revoke the
            // prior routing snapshot. Only successful revalidation replaces it.
            R_TRY(PublishMap(TitleContentMapPath, TitleContentMapTempPath, TitleContentMapBackupPath,
                std::addressof(header), sizeof(header), entries, entries_size));
            UpdateProgramIdContentCache(entries, count);
            R_SUCCEED();
        }

    }

    namespace {

        size_t FindContentLocation(ContentId content_id) {
            for (u32 i = 0; i < g_content_location_count; ++i) {
                if (g_content_locations[i] == content_id) {
                    return i;
                }
            }
            return g_content_location_count;
        }

        Result LoadContentLocations() {
            R_SUCCEED_IF(g_content_locations_loaded);
            if(!g_content_locations)g_content_locations=new (std::nothrow) ContentId[ContentLocationsMaxEntries]{};
            if(!g_content_registered_this_boot)g_content_registered_this_boot=new (std::nothrow) bool[ContentLocationsMaxEntries]{};
            R_UNLESS(g_content_locations && g_content_registered_this_boot,fs::ResultAllocationMemoryFailed());
            R_TRY(EnsureTitleMapStorageMounted());
            g_content_location_count = 0;

            fs::FileHandle file;
            Result open_result = fs::OpenFile(std::addressof(file), ContentLocationsPath, fs::OpenMode_Read);
            if (fs::ResultPathNotFound::Includes(open_result)) {
                open_result = fs::OpenFile(std::addressof(file), ContentLocationsBackupPath, fs::OpenMode_Read);
            }
            if (fs::ResultPathNotFound::Includes(open_result)) {
                /* Migrate the older three-ID map without touching the drive.
                 * Subsequent registrations add every NCA, including data. */
                u32 title_count = 0;
                R_UNLESS(ReadTitleContentMap(g_title_content_map_scratch, std::addressof(title_count), true), ncm::ResultInvalidOperation());
                {
                    for (u32 i = 0; i < title_count; ++i) {
                        const auto &entry = g_title_content_map_scratch[i];
                        for (const auto &id : {entry.meta_content_id, entry.control_content_id, entry.program_content_id}) {
                            if (id != InvalidContentId && FindContentLocation(id) == g_content_location_count) {
                                g_content_locations[g_content_location_count++] = id;
                            }
                        }
                    }
                }
                g_content_locations_loaded = true;
                R_SUCCEED();
            }
            R_TRY(open_result);
            ON_SCOPE_EXIT { fs::CloseFile(file); };

            TitleMapHeader header{};
            s64 size = 0;
            size_t read = 0;
            R_TRY(fs::GetFileSize(std::addressof(size), file));
            R_TRY(fs::ReadFile(std::addressof(read), file, 0, std::addressof(header), sizeof(header)));
            R_UNLESS(read == sizeof(header) && header.magic == ContentLocationsMagic
                && header.version == 1 && header.reserved == 0 && header.count <= ContentLocationsMaxEntries
                && size == static_cast<s64>(sizeof(header) + header.count * sizeof(ContentId)), ncm::ResultInvalidOperation());
            if (header.count > 0) {
                const size_t bytes = header.count * sizeof(ContentId);
                R_TRY(fs::ReadFile(std::addressof(read), file, sizeof(header), g_content_locations, bytes));
                R_UNLESS(read == bytes, ncm::ResultInvalidOperation());
                for (u32 i = 0; i < header.count; ++i) {
                    R_UNLESS(g_content_locations[i] != InvalidContentId, ncm::ResultInvalidOperation());
                    for (u32 j = 0; j < i; ++j) {
                        R_UNLESS(g_content_locations[i] != g_content_locations[j], ncm::ResultInvalidOperation());
                    }
                }
            }
            g_content_location_count = header.count;
            g_content_locations_loaded = true;
            R_SUCCEED();
        }

        Result SaveContentLocations() {
            const size_t bytes = g_content_location_count * sizeof(ContentId);
            const TitleMapHeader header{ContentLocationsMagic, 1, g_content_location_count, 0};
            R_RETURN(PublishMap(ContentLocationsPath, ContentLocationsTempPath, ContentLocationsBackupPath,
                std::addressof(header), sizeof(header), g_content_locations, bytes));
        }

        Result ReloadContentLocationsForMutation() {
            R_TRY(CheckNoPendingRepair());
            // Recovery can replace the SD indexes while NCM remains alive.
            // Never overwrite such a newer snapshot with our boot-time cache.
            const u32 old_count = g_content_locations_loaded ? g_content_location_count : 0;
            std::unique_ptr<ContentId[]> fresh_ids(new (std::nothrow) ContentId[old_count]);
            R_UNLESS(old_count == 0 || fresh_ids != nullptr, fs::ResultAllocationMemoryFailed());
            u32 fresh_count = 0;
            for (u32 i = 0; i < old_count; ++i) {
                if (g_content_registered_this_boot[i]) { fresh_ids[fresh_count++] = g_content_locations[i]; }
            }
            g_content_locations_loaded = false;
            if(g_content_registered_this_boot)std::memset(g_content_registered_this_boot, 0, ContentLocationsMaxEntries*sizeof(bool));
            R_TRY(LoadContentLocations());
            for (u32 i = 0; i < fresh_count; ++i) {
                const size_t index = FindContentLocation(fresh_ids[i]);
                if (index < g_content_location_count) { g_content_registered_this_boot[index] = true; }
            }
            R_SUCCEED();
        }

    }

    Result EnsureUsbContentLocationsLoaded() {
        std::scoped_lock lk(g_title_map_mutex);
        R_RETURN(LoadContentLocations());
    }

    Result RefreshUsbProgramIdCache(u64 media_generation) {
        std::scoped_lock lk(g_title_map_mutex);
        {
            std::scoped_lock cache_lk(g_program_id_cache_mutex);
            R_SUCCEED_IF(g_program_id_title_cache_valid && g_program_id_content_cache_valid
                && g_program_id_cache_media_generation == media_generation);
        }

        R_TRY(PrimeProgramIdCache());
        {
            std::scoped_lock cache_lk(g_program_id_cache_mutex);
            g_program_id_cache_media_generation = media_generation;
        }
        R_SUCCEED();
    }

    Result RegisterUsbContentLocation(ContentId content_id) {
        R_UNLESS(content_id != InvalidContentId, ncm::ResultInvalidOperation());
        std::scoped_lock lk(g_title_map_mutex);
        R_TRY(ReloadContentLocationsForMutation());
        size_t index = FindContentLocation(content_id);
        const bool added = index == g_content_location_count;
        if (added) {
            R_UNLESS(g_content_location_count < ContentLocationsMaxEntries, ncm::ResultBufferInsufficient());
            g_content_locations[g_content_location_count++] = content_id;
        }
        // A migration/backup-only state still needs an actual primary file.
        fs::FileHandle primary;
        const Result primary_result = fs::OpenFile(std::addressof(primary), ContentLocationsPath, fs::OpenMode_Read);
        if (R_SUCCEEDED(primary_result)) { fs::CloseFile(primary); }
        Result result = ResultSuccess();
        if (added || R_FAILED(primary_result)) {
            result = SaveContentLocations();
        }
        if (R_FAILED(result)) {
            // Publication may have succeeded but readback failed. Reload the
            // actual durable state next time; do not cache a guessed rollback.
            g_content_locations_loaded = false;
            if(g_content_registered_this_boot)std::memset(g_content_registered_this_boot, 0, ContentLocationsMaxEntries*sizeof(bool));
            R_RETURN(result);
        }
        g_content_registered_this_boot[index] = true;
        R_SUCCEED();
    }

    Result UnregisterUsbContentLocation(ContentId content_id) {
        std::scoped_lock lk(g_title_map_mutex);
        R_TRY(ReloadContentLocationsForMutation());
        const size_t index = FindContentLocation(content_id);
        R_SUCCEED_IF(index == g_content_location_count);
        const bool was_new = g_content_registered_this_boot[index];
        const size_t last = --g_content_location_count;
        g_content_locations[index] = g_content_locations[last];
        g_content_registered_this_boot[index] = g_content_registered_this_boot[last];
        const Result result = SaveContentLocations();
        if (R_FAILED(result)) {
            g_content_locations[last] = g_content_locations[index];
            g_content_registered_this_boot[last] = g_content_registered_this_boot[index];
            g_content_locations[index] = content_id;
            g_content_registered_this_boot[index] = was_new;
            ++g_content_location_count;
            g_content_locations_loaded = false;
            if(g_content_registered_this_boot)std::memset(g_content_registered_this_boot, 0, ContentLocationsMaxEntries*sizeof(bool));
        }
        R_RETURN(result);
    }

    bool IsUsbContentLocationRegistered(ContentId content_id) {
        std::scoped_lock lk(g_title_map_mutex);
        return R_SUCCEEDED(LoadContentLocations()) && FindContentLocation(content_id) != g_content_location_count;
    }

    bool WasUsbContentRegisteredThisBoot(ContentId content_id) {
        std::scoped_lock lk(g_title_map_mutex);
        if (R_FAILED(LoadContentLocations())) {
            return false;
        }
        const size_t index = FindContentLocation(content_id);
        return index != g_content_location_count && g_content_registered_this_boot[index];
    }

    Result ListUsbContentLocations(ContentId *out, size_t capacity, size_t offset, size_t *out_count) {
        R_UNLESS(out_count != nullptr && (out != nullptr || capacity == 0), ncm::ResultInvalidOperation());
        std::scoped_lock lk(g_title_map_mutex);
        R_TRY(LoadContentLocations());
        *out_count = 0;
        for (size_t i = offset; i < g_content_location_count && *out_count < capacity; ++i) {
            out[(*out_count)++] = g_content_locations[i];
        }
        R_SUCCEED();
    }

    bool IsUsbTitleMapped(ProgramId program_id) {
        std::scoped_lock lk(g_title_map_mutex);

        auto &entries = g_title_map_scratch;
        u32 count = 0;
        if (!ReadTitleMap(entries, std::addressof(count))) {
            return false;
        }

        for (u32 i = 0; i < count; i++) {
            if (entries[i].program_id == program_id.value) {
                return true;
            }
        }

        return false;
    }

    Result ResolveUsbTitleAlias(ProgramId *out_program_id, ProgramId alias_program_id) {
        R_UNLESS(out_program_id != nullptr && alias_program_id.value != 0, ncm::ResultInvalidOperation());
        *out_program_id = {};
        /* Only an opaque, non-system ID may be looked up as an alias. The
         * second u64 is written only after a validated USB Control NCA has
         * identified the canonical application, so this is an exact durable
         * association rather than the old single-title fallback. */
        R_UNLESS(!IsApplicationId(alias_program_id) && !IsSystemProgramId(alias_program_id), ncm::ResultContentNotFound());

        std::scoped_lock lk(g_title_map_mutex);
        auto &entries = g_title_map_scratch;
        u32 count = 0;
        R_UNLESS(ReadTitleMap(entries, std::addressof(count)), fs::ResultPathNotFound());

        for (u32 i = 0; i < count; ++i) {
            const auto &entry = entries[i];
            const ProgramId program_id{entry.program_id};
            if (entry.alias_program_id == alias_program_id.value
                && IsApplicationId(program_id)
                && !IsSystemProgramId(program_id)
                && program_id != alias_program_id) {
                *out_program_id = program_id;
                R_SUCCEED();
            }
        }

        R_RETURN(ncm::ResultContentNotFound());
    }

    Result ListUsbTitles(ProgramId *out_program_ids, size_t max_program_ids, u32 *out_count) {
        R_UNLESS(out_count != nullptr, fs::ResultInvalidArgument());
        R_UNLESS(out_program_ids != nullptr || max_program_ids == 0, fs::ResultInvalidArgument());

        std::scoped_lock lk(g_title_map_mutex);

        auto &entries = g_title_map_scratch;
        u32 count = 0;
        R_UNLESS(ReadTitleMap(entries, std::addressof(count)), fs::ResultPathNotFound());

        const u32 written = std::min<u32>(count, static_cast<u32>(std::min<size_t>(max_program_ids, TitleMapMaxEntries)));
        for (u32 i = 0; i < written; i++) {
            out_program_ids[i] = ProgramId{entries[i].program_id};
        }
        *out_count = written;
        R_SUCCEED();
    }

    Result RegisterUsbTitle(ProgramId program_id) {
        R_UNLESS(program_id.value != 0, fs::ResultInvalidArgument());

        std::scoped_lock lk(g_title_map_mutex);
        R_TRY(CheckNoPendingRepair());

        auto &entries = g_title_map_scratch;
        u32 count = 0;
        R_UNLESS(ReadTitleMap(entries, std::addressof(count), true), ncm::ResultInvalidOperation());

        for (u32 i = 0; i < count; i++) {
            if (entries[i].program_id == program_id.value) {
                // An idempotent retry or external repair can leave disk newer
                // than RAM. Publish the already validated snapshot as well.
                UpdateProgramIdTitleCache(entries, count);
                R_SUCCEED();
            }
        }

        R_UNLESS(count < TitleMapMaxEntries, ncm::ResultBufferInsufficient());
        entries[count++] = { .program_id = program_id.value, .alias_program_id = 0 };
        R_RETURN(WriteTitleMap(entries, count));
    }

    Result RegisterUsbTitleContent(ProgramId program_id, const ContentId &meta_content_id, const ContentId &control_content_id, const ContentId &program_content_id) {
        R_UNLESS(program_id.value != 0, fs::ResultInvalidArgument());
        R_UNLESS(meta_content_id != InvalidContentId && control_content_id != InvalidContentId && program_content_id != InvalidContentId, fs::ResultInvalidArgument());

        std::scoped_lock lk(g_title_map_mutex);
        R_TRY(CheckNoPendingRepair());

        auto &entries = g_title_content_map_scratch;
        u32 count = 0;
        R_UNLESS(ReadTitleContentMap(entries, std::addressof(count), true), ncm::ResultInvalidOperation());

        for (u32 i = 0; i < count; i++) {
            if (entries[i].program_id == program_id.value) {
                if (entries[i].meta_content_id == meta_content_id
                    && entries[i].control_content_id == control_content_id
                    && entries[i].program_content_id == program_content_id) {
                    UpdateProgramIdContentCache(entries, count);
                    R_SUCCEED();
                }

                entries[i].meta_content_id = meta_content_id;
                entries[i].control_content_id = control_content_id;
                entries[i].program_content_id = program_content_id;
                R_RETURN(WriteTitleContentMap(entries, count));
            }
        }

        R_UNLESS(count < TitleContentMapMaxEntries, ncm::ResultBufferInsufficient());
        entries[count++] = {
            .program_id = program_id.value,
            .meta_content_id = meta_content_id,
            .control_content_id = control_content_id,
            .program_content_id = program_content_id,
        };
        R_RETURN(WriteTitleContentMap(entries, count));
    }

    Result GetUsbTitleContentIds(ProgramId program_id, ContentId *out_meta_content_id, ContentId *out_control_content_id, ContentId *out_program_content_id) {
        R_UNLESS(program_id.value != 0 && out_meta_content_id != nullptr && out_control_content_id != nullptr && out_program_content_id != nullptr, fs::ResultInvalidArgument());
        *out_meta_content_id = InvalidContentId;
        *out_control_content_id = InvalidContentId;
        *out_program_content_id = InvalidContentId;

        std::scoped_lock lk(g_title_map_mutex);

        auto &entries = g_title_content_map_scratch;
        u32 count = 0;
        R_UNLESS(ReadTitleContentMap(entries, std::addressof(count)), ncm::ResultContentNotFound());

        for (u32 i = 0; i < count; i++) {
            if (entries[i].program_id == program_id.value) {
                *out_meta_content_id = entries[i].meta_content_id;
                *out_control_content_id = entries[i].control_content_id;
                *out_program_content_id = entries[i].program_content_id;
                R_SUCCEED();
            }
        }

        R_RETURN(ncm::ResultContentNotFound());
    }

    Result ResolveUsbContentProgramId(ProgramId *out_program_id, const ContentId &content_id) {
        R_UNLESS(out_program_id != nullptr && content_id != InvalidContentId, fs::ResultInvalidArgument());
        *out_program_id = {};

        std::scoped_lock lk(g_program_id_cache_mutex);
        R_UNLESS(g_program_id_title_cache_valid && g_program_id_content_cache_valid, ncm::ResultContentNotFound());

        ProgramId resolved_program_id{};
        bool found = false;
        for (u32 i = 0; i < g_program_id_content_cache_count; ++i) {
            const auto &content_entry = g_program_id_content_cache[i];
            if (content_entry.meta_content_id != content_id
                && content_entry.control_content_id != content_id
                && content_entry.program_content_id != content_id) {
                continue;
            }

            const TitleMapEntry *title_entry = nullptr;
            for (u32 j = 0; j < g_program_id_title_cache_count; ++j) {
                if (g_program_id_title_cache[j].program_id == content_entry.program_id) {
                    title_entry = g_program_id_title_cache + j;
                    break;
                }
            }
            if (title_entry == nullptr) {
                continue;
            }

            /* A validated Control NCA may have persisted its opaque header
             * ProgramId as an alias. Otherwise the canonical application id
             * is also the NCA ProgramId for an ordinary installed title. */
            const ProgramId candidate{
                title_entry->alias_program_id != 0 ? title_entry->alias_program_id : title_entry->program_id
            };
            R_UNLESS(candidate.value != 0, ncm::ResultContentNotFound());
            R_UNLESS(!found || candidate == resolved_program_id, ncm::ResultContentNotFound());
            resolved_program_id = candidate;
            found = true;
        }

        R_UNLESS(found, ncm::ResultContentNotFound());
        *out_program_id = resolved_program_id;
        R_SUCCEED();
    }

    Result UnregisterUsbTitle(ProgramId program_id) {
        R_UNLESS(program_id.value != 0, fs::ResultInvalidArgument());

        std::scoped_lock lk(g_title_map_mutex);
        R_TRY(CheckNoPendingRepair());

        {
            auto &entries = g_title_map_scratch;
            u32 count = 0;
            R_UNLESS(ReadTitleMap(entries, std::addressof(count), true), ncm::ResultInvalidOperation());
            {
                u32 write_count = 0;
                bool changed = false;
                for (u32 i = 0; i < count; i++) {
                    auto entry = entries[i];
                    if (entry.program_id == program_id.value) {
                        /* This title is now stock-backed; remove its USB row. */
                        changed = true;
                        continue;
                    }
                    if (entry.alias_program_id == program_id.value) {
                        /* A real installed SD title owns this opaque ID now.
                         * Clear only the stale alias while retaining the
                         * canonical USB application's own mapping. */
                        entry.alias_program_id = 0;
                        changed = true;
                    }
                    entries[write_count++] = entry;
                }
                if (changed) {
                    R_TRY(WriteTitleMap(entries, write_count));
                } else {
                    UpdateProgramIdTitleCache(entries, count);
                }
            }
        }

        {
            auto &entries = g_title_content_map_scratch;
            u32 count = 0;
            R_UNLESS(ReadTitleContentMap(entries, std::addressof(count), true), ncm::ResultInvalidOperation());
            {
                u32 write_count = 0;
                for (u32 i = 0; i < count; i++) {
                    if (entries[i].program_id != program_id.value) {
                        entries[write_count++] = entries[i];
                    }
                }
                if (write_count != count) {
                    R_TRY(WriteTitleContentMap(entries, write_count));
                } else {
                    UpdateProgramIdContentCache(entries, count);
                }
            }
        }

        R_SUCCEED();
    }

}
