#include "sd_io_hooks.hpp"
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
#include <stratosphere.hpp>
#include "../../../../libraries/libstratosphere/source/ncm/ncm_usb_limits.hpp"
#include "../../../../libraries/libstratosphere/source/fssystem/fssystem_patch_witness.hpp"
#include <stratosphere/usb/usb_runtime_diagnostics.hpp>
#include <stratosphere/usb/usb_io_diagnostics.hpp>
#include <stratosphere/usb/usb_pause_diagnostics.hpp>
#include <stratosphere/usb/usb_completion_diagnostics.hpp>
#include <stratosphere/usb/usb_write_cache_diagnostics.hpp>
#include <stratosphere/usb/usb_reader_diagnostics.hpp>
#include <stratosphere/usb/usb_device_map.hpp>
#include <stratosphere/fs/fsa/fs_registrar.hpp>
#include <stratosphere/fssrv/fssrv_interface_adapters.hpp>
#include "../amsmitm_initialization.hpp"
#include "../amsmitm_fs_utils.hpp"
#include "usb_slot_preferences_sd.hpp"
#include "usb_compatibility_sd.hpp"
#include "usb_settings_cache.hpp"
#include <stratosphere/usb/usb_interface_recovery.hpp>
#include <stratosphere/usb/usb_probe_trace.hpp>
#include <stratosphere/usb/usb_reader_device_reset.hpp>
#include <stratosphere/usb/usb_reader_direct_start.hpp>
#include <stratosphere/usb/usb_startup_history.hpp>
#include <stratosphere/usb/usb_startup_sense_csw.hpp>
#include <stratosphere/usb/usb_reader_fixed_sense.hpp>
#include <stratosphere/usb/usb_reader_startup_profile.hpp>
#include <stratosphere/usb/usb_reader_endpoint_profile.hpp>
#include "usb_homebrew_service.hpp"
#include "usb_virtual_sd.hpp"
#include "usb_startup_timing.hpp"
#include "usb_launch_trace.hpp"
#include "usbmitm_module.hpp"
#include "usb_system_power.hpp"
#include "usb_game_wake_trace.hpp"
#include "usb_wake_compare.hpp"
#include "usb_romfs_probe.hpp"
#include "usb_control_identity.hpp"
#include "usb_applet_forward_progress.hpp"
#include "usb_fs_forward_progress.hpp"
#include "usb_monitor_progress.hpp"
#include "usb_memory_snapshot.hpp"
#include "usb_trace_file_system.hpp"
#include "usb_hang_capture.hpp"
#include "usb_content_route_stats.hpp"
#include "usb_external_key_stats.hpp"
#include "usb_romfs_mods.hpp"
#include "usb_compatibility_policy.hpp"
#include "usb_verification_path.hpp"
#include "usb_verification_addon.hpp"
#include <stratosphere/usb/usb_launch_content.hpp>
#include <stratosphere/usb/usb_eject_protocol.hpp>

namespace ams::mitm::usb_storage {

    /* Bound to the process by Loader. Copies of this pin keep exactly the
     * original content IDs and media generation, even after process exit. */
    struct UsbLaunchSnapshot {
        u64 generation;
        usb::content::LaunchContent content;
    };

    namespace {

        constexpr const char *SettingName = "enable_usb_storage";
        constexpr const char *ContentSettingName = "enable_usb_content";
        constexpr const char *ContentNativePathSettingName = "enable_usb_content_native_path";
        constexpr const char *ContentForcePathSettingName = "enable_usb_content_force_path";
        constexpr const char *ContentMetadataSettingName = "enable_usb_content_force_metadata";
        constexpr const char *StatusPath = "/logs/usb_storage_status.txt";
        constexpr const char *StatusTempPath = "/logs/usb_storage_status.txt.tmp";
        constexpr const char *LastReadyStatusPath = "/logs/usb_storage_last_ready.txt";
        constexpr const char *LastReadyStatusTempPath = "/logs/usb_storage_last_ready.txt.tmp";
        constexpr const char *LoaderTracePath = "/logs/usb_loader_trace.txt";
        constexpr const char *NcmLaunchTracePath = "/logs/usb_ncm_launch_trace.txt";
        constexpr const char *NcmLaunchTraceTempPath = "/logs/usb_ncm_launch_trace.txt.tmp";
        constexpr const char *ContentSettingBridgePath = "/logs/usb_content_enabled.txt";
        constexpr const char *ContentRouteCommandPath = "/logs/usb_content_route.txt";
        constexpr const char *ContentStatusRefreshCommandPath = "/logs/usb_content_refresh.txt";
        constexpr const char *TitleMapDirectory = "/usb_content";
        constexpr const char *TitleMapPath = "/usb_content/title_map.bin";
        constexpr const char *TitleMapTempPath = "/usb_content/title_map.bin.tmp";
        constexpr const char *UltrahandNotificationDirectory = "/config/ultrahand/notifications";
        /* R83 also accepts NS patch status from a late generic Loader request
         * when the failing title aborts before its mapped resolver executes. */
    constexpr const char *BuildId = "NEXUS-1.12.2-N1-sd-register";
        int FormatUsbMetadataOptimizationStatus(char *out, size_t capacity) {
            return util::SNPrintf(out, capacity,
                "io_metadata_cache=SD9\nio_metadata_cache_slots=64\n"
                "io_metadata_cache_hits=%llu\nio_metadata_cache_misses=%llu\n"
                "io_metadata_cache_invalidations=%llu\nio_metadata_stat_calls=%llu\n"
                "io_virtual_sd_append_writes=%llu\n",
                static_cast<unsigned long long>(usb::io_diagnostics::metadata_cache_hits.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(usb::io_diagnostics::metadata_cache_misses.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(usb::io_diagnostics::metadata_cache_invalidations.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(usb::io_diagnostics::metadata_stat_calls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(usb::io_diagnostics::virtual_sd_append_writes.load(std::memory_order_relaxed)));
        }
        impl::UsbSettingsCache g_usb_settings_cache;
        std::atomic_bool g_allow_unsigned_usb_application{false};
        std::atomic<u64> g_unsigned_usb_headers_accepted{0};

        constexpr bool EnableContentStatusMonitor = true;
        constexpr TimeSpan ContentMonitorBootDelay = TimeSpan::FromSeconds(10);
        constexpr size_t StatusBufferSize = 0xC000;
        constexpr size_t LoaderTraceBufferSize = 0x20000;
        constexpr size_t NcmLaunchTraceBufferSize = 0x8000;
        constexpr size_t MaxLoaderTraceRecords = 384;
        /* R107 preserves R105's complete bounded launch sequence and records
         * the stock identity seed used for an untouched Control-NCA handoff. */
        constexpr size_t MaxPublishedLoaderTraceRecords = MaxLoaderTraceRecords;
        constexpr size_t NotificationBufferSize = 0x800;
        [[maybe_unused]] constexpr size_t ImageDirectoryBatchSize = 16;
        [[maybe_unused]] constexpr size_t MaxImageDirectoryEntries = 256;
        constexpr size_t MaxIndexedImages = 2;
        [[maybe_unused]] constexpr size_t HomebrewDirectoryBatchSize = 16;
        [[maybe_unused]] constexpr size_t MaxHomebrewDirectoryEntries = 256;
        [[maybe_unused]] constexpr size_t MaxHomebrewDirectories = 64;
        constexpr size_t MaxIndexedHomebrew = 2;
        constexpr size_t MaxHomebrewReadSize = 1_MB;
        constexpr size_t MaxContentTransferSize = 4_MB;
        /* Includes returned content-filesystem/file/directory sessions. */
        constexpr size_t HomebrewServiceMaxSessions = 32;
        u64 g_last_ready_content_changes{};
        constexpr const char *UsbContentMountPrefix = "usbcontent:";
        constexpr size_t MaxCachedProgramPaths = 16;
        constexpr size_t MaxCachedMappedTitles = 32;
        constexpr size_t MaxCachedControlPaths = 16;
        constexpr size_t MaxCachedProgramAliases = 16;
        constexpr size_t ControlScanBatchSize = 8;
        constexpr size_t MaxControlScanDirectories = 256;
        constexpr size_t MaxControlScanFiles = 2048;
        constexpr u32 TitleMapMagic = 0x54425355; /* "USBT" */
        constexpr u32 TitleMapVersion = 1;
        constexpr u32 TitleMapMaxEntries = ncm::usb_limits::Titles;

        struct TitleMapHeader {
            u32 magic;
            u32 version;
            u32 count;
            u32 reserved;
        };

        struct TitleMapEntry {
            u64 program_id;
            /* Version 1 reserved this field. v130 stores the exact opaque
             * launch id here without changing the on-disk entry size. */
            u64 alias_program_id;
        };

        static_assert(sizeof(TitleMapHeader) == 0x10);
        static_assert(sizeof(TitleMapEntry) == 0x10);

        [[maybe_unused]] char ToLowerAscii(char c) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            return c;
        }

        [[maybe_unused]] bool HasImageExtension(const char *name, const char *extension) {
            const size_t name_length = std::strlen(name);
            const size_t extension_length = std::strlen(extension);
            if (name_length < extension_length) {
                return false;
            }

            const char *suffix = name + name_length - extension_length;
            for (size_t i = 0; i < extension_length; ++i) {
                if (ToLowerAscii(suffix[i]) != extension[i]) {
                    return false;
                }
            }
            return true;
        }

        [[maybe_unused]] bool IsImageCandidate(const char *name) {
            return HasImageExtension(name, ".xci") ||
                   HasImageExtension(name, ".nsp") ||
                   HasImageExtension(name, ".nsz");
        }

        [[maybe_unused]] bool IsHomebrewCandidate(const char *name) {
            return HasImageExtension(name, ".nro");
        }

        ams::usb::storage::Service g_storage_service;
        ams::usb::filesystem::Service g_filesystem_service;
        fssrv::impl::ExternalKeyManager g_external_key_manager;
        bool g_filesystem_registered{false};
        bool g_content_route_auto_enabled{false};
        std::atomic_bool g_content_route_applied_enabled{false};
        std::atomic_bool g_content_route_applied_once{false};
        std::atomic_bool g_content_launch_path_enabled{false};
        std::atomic_bool g_content_install_target_enabled{false};
        /* Passive shared-client diagnostics. No logging I/O inside IPC handlers. */
        std::atomic<u64> g_shared_status_queries{}, g_shared_ready_replies{};
        std::atomic<u64> g_shared_info_queries{}, g_shared_directory_queries{};
        std::atomic<u32> g_shared_status_result{}, g_shared_status_flags{};
        u32 g_last_module_result{ResultSuccess().GetValue()};
        os::SdkMutex g_start_mutex{};
        os::SdkMutex g_status_mutex{};
        bool g_last_ready_snapshot_valid{false};
        u64 g_last_ready_snapshot_generation{};
        u64 g_last_ready_snapshot_free_space{};
        u64 g_last_ready_snapshot_total_space{};
        u32 g_last_ready_snapshot_route_requests{};
        u32 g_last_ready_snapshot_route_successes{};
        u32 g_last_ready_snapshot_route_fallbacks{};
        u32 g_last_ready_snapshot_runtime_code_attempts{};
        u32 g_last_ready_snapshot_runtime_code_successes{};
        u32 g_last_ready_snapshot_runtime_code_last_result{ResultSuccess().GetValue()};
        u64 g_last_ready_snapshot_unsigned_headers_accepted{};
        u64 g_last_ready_snapshot_read_bytes{}, g_last_ready_snapshot_written_bytes{};

        struct CachedProgramPath {
            bool valid;
            ncm::ProgramId program_id;
            char path[ams::fs::EntryNameLengthMax + 1];
        };

        CachedProgramPath g_cached_program_paths[MaxCachedProgramPaths]{};
        size_t g_next_cached_program_path{};
        std::atomic<u64> g_last_cached_program_id{};
        os::SdkMutex g_program_path_mutex{};

        /* Launch snapshots are separate from periodically refreshed base-map
         * hints. A monitor refresh must never replace a running patch with base. */
        struct Comparison {
            LaunchPin pin;
            u32 epoch{};
            s64 offset{};
            size_t size{};
            u32 original{wake_compare::NotAttempted};
            wake_compare::Stage stage{};
            wake_compare::Results fresh{};
        };
        os::SdkMutex g_comparison_mutex;
        Comparison g_comparison;
        std::atomic<u32> g_comparison_revision{};

        namespace pw = fssystem::patch_witness;
        struct PatchWitness {
            LaunchPin pin;
            pw::Sample baseline{};
            u32 cold_epoch{}, sample_epoch{}, attempted_epoch{}, open_result{UINT32_MAX};
            s32 section{}, same_extent{-1}, same_expected{-1}, same_cipher{-1}, same_plain{-1}, verified{-1};
        };
        PatchWitness g_patch_witness;
        const void *g_observed_reader{};
        os::ThreadType *g_observer_thread{};
        pw::Sample *g_active_sample{};

        pw::Sample *FindPatchWitness(const void *reader) {
            std::scoped_lock lock(g_comparison_mutex);
            return reader == g_observed_reader && os::GetCurrentThread() == g_observer_thread ? g_active_sample : nullptr;
        }
        bool BeginPatchWitness(const LaunchPin &pin, const void *reader, pw::Sample &sample) {
            if (!pin) return false;
            std::scoped_lock lock(g_comparison_mutex);
            if (g_active_sample) return false;
            g_observed_reader = reader;
            g_observer_thread = os::GetCurrentThread();
            g_active_sample = &sample;
            pw::lookup.store(FindPatchWitness, std::memory_order_release);
            return true;
        }
        void FinishPatchWitness(const LaunchPin &pin, s32 section, u32 epoch, const pw::Sample &sample, Result rc, bool stable) {
            std::scoped_lock lock(g_comparison_mutex);
            g_active_sample = nullptr;
            g_observed_reader = nullptr;
            g_observer_thread = nullptr;
            if (!stable || sample.offset < 0 || epoch != game_wake::wake_epoch.load(std::memory_order_acquire)) return;
            auto &w = g_patch_witness;
            if (w.pin != pin) {
                if (!sample.Verified() || !sample.cipher_seen || R_FAILED(rc)) return;
                w = {};
                w.pin = pin;
                w.baseline = sample;
                w.section = section;
                w.cold_epoch = epoch;
            } else if (epoch > w.cold_epoch && epoch == wake_compare::ready_epoch.load(std::memory_order_acquire)) {
                const auto &cold = w.baseline;
                w.sample_epoch = epoch;
                w.same_extent = w.section == section && cold.offset == sample.offset && cold.size == sample.size;
                w.same_expected = w.same_extent ? static_cast<int>(cold.expected == sample.expected) : -1;
                w.same_cipher = w.same_expected == 1 && sample.cipher_seen ? static_cast<int>(cold.cipher == sample.cipher) : -1;
                w.same_plain = w.same_expected == 1 && sample.plain_seen ? static_cast<int>(cold.plain == sample.plain) : -1;
                w.verified = sample.plain_seen ? static_cast<int>(sample.Verified()) : -1;
                w.open_result = rc.GetValue();
            } else {
                return;
            }
            g_comparison_revision.fetch_add(1, std::memory_order_release);
        }
        os::SdkMutex g_launch_snapshot_mutex;
        std::shared_ptr<const UsbLaunchSnapshot> g_launch_snapshots[MaxCachedProgramPaths];
        size_t g_next_launch_snapshot{};

        std::shared_ptr<const UsbLaunchSnapshot> FindLaunchSnapshot(ncm::ProgramId id) {
            std::scoped_lock lock(g_launch_snapshot_mutex);
            for (const auto &entry : g_launch_snapshots) {
                if (entry && entry->content.program_id == id.value) return entry;
            }
            return nullptr;
        }

        void RememberLaunchSnapshot(std::shared_ptr<const UsbLaunchSnapshot> snapshot) {
            std::scoped_lock lock(g_launch_snapshot_mutex);
            for (auto &entry : g_launch_snapshots) {
                if (entry && entry->content.program_id == snapshot->content.program_id) {
                    entry = std::move(snapshot);
                    return;
                }
            }
            g_launch_snapshots[g_next_launch_snapshot] = std::move(snapshot);
            g_next_launch_snapshot = (g_next_launch_snapshot + 1) % MaxCachedProgramPaths;
        }

        /* Discovery hints and GetProgramInfo snapshots never authorize
         * runtime reads. Only Loader's actual process-creation commit does.
         * Retain a shared immutable snapshot per PID, including its generation,
         * patch and add-ons. No live entry is evicted by a later lookup. */
        struct UsbProcessContent {
            u64 pin_id{};
            u64 process_id{};
            std::shared_ptr<const UsbLaunchSnapshot> snapshot;
            std::shared_ptr<usb::storage::EjectLease> eject_lease;
        };
        os::SdkMutex g_process_content_mutex;
        UsbProcessContent g_process_contents[0x40]{};

        std::shared_ptr<const UsbLaunchSnapshot> FindProcessContent(u64 process_id, ncm::ProgramId program_id) {
            std::scoped_lock lock(g_process_content_mutex);
            if (process_id == 0) return nullptr;
            for (const auto &entry : g_process_contents) {
                if (entry.process_id == process_id && entry.snapshot
                    && entry.snapshot->content.program_id == program_id.value) return entry.snapshot;
            }
            return nullptr;
        }

        Result SetProcessContentBinding(const usb::content::ProcessContentBinding &binding) {
            R_UNLESS(binding.pin_id != 0 && binding.program_id != 0, usb::ResultInvalidParameter());
            if (binding.process_id == 0) {
                std::scoped_lock lock(g_process_content_mutex);
                for (auto &entry : g_process_contents) {
                    if (entry.pin_id == binding.pin_id && entry.snapshot
                        && entry.snapshot->content.program_id == binding.program_id) entry = {};
                }
                R_SUCCEED();
            }
            const auto snapshot = FindLaunchSnapshot(ncm::ProgramId{binding.program_id});
            R_UNLESS(snapshot != nullptr, ams::fs::ResultPathNotFound());
            std::scoped_lock lock(g_process_content_mutex);
            for (const auto &entry : g_process_contents) {
                if (entry.pin_id == binding.pin_id || entry.process_id == binding.process_id) {
                    R_UNLESS(entry.pin_id == binding.pin_id && entry.process_id == binding.process_id
                        && entry.snapshot == snapshot, usb::ResultInvalidParameter());
                    R_SUCCEED();
                }
            }
            for (auto &entry : g_process_contents) {
                if (entry.pin_id == 0) {
                    auto lease=std::make_shared<usb::storage::EjectLease>();
                    R_UNLESS(lease,ams::fs::ResultAllocationMemoryFailed());
                    R_TRY(lease->Acquire(&g_storage_service,snapshot->generation));
                    entry = {binding.pin_id, binding.process_id, snapshot, std::move(lease)};
                    R_SUCCEED();
                }
            }
            R_THROW(ams::fs::ResultAllocationMemoryFailed());
        }

        struct CachedMappedTitle {
            ncm::ProgramId program_id;
            ncm::ContentId expected_meta_content_id;
            ncm::ContentId expected_control_content_id;
            ncm::ContentId expected_program_content_id;
            bool has_expected_meta_content_id;
            bool has_expected_control_content_id;
            bool has_expected_program_content_id;
        };

        CachedMappedTitle g_cached_mapped_titles[MaxCachedMappedTitles]{};
        size_t g_cached_mapped_title_count{};
        os::SdkMutex g_mapped_title_mutex{};

        struct CachedControlPath {
            bool valid;
            ncm::ProgramId program_id;
            u64 media_generation;
            char path[ams::fs::EntryNameLengthMax + 1];
        };

        CachedControlPath g_cached_control_paths[MaxCachedControlPaths]{};
        size_t g_next_cached_control_path{};
        os::SdkMutex g_control_path_mutex{};

        struct CachedProgramAlias {
            bool valid;
            bool persisted;
            ncm::ProgramId requested_program_id;
            ncm::ProgramId resolved_program_id;
            u64 media_generation;
        };

        CachedProgramAlias g_cached_program_aliases[MaxCachedProgramAliases]{};
        size_t g_next_cached_program_alias{};
        os::SdkMutex g_program_alias_mutex{};
        os::SdkMutex g_persistent_title_map_mutex{};

        struct PendingControlProgram {
            bool valid;
            ncm::ProgramId program_id;
            u64 media_generation;
        };

        /* The last Control NCA successfully opened through the mapped USB
         * path is the only candidate allowed to bind a new opaque launch ID.
         * It is generation-bound and consumed once, so a stale drive or an
         * arbitrary homebrew launch cannot reuse it. */
        PendingControlProgram g_pending_control_program{};
        os::SdkMutex g_pending_control_mutex{};
        std::atomic<u32> g_runtime_code_attempts{};
        std::atomic<u32> g_runtime_code_successes{};
        std::atomic<u32> g_runtime_code_last_result{ResultSuccess().GetValue()};
        std::atomic<u32> g_runtime_romfs_attempts{};
        std::atomic<u32> g_runtime_romfs_successes{};
        std::atomic<u32> g_runtime_romfs_last_result{ResultSuccess().GetValue()};
        std::atomic<u64> g_loader_probe_program_id{};
        std::atomic<u32> g_loader_probe_stage{};
        std::atomic<u32> g_loader_probe_result{ResultSuccess().GetValue()};
        std::atomic<u32> g_loader_probe_location_storage{};
        std::atomic<u32> g_loader_probe_metadata_storage{};

        struct LoaderTraceRecord {
            u64 sequence;
            ams::usb::content::LoaderTraceInfo info;
        };

        LoaderTraceRecord g_loader_trace_records[MaxLoaderTraceRecords]{};
        u64 g_loader_trace_next_sequence{};
        u32 g_loader_trace_count{};
        u64 g_loader_trace_last_published_sequence{};
        impl::AppletForwardProgress g_applet_forward_progress{};
        u64 g_applet_forward_last_published_revision{};
        s64 g_applet_forward_last_published_ms{};
        impl::FsForwardProgress g_fs_forward_progress{};
        u64 g_fs_forward_last_published_revision{};
        impl::MonitorProgress g_monitor_progress{};
        /* These counters are owned only by the trace publisher thread. */
        u64 g_trace_publisher_cycles{};
        u64 g_trace_publisher_failures{};
        u32 g_trace_publisher_last_result{};
        os::SdkMutex g_loader_trace_mutex{};
        char g_loader_trace_buffer[LoaderTraceBufferSize]{};
        LoaderTraceRecord g_ns_pre_pgl_patch_diagnostic_record{};
        bool g_ns_pre_pgl_patch_diagnostic_valid{};
        std::atomic<u64> g_active_usb_launch_program_id{};
        std::atomic<u64> g_active_launch_trace_program_id{};

        struct NcmLaunchTraceState {
            ncm_trace::LookupSnapshot snapshot;
            ncm::ProgramId application_id;
            u64 session;
            u64 generation;
            u32 begin_result;
            u32 end_result;
            u32 launch_result;
            bool valid;
        };

        NcmLaunchTraceState g_ncm_launch_trace{};
        u64 g_ncm_launch_trace_last_published_generation{};
        os::SdkMutex g_ncm_launch_trace_mutex{};
        char g_ncm_launch_trace_buffer[NcmLaunchTraceBufferSize]{};

        bool g_notification_state_initialized{false};
        bool g_notification_last_mounted{false};
        bool g_notification_last_ready{false};
        u64 g_notification_last_generation{0};
        u8 g_notification_last_state{static_cast<u8>(ams::usb::storage::State::Stopped)};
        u32 g_notification_last_result{ResultSuccess().GetValue()};
        u32 g_notification_sequence{0};
        char g_notification_last_partition_name[37]{};
        char g_notification_last_vendor[9]{};
        char g_notification_last_product[17]{};

        bool g_content_notification_state_initialized{false};
        bool g_content_notification_last_ready{false};
        u32 g_content_notification_last_result{ResultSuccess().GetValue()};
        u64 g_content_notification_last_generation{0};
        u8 g_content_notification_last_filesystem{static_cast<u8>(ams::usb::storage::FileSystemType::Unknown)};
        bool g_content_notification_last_writable{false};
        char g_content_notification_last_partition_name[37]{};
        char g_content_notification_last_vendor[9]{};
        char g_content_notification_last_product[17]{};
        u64 g_content_notification_last_free_space{0};
        u64 g_content_notification_last_total_space{0};
        struct ImageRecord {
            u32 result;
            ams::usb::game_image::Format format;
            u8 reserved[3];
            u32 entry_count;
            u64 size;
            char path[ams::fs::EntryNameLengthMax + 1];
        };

        struct ImageScanStatus {
            bool scanned;
            bool truncated;
            u16 reserved;
            u32 result;
            u64 media_generation;
            u32 candidate_count;
            u32 valid_count;
            u32 invalid_count;
            u32 unsupported_count;
            u32 record_count;
            ImageRecord records[MaxIndexedImages];
        };

        ImageScanStatus g_image_scan{};

        struct HomebrewRecord {
            u32 result;
            u32 version;
            u32 flags;
            u32 reserved;
            u64 image_size;
            u64 file_size;
            char path[ams::fs::EntryNameLengthMax + 1];
        };

        struct HomebrewScanStatus {
            bool scanned;
            bool truncated;
            u16 reserved;
            u32 result;
            u64 media_generation;
            u32 candidate_count;
            u32 valid_count;
            u32 invalid_count;
            u32 record_count;
            HomebrewRecord records[MaxIndexedHomebrew];
        };

        HomebrewScanStatus g_homebrew_scan{};

        void AppendLoaderTraceRecord(const ams::usb::content::LoaderTraceInfo &info);

        constexpr bool IsPriorityLoaderTraceStage(u32 stage) {
            using Stage = ams::usb::content::LoaderProbeStage;

            /* Attempts plus stock qlaunch discovery/application-view records
             * are high-volume. Preserve the corresponding USB fallback,
             * Control-domain child-object and applet launch results so a
             * focused hardware trace cannot evict the exact pre-PGL failure. */
            if (stage == 78 || stage == 79 || stage == 82 || stage == 83
                    || stage == 91 || stage == 92 || stage == 95) {
                return false;
            }
            switch (static_cast<Stage>(stage)) {
                case Stage::FsCurrentStorageAttempt:
                case Stage::FsCurrentStorageResult:
                case Stage::FsDataIdStorageAttempt:
                case Stage::FsDataIdStorageResult:
                case Stage::FsControlMountAttempt:
                case Stage::FsControlScanAttempt:
                case Stage::FsStockControlMountResult:
                case Stage::FsStockRightsHeaderResult:
                case Stage::FsStockProgramHeaderResult:
                    return false;
                default:
                    return true;
            }
        }

        struct HomebrewServerOptions {
            static constexpr size_t PointerBufferSize   = 2_KB;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = false;
        };

        enum HomebrewPortIndex {
            HomebrewPortIndex_Service,
            HomebrewPortIndex_Count,
        };

        constexpr sm::ServiceName HomebrewServiceName = sm::ServiceName::Encode("ams:usb");
        sf::hipc::ServerManager<HomebrewPortIndex_Count, HomebrewServerOptions, HomebrewServiceMaxSessions> g_homebrew_server_manager;
        constinit sf::UnmanagedServiceObject<impl::IHomebrewInterface, HomebrewService> g_homebrew_service_object;

        constexpr size_t HomebrewServiceThreadStackSize = 0x8000;
        alignas(os::ThreadStackAlignment) u8 g_homebrew_service_thread_stack[HomebrewServiceThreadStackSize];
        os::ThreadType g_homebrew_service_thread;

        constexpr size_t TracePublisherThreadStackSize = 0x8000;
        alignas(os::ThreadStackAlignment) u8 g_trace_publisher_thread_stack[TracePublisherThreadStackSize];
        os::ThreadType g_trace_publisher_thread;

        void SetMonitorPhase(impl::MonitorProgress::Phase phase) {
            hang_capture::BeatMonitor(static_cast<u32>(phase));
            const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
            std::scoped_lock lk(g_loader_trace_mutex);
            g_monitor_progress.SetPhase(phase, now);
        }

        void RecordMonitorStatusResult(Result result) {
            std::scoped_lock lk(g_loader_trace_mutex);
            g_monitor_progress.StatusReturned(result.GetValue());
        }

        struct ContentSettingIniState {
            bool found;
            bool enabled;
            bool route_found;
            bool route_enabled;
            bool native_path_found;
            bool native_path_enabled;
            bool force_path_found;
            bool force_path_enabled;
            bool metadata_found;
            bool metadata_enabled;
            bool external_kip_found;
            bool external_kip_enabled;
        };

        int ContentSettingIniHandler(void *user, const char *section, const char *key, const char *value) {
            auto *state = reinterpret_cast<ContentSettingIniState *>(user);
            if (std::strcmp(section, "atmosphere") != 0) {
                return 1;
            }

            const bool enabled = std::strcmp(value, "u8!0x1") == 0 ||
                                 std::strcmp(value, "u8!1") == 0 ||
                                 std::strcmp(value, "1") == 0;
            if (std::strcmp(key, ContentSettingName) == 0) {
                state->found = true;
                state->enabled = enabled;
            } else if (std::strcmp(key, "enable_usb_content_route") == 0) {
                state->route_found = true;
                state->route_enabled = enabled;
            } else if (std::strcmp(key, ContentNativePathSettingName) == 0) {
                state->native_path_found = true;
                state->native_path_enabled = enabled;
            } else if (std::strcmp(key, ContentForcePathSettingName) == 0) {
                state->force_path_found = true;
                state->force_path_enabled = enabled;
            } else if (std::strcmp(key, ContentMetadataSettingName) == 0) {
                state->metadata_found = true;
                state->metadata_enabled = enabled;
            } else if (std::strcmp(key, "enable_usb_external_kip") == 0) {
                state->external_kip_found = true;
                state->external_kip_enabled = enabled;
            }
            return 1;
        }

        [[maybe_unused]] bool TryGetContentEnabledFromSystemSettings(bool *out_enabled) {
            std::unique_ptr<ams::fs::fsa::IFile> file;
            {
                FsFile f;
                if (R_FAILED(ams::mitm::fs::OpenAtmosphereSdFile(std::addressof(f), "/config/system_settings.ini", ams::fs::OpenMode_Read))) {
                    return false;
                }
                file = std::make_unique<ams::fs::RemoteFile>(f);
            }

            ContentSettingIniState state{};
            util::ini::ParseFile(file.get(), std::addressof(state), ContentSettingIniHandler);
            if (!state.found) {
                return false;
            }

            *out_enabled = state.enabled;
            return true;
        }

        bool TryGetContentRouteEnabledFromSystemSettings(bool *out_enabled) {
            if (out_enabled == nullptr) {
                return false;
            }

            std::unique_ptr<ams::fs::fsa::IFile> file;
            {
                FsFile f;
                if (R_FAILED(ams::mitm::fs::OpenAtmosphereSdFile(std::addressof(f), "/config/system_settings.ini", ams::fs::OpenMode_Read))) {
                    return false;
                }
                file = std::make_unique<ams::fs::RemoteFile>(f);
            }

            ContentSettingIniState state{};
            util::ini::ParseFile(file.get(), std::addressof(state), ContentSettingIniHandler);
            *out_enabled = state.route_found && state.route_enabled;
            return state.route_found;
        }

        bool TryGetContentNativePathEnabledFromSystemSettings(bool *out_enabled) {
            if (out_enabled == nullptr) {
                return false;
            }

            std::unique_ptr<ams::fs::fsa::IFile> file;
            {
                FsFile f;
                if (R_FAILED(ams::mitm::fs::OpenAtmosphereSdFile(std::addressof(f), "/config/system_settings.ini", ams::fs::OpenMode_Read))) {
                    return false;
                }
                file = std::make_unique<ams::fs::RemoteFile>(f);
            }

            ContentSettingIniState state{};
            util::ini::ParseFile(file.get(), std::addressof(state), ContentSettingIniHandler);
            *out_enabled = (state.native_path_found && state.native_path_enabled) ||
                           (state.force_path_found && state.force_path_enabled);
            return state.native_path_found || state.force_path_found;
        }

        bool TryGetContentMetadataRouteEnabledFromSystemSettings(bool *out_enabled) {
            if (out_enabled == nullptr) {
                return false;
            }

            std::unique_ptr<ams::fs::fsa::IFile> file;
            {
                FsFile f;
                if (R_FAILED(ams::mitm::fs::OpenAtmosphereSdFile(std::addressof(f), "/config/system_settings.ini", ams::fs::OpenMode_Read))) {
                    return false;
                }
                file = std::make_unique<ams::fs::RemoteFile>(f);
            }

            ContentSettingIniState state{};
            util::ini::ParseFile(file.get(), std::addressof(state), ContentSettingIniHandler);
            *out_enabled = state.metadata_found && state.metadata_enabled;
            return state.metadata_found;
        }

        bool IsExternalProviderSelected() {
            std::unique_ptr<ams::fs::fsa::IFile> file;
            {
                FsFile f;
                if (R_FAILED(ams::mitm::fs::OpenAtmosphereSdFile(std::addressof(f), "/config/system_settings.ini", ams::fs::OpenMode_Read))) {
                    return false;
                }
                file = std::make_unique<ams::fs::RemoteFile>(f);
            }

            ContentSettingIniState state{};
            util::ini::ParseFile(file.get(), std::addressof(state), ContentSettingIniHandler);
            return state.external_kip_found && state.external_kip_enabled;
        }

        void RefreshUsbSettings() {
            const auto now_ns = os::GetSystemTick().ToTimeSpan().GetNanoSeconds();
            g_usb_settings_cache.Refresh(static_cast<u64>(now_ns), [] {
                bool content = false;
                const bool content_enabled = TryGetContentEnabledFromSystemSettings(std::addressof(content)) && content;
                u8 storage = 0;
                const bool storage_enabled = !content_enabled &&
                    settings::fwdbg::GetSettingsItemValue(std::addressof(storage), sizeof(storage), "atmosphere", SettingName) == sizeof(storage) && storage != 0;
                return impl::UsbEnablement{content_enabled, storage_enabled};
            });
        }

        bool IsContentEnabled() {
            return g_usb_settings_cache.IsContentEnabled();
        }

        bool IsEnabled() {
            /* In content mode ams.mitm owns the sole usb:hs session and NCM
             * reaches it through ams:usb. This is deliberately different
             * from the v45 design, where NCM tried to acquire usb:hs from an
             * NCM IPC worker and stalled before returning Host storage. */
            return g_usb_settings_cache.IsEnabled();
        }

        const char *GetStateName(ams::usb::storage::State state) {
            switch (state) {
                case ams::usb::storage::State::Stopped:           return "stopped";
                case ams::usb::storage::State::Starting:           return "starting";
                case ams::usb::storage::State::WaitingForDevice:  return "waiting_for_device";
                case ams::usb::storage::State::Mounting:          return "mounting";
                case ams::usb::storage::State::Ready:             return "ready";
                case ams::usb::storage::State::Error:             return "error";
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        const char *GetFilesystemName(ams::usb::storage::FileSystemType type) {
            switch (type) {
                case ams::usb::storage::FileSystemType::Unknown: return "unknown";
                case ams::usb::storage::FileSystemType::Fat32:   return "fat32";
                case ams::usb::storage::FileSystemType::ExFat:   return "exfat";
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        void FormatCapacity(char *out, size_t out_size, u64 bytes) {
            const char *unit = "B";
            u64 divisor = 1;
            constexpr u64 BytesPerTerabyte = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            if (bytes >= BytesPerTerabyte) {
                unit = "TB";
                divisor = BytesPerTerabyte;
            } else if (bytes >= 1_GB) {
                unit = "GB";
                divisor = 1_GB;
            } else if (bytes >= 1_MB) {
                unit = "MB";
                divisor = 1_MB;
            } else if (bytes >= 1_KB) {
                unit = "KB";
                divisor = 1_KB;
            }
            util::SNPrintf(out, out_size, "%llu %s", static_cast<unsigned long long>(bytes / divisor), unit);
        }

        void GetContentDeviceName(char *out, size_t out_size, const char *partition_name, const char *vendor, const char *product) {
            /* Prefer the USB device identity. The partition label is often
             * only "mbr0"/"gpt0", which is useful for diagnostics but not
             * useful in a user-facing mount notification. */
            const bool has_vendor = vendor != nullptr && vendor[0] != '\0';
            const bool has_product = product != nullptr && product[0] != '\0';
            if (has_vendor && has_product) {
                util::SNPrintf(out, out_size, "%s %s", vendor, product);
            } else if (has_vendor) {
                util::SNPrintf(out, out_size, "%s", vendor);
            } else if (has_product) {
                util::SNPrintf(out, out_size, "%s", product);
            } else if (partition_name != nullptr && partition_name[0] != '\0') {
                util::SNPrintf(out, out_size, "%s", partition_name);
            } else {
                util::SNPrintf(out, out_size, "USB drive");
            }
        }

        const char *GetImageFormatName(ams::usb::game_image::Format format) {
            switch (format) {
                case ams::usb::game_image::Format::Unknown: return "unknown";
                case ams::usb::game_image::Format::Pfs0:    return "pfs0";
                case ams::usb::game_image::Format::Hfs0:    return "hfs0";
                case ams::usb::game_image::Format::Xci:     return "xci";
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        bool IsSafeHomebrewPath(const char *path, size_t length) {
            if (path == nullptr || length == 0 || path[0] != '/') {
                return false;
            }
            if (length == 1) {
                return true;
            }

            size_t offset = 1;
            while (offset < length) {
                const size_t segment_start = offset;
                while (offset < length && path[offset] != '/') {
                    if (path[offset] == ':' || path[offset] == '\\') {
                        return false;
                    }
                    ++offset;
                }

                const size_t segment_length = offset - segment_start;
                if (segment_length == 0 || (segment_length == 1 && path[segment_start] == '.')
                    || (segment_length == 2 && path[segment_start] == '.' && path[segment_start + 1] == '.')) {
                    return false;
                }

                if (offset < length) {
                    ++offset;
                }
            }
            return true;
        }

        Result CopyHomebrewPath(char *out, size_t out_size, const sf::InBuffer &path) {
            R_UNLESS(path.GetPointer() != nullptr && path.GetSize() > 0, usb::ResultInvalidParameter());
            R_UNLESS(path.GetSize() <= out_size, usb::ResultInvalidParameter());

            const auto *begin = reinterpret_cast<const char *>(path.GetPointer());
            const auto *end = static_cast<const char *>(std::memchr(begin, '\0', path.GetSize()));
            R_UNLESS(end != nullptr, usb::ResultInvalidParameter());

            const size_t length = static_cast<size_t>(end - begin);
            R_UNLESS(length > 0 && length < out_size, usb::ResultInvalidParameter());
            R_UNLESS(IsSafeHomebrewPath(begin, length), usb::ResultInvalidParameter());

            std::memcpy(out, begin, length + 1);
            R_SUCCEED();
        }

        Result CopyUsbContentPath(char *out, size_t out_size, const sf::InBuffer &path) {
            R_UNLESS(path.GetPointer() != nullptr && path.GetSize() > 0, usb::ResultInvalidParameter());
            R_UNLESS(path.GetSize() <= out_size, usb::ResultInvalidParameter());

            const auto *begin = reinterpret_cast<const char *>(path.GetPointer());
            const auto *end = static_cast<const char *>(std::memchr(begin, '\0', path.GetSize()));
            R_UNLESS(end != nullptr, usb::ResultInvalidParameter());

            const size_t length = static_cast<size_t>(end - begin);
            const size_t prefix_length = std::strlen(UsbContentMountPrefix);
            R_UNLESS(length > prefix_length && length < out_size, usb::ResultInvalidParameter());
            R_UNLESS(std::memcmp(begin, UsbContentMountPrefix, prefix_length) == 0, usb::ResultInvalidParameter());
            R_UNLESS(IsSafeHomebrewPath(begin + prefix_length, length - prefix_length), usb::ResultInvalidParameter());

            std::memcpy(out, begin, length + 1);
            R_SUCCEED();
        }

        void RememberProgramPath(ncm::ProgramId program_id, const char *path) {
            std::scoped_lock lk(g_program_path_mutex);

            CachedProgramPath *entry = nullptr;
            for (auto &candidate : g_cached_program_paths) {
                if (candidate.valid && candidate.program_id == program_id) {
                    entry = std::addressof(candidate);
                    break;
                }
            }
            if (entry == nullptr) {
                entry = std::addressof(g_cached_program_paths[g_next_cached_program_path]);
                g_next_cached_program_path = (g_next_cached_program_path + 1) % MaxCachedProgramPaths;
            }

            entry->valid = true;
            entry->program_id = program_id;
            util::Strlcpy(entry->path, path, sizeof(entry->path));
            g_last_cached_program_id.store(program_id.value, std::memory_order_release);
        }

        bool FindProgramPath(char *out, size_t out_size, ncm::ProgramId program_id) {
            std::scoped_lock lk(g_program_path_mutex);
            for (const auto &entry : g_cached_program_paths) {
                if (entry.valid && entry.program_id == program_id) {
                    util::Strlcpy(out, entry.path, out_size);
                    return true;
                }
            }
            return false;
        }

        bool CacheMappedProgramPath(ncm::ProgramId program_id) {
            ncm::ContentId program_content_id{};
            bool found = false;
            {
                /* Cache-only lookup: this runs from an active FS request and
                 * must never call back into NCM. The persisted title/content
                 * map is refreshed by the post-boot monitor. */
                std::scoped_lock lk(g_mapped_title_mutex);
                for (size_t i = 0; i < g_cached_mapped_title_count; ++i) {
                    const auto &entry = g_cached_mapped_titles[i];
                    if (entry.program_id == program_id && entry.has_expected_program_content_id) {
                        program_content_id = entry.expected_program_content_id;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                return false;
            }

            ncm::PathString relative_path;
            ncm::MakeSha256HierarchicalContentFilePath_ForFat16KCluster(
                std::addressof(relative_path), program_content_id, "/registered");

            char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
            const int written = util::SNPrintf(mounted_path, sizeof(mounted_path), "%s%s",
                UsbContentMountPrefix, relative_path.Get());
            if (written <= 0 || static_cast<size_t>(written) >= sizeof(mounted_path)) {
                return false;
            }

            RememberProgramPath(program_id, mounted_path);
            return true;
        }

        void RememberMappedTitleContentIds(ncm::ProgramId program_id,
                                           const ncm::ContentId &meta_content_id,
                                           const ncm::ContentId &control_content_id,
                                           const ncm::ContentId &program_content_id) {
            if (program_id == ncm::InvalidProgramId) {
                return;
            }

            std::scoped_lock lk(g_mapped_title_mutex);
            CachedMappedTitle *entry = nullptr;
            for (size_t i = 0; i < g_cached_mapped_title_count; ++i) {
                if (g_cached_mapped_titles[i].program_id == program_id) {
                    entry = g_cached_mapped_titles + i;
                    break;
                }
            }
            if (entry == nullptr) {
                if (g_cached_mapped_title_count >= MaxCachedMappedTitles) {
                    return;
                }
                entry = g_cached_mapped_titles + g_cached_mapped_title_count++;
                *entry = {};
                entry->program_id = program_id;
            }

            /* The persistent v2 title-content map is already the exact,
             * install-time association. Seed the FS-safe RAM snapshot while
             * NS resolves the mapped launch path, before fsp-srv asks for the
             * Control NCA header. This keeps the later FS request cache-only
             * and avoids a re-entrant NCM/SD lookup from inside fs.mitm. */
            if (meta_content_id != ncm::InvalidContentId) {
                entry->expected_meta_content_id = meta_content_id;
                entry->has_expected_meta_content_id = true;
            }
            if (control_content_id != ncm::InvalidContentId) {
                entry->expected_control_content_id = control_content_id;
                entry->has_expected_control_content_id = true;
            }
            if (program_content_id != ncm::InvalidContentId) {
                entry->expected_program_content_id = program_content_id;
                entry->has_expected_program_content_id = true;
            }
        }

        void RefreshMappedTitleCache() {
            ncm::ProgramId mapped_program_ids[MaxCachedMappedTitles]{};
            u32 mapped_count = 0;
            if (R_FAILED(ncm::ListUsbTitles(mapped_program_ids, util::size(mapped_program_ids), std::addressof(mapped_count)))) {
                return;
            }

            CachedMappedTitle refreshed[MaxCachedMappedTitles]{};
            for (u32 i = 0; i < mapped_count; ++i) {
                refreshed[i].program_id = mapped_program_ids[i];

                ncm::ContentId persisted_meta_content_id{};
                ncm::ContentId persisted_control_content_id{};
                ncm::ContentId persisted_program_content_id{};
                if (R_SUCCEEDED(ncm::GetUsbTitleContentIds(mapped_program_ids[i], std::addressof(persisted_meta_content_id), std::addressof(persisted_control_content_id), std::addressof(persisted_program_content_id)))) {
                    refreshed[i].expected_meta_content_id = persisted_meta_content_id;
                    refreshed[i].expected_control_content_id = persisted_control_content_id;
                    refreshed[i].expected_program_content_id = persisted_program_content_id;
                    refreshed[i].has_expected_meta_content_id = persisted_meta_content_id != ncm::InvalidContentId;
                    refreshed[i].has_expected_control_content_id = persisted_control_content_id != ncm::InvalidContentId;
                    refreshed[i].has_expected_program_content_id = persisted_program_content_id != ncm::InvalidContentId;
                    continue;
                }

                const std::array metadata_candidates = {
                    ncm::StorageId::SdCard,
                    ncm::StorageId::BuiltInUser,
                };
                for (const auto storage_id : metadata_candidates) {
                    ncm::ContentMetaDatabase database;
                    if (R_FAILED(ncm::OpenContentMetaDatabase(std::addressof(database), storage_id))) {
                        continue;
                    }

                    ncm::ContentId control_content_id{};
                    if (R_SUCCEEDED(database.GetLatestControl(std::addressof(control_content_id), ncm::ApplicationId{mapped_program_ids[i].value}))) {
                        refreshed[i].expected_control_content_id = control_content_id;
                        refreshed[i].has_expected_control_content_id = true;
                        break;
                    }
                }
            }

            {
                std::scoped_lock lk(g_mapped_title_mutex);
                std::memcpy(g_cached_mapped_titles, refreshed, sizeof(refreshed));
                g_cached_mapped_title_count = mapped_count;
            }

            /* The launch boundary intentionally performs cache-only checks.
             * Build each exact Program path as soon as the persistent map is
             * refreshed so a title installed during this boot does not remain
             * invisible until a Control NCA happens to be opened first. */
            for (u32 i = 0; i < mapped_count; ++i) {
                static_cast<void>(CacheMappedProgramPath(mapped_program_ids[i]));
            }
        }

        bool IsCachedMappedTitle(ncm::ProgramId program_id) {
            {
                std::scoped_lock lk(g_mapped_title_mutex);
                for (size_t i = 0; i < g_cached_mapped_title_count; ++i) {
                    if (g_cached_mapped_titles[i].program_id == program_id) {
                        return true;
                    }
                }
            }
            /* The periodic cache is an optimization, not an authority. NS
             * can request the icon immediately after Commit, before the next
             * monitor refresh. Consult the persisted SD map on a miss; this
             * never enumerates USB or calls back into the NCM service. */
            return ncm::IsUsbTitleMapped(program_id);
        }

        bool TryGetRegisteredPathContentId(ncm::ContentId *out, const char *path) {
            if (out == nullptr || path == nullptr) {
                return false;
            }

            const char *name = std::strrchr(path, '/');
            name = name != nullptr ? name + 1 : path;
            const size_t name_length = std::strlen(name);
            if (name_length != ncm::ContentIdStringLength + 4 || std::strcmp(name + ncm::ContentIdStringLength, ".nca") != 0) {
                return false;
            }

            const auto content_id = ncm::GetContentIdFromString(name, ncm::ContentIdStringLength);
            if (!content_id) {
                return false;
            }

            *out = *content_id;
            return true;
        }

        bool ResolveMappedControlProgramId(ncm::ProgramId *out, const char *requested_path, ncm::ProgramId requested_program_id) {
            if (out == nullptr) {
                return false;
            }

            ncm::ContentId requested_content_id{};
            if (!TryGetRegisteredPathContentId(std::addressof(requested_content_id), requested_path)) {
                return false;
            }

            std::scoped_lock lk(g_mapped_title_mutex);
            for (size_t i = 0; i < g_cached_mapped_title_count; ++i) {
                const auto &entry = g_cached_mapped_titles[i];
                if (entry.has_expected_control_content_id
                    && impl::MatchesUsbControlIdentity(entry.program_id.value, entry.expected_control_content_id,
                                                       requested_program_id.value, requested_content_id)) {
                    *out = entry.program_id;
                    return true;
                }
            }

            return false;
        }

        void RememberControlPath(ncm::ProgramId program_id, u64 media_generation, const char *path) {
            std::scoped_lock lk(g_control_path_mutex);

            CachedControlPath *entry = nullptr;
            for (auto &candidate : g_cached_control_paths) {
                if (candidate.valid && candidate.program_id == program_id) {
                    entry = std::addressof(candidate);
                    break;
                }
            }
            if (entry == nullptr) {
                entry = std::addressof(g_cached_control_paths[g_next_cached_control_path]);
                g_next_cached_control_path = (g_next_cached_control_path + 1) % MaxCachedControlPaths;
            }

            entry->valid = true;
            entry->program_id = program_id;
            entry->media_generation = media_generation;
            util::Strlcpy(entry->path, path, sizeof(entry->path));
        }

        void RememberPendingControlProgram(ncm::ProgramId program_id) {
            if (!ncm::IsApplicationId(program_id) || ncm::IsSystemProgramId(program_id)) {
                return;
            }

            u64 media_generation = 0;
            if (R_FAILED(g_filesystem_service.GetMediaGeneration(std::addressof(media_generation))) || media_generation == 0) {
                return;
            }

            std::scoped_lock lk(g_pending_control_mutex);
            g_pending_control_program = {
                .valid = true,
                .program_id = program_id,
                .media_generation = media_generation,
            };
        }

        bool FindControlPath(char *out, size_t out_size, ncm::ProgramId program_id, u64 media_generation) {
            std::scoped_lock lk(g_control_path_mutex);
            for (const auto &entry : g_cached_control_paths) {
                if (entry.valid && entry.program_id == program_id && entry.media_generation == media_generation) {
                    util::Strlcpy(out, entry.path, out_size);
                    return true;
                }
            }
            return false;
        }

        bool ReadPersistentTitleMap(TitleMapEntry *out_entries, u32 *out_count) {
            AMS_ASSERT(out_entries != nullptr);
            AMS_ASSERT(out_count != nullptr);
            *out_count = 0;

            FsFile file{};
            if (R_FAILED(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), TitleMapPath, ams::fs::OpenMode_Read))) {
                return false;
            }
            ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };

            s64 file_size = 0;
            if (R_FAILED(fsFileGetSize(std::addressof(file), std::addressof(file_size)))) {
                return false;
            }

            TitleMapHeader header{};
            size_t read_size = 0;
            if (file_size < static_cast<s64>(sizeof(header))
                || R_FAILED(fsFileRead(std::addressof(file), 0, std::addressof(header), sizeof(header), FsReadOption_None, std::addressof(read_size)))
                || read_size != sizeof(header)
                || header.magic != TitleMapMagic
                || header.version != TitleMapVersion
                || header.count > TitleMapMaxEntries) {
                return false;
            }

            const s64 expected_size = static_cast<s64>(sizeof(header)) + static_cast<s64>(header.count) * sizeof(TitleMapEntry);
            if (file_size != expected_size) {
                return false;
            }

            if (header.count > 0) {
                read_size = 0;
                const size_t entries_size = static_cast<size_t>(header.count) * sizeof(TitleMapEntry);
                if (R_FAILED(fsFileRead(std::addressof(file), sizeof(header), out_entries, entries_size, FsReadOption_None, std::addressof(read_size)))
                    || read_size != entries_size) {
                    return false;
                }
            }

            *out_count = header.count;
            return true;
        }

        Result WritePersistentTitleMap(const TitleMapEntry *entries, u32 count) {
            R_UNLESS(entries != nullptr || count == 0, usb::ResultInvalidParameter());
            R_UNLESS(count <= TitleMapMaxEntries, ncm::ResultBufferInsufficient());

            static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory(TitleMapDirectory));
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(TitleMapTempPath));
            R_TRY(mitm::fs::CreateAtmosphereSdFile(TitleMapTempPath, 0, ams::fs::CreateOption_None));

            {
                FsFile file{};
                R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), TitleMapTempPath, ams::fs::OpenMode_ReadWrite));
                ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };

                const s64 file_size = static_cast<s64>(sizeof(TitleMapHeader)) + static_cast<s64>(count) * sizeof(TitleMapEntry);
                R_TRY(fsFileSetSize(std::addressof(file), file_size));

                const TitleMapHeader header{
                    .magic = TitleMapMagic,
                    .version = TitleMapVersion,
                    .count = count,
                    .reserved = 0,
                };
                R_TRY(fsFileWrite(std::addressof(file), 0, std::addressof(header), sizeof(header), FsWriteOption_None));
                if (count > 0) {
                    R_TRY(fsFileWrite(std::addressof(file), sizeof(header), entries, static_cast<size_t>(count) * sizeof(TitleMapEntry), FsWriteOption_Flush));
                } else {
                    R_TRY(fsFileFlush(std::addressof(file)));
                }
            }

            char old_path[ams::fs::EntryNameLengthMax + 1]{};
            char new_path[ams::fs::EntryNameLengthMax + 1]{};
            util::SNPrintf(old_path, sizeof(old_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, TitleMapTempPath);
            util::SNPrintf(new_path, sizeof(new_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, TitleMapPath);
            static_cast<void>(ams::fs::DeleteFile(new_path));
            R_RETURN(ams::fs::RenameFile(old_path, new_path));
        }

        Result PersistProgramAlias(ncm::ProgramId alias_program_id, ncm::ProgramId program_id) {
            R_UNLESS(alias_program_id.value != 0 && program_id.value != 0, usb::ResultInvalidParameter());
            R_UNLESS(alias_program_id != program_id, usb::ResultInvalidParameter());
            R_UNLESS(!ncm::IsApplicationId(alias_program_id) && ncm::IsApplicationId(program_id), ncm::ResultContentNotFound());

            std::scoped_lock lk(g_persistent_title_map_mutex);

            std::unique_ptr<TitleMapEntry[]> buffer(new (std::nothrow) TitleMapEntry[TitleMapMaxEntries]{});
            R_UNLESS(buffer,ams::fs::ResultAllocationMemoryFailed());
            auto *entries=buffer.get();
            u32 count = 0;
            // Never turn an unreadable/oversized map into an empty rewrite.
            R_UNLESS(ReadPersistentTitleMap(entries, std::addressof(count)),ncm::ResultInvalidOperation());

            s32 program_index = -1;
            bool changed = false;
            for (u32 i = 0; i < count; i++) {
                if (entries[i].program_id == program_id.value) {
                    program_index = static_cast<s32>(i);
                } else if (entries[i].alias_program_id == alias_program_id.value) {
                    /* An opaque launch id must never identify two titles. */
                    entries[i].alias_program_id = 0;
                    changed = true;
                }
            }

            if (program_index < 0) {
                R_UNLESS(count < TitleMapMaxEntries, ncm::ResultBufferInsufficient());
                program_index = static_cast<s32>(count++);
                entries[program_index] = {
                    .program_id = program_id.value,
                    .alias_program_id = alias_program_id.value,
                };
                changed = true;
            } else if (entries[program_index].alias_program_id != alias_program_id.value) {
                entries[program_index].alias_program_id = alias_program_id.value;
                changed = true;
            }

            R_SUCCEED_IF(!changed);
            R_RETURN(WritePersistentTitleMap(entries, count));
        }

        [[maybe_unused]] void RememberProgramAlias(ncm::ProgramId requested_program_id, ncm::ProgramId resolved_program_id) {
            if (requested_program_id == resolved_program_id) {
                return;
            }

            u64 media_generation = 0;
            if (R_FAILED(g_filesystem_service.GetMediaGeneration(std::addressof(media_generation)))) {
                return;
            }

            std::scoped_lock lk(g_program_alias_mutex);
            CachedProgramAlias *entry = nullptr;
            for (auto &candidate : g_cached_program_aliases) {
                if (candidate.valid && candidate.requested_program_id == requested_program_id) {
                    entry = std::addressof(candidate);
                    break;
                }
            }
            if (entry == nullptr) {
                entry = std::addressof(g_cached_program_aliases[g_next_cached_program_alias]);
                g_next_cached_program_alias = (g_next_cached_program_alias + 1) % MaxCachedProgramAliases;
            }

            const bool same_persisted_pair = entry->valid
                && entry->persisted
                && entry->requested_program_id == requested_program_id
                && entry->resolved_program_id == resolved_program_id;
            entry->valid = true;
            entry->persisted = same_persisted_pair;
            entry->requested_program_id = requested_program_id;
            entry->resolved_program_id = resolved_program_id;
            entry->media_generation = media_generation;
        }

        Result ResolveProgramAlias(ncm::ProgramId *out, ncm::ProgramId requested_program_id) {
            R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
            *out = {};
            R_UNLESS(IsUsbContentLaunchPathEnabled(), ncm::ResultContentNotFound());
            R_UNLESS(requested_program_id.value != 0
                     && !ncm::IsApplicationId(requested_program_id)
                     && !ncm::IsSystemProgramId(requested_program_id),
                     ncm::ResultContentNotFound());

            u64 media_generation = 0;
            R_TRY(g_filesystem_service.GetMediaGeneration(std::addressof(media_generation)));
            R_UNLESS(media_generation != 0, ncm::ResultContentNotFound());

            ncm::ProgramId resolved_program_id{};
            bool from_persistent_map = false;
            {
                /* The durable alias wins across reboots. It is checked before
                 * the one-shot candidate so a qlaunch scan cannot replace an
                 * already validated association. */
                std::scoped_lock lk(g_persistent_title_map_mutex);
                std::unique_ptr<TitleMapEntry[]> buffer(new (std::nothrow) TitleMapEntry[TitleMapMaxEntries]{});
                R_UNLESS(buffer,ams::fs::ResultAllocationMemoryFailed());
                auto *entries=buffer.get();
                u32 count = 0;
                if (ReadPersistentTitleMap(entries, std::addressof(count))) {
                    for (u32 i = 0; i < count; ++i) {
                        const auto &entry = entries[i];
                        const ncm::ProgramId program_id{entry.program_id};
                        if (entry.alias_program_id == requested_program_id.value
                            && ncm::IsApplicationId(program_id)
                            && !ncm::IsSystemProgramId(program_id)) {
                            resolved_program_id = program_id;
                            from_persistent_map = true;
                            break;
                        }
                    }
                }
            }

            if (!from_persistent_map) {
                std::scoped_lock lk(g_pending_control_mutex);
                if (g_pending_control_program.valid
                    && g_pending_control_program.media_generation == media_generation) {
                    resolved_program_id = g_pending_control_program.program_id;
                    g_pending_control_program.valid = false;
                }
            }

            R_UNLESS(resolved_program_id.value != 0
                     && resolved_program_id != requested_program_id
                     && ncm::IsApplicationId(resolved_program_id)
                     && !ncm::IsSystemProgramId(resolved_program_id),
                     ncm::ResultContentNotFound());

            if (!from_persistent_map) {
                /* Persist only the pair that was just validated by the USB
                 * Control NCA path. A write failure must not discard this
                 * already-valid current launch; the next boot will simply
                 * need to observe the Control NCA again. */
                static_cast<void>(PersistProgramAlias(requested_program_id, resolved_program_id));
            }

            *out = resolved_program_id;
            R_SUCCEED();
        }

        [[maybe_unused]] void FlushPendingProgramAliases() {
            struct PendingAlias {
                ncm::ProgramId requested_program_id;
                ncm::ProgramId resolved_program_id;
            };

            PendingAlias pending[MaxCachedProgramAliases]{};
            size_t pending_count = 0;
            {
                std::scoped_lock lk(g_program_alias_mutex);
                for (const auto &entry : g_cached_program_aliases) {
                    if (entry.valid && !entry.persisted && pending_count < util::size(pending)) {
                        pending[pending_count++] = {
                            .requested_program_id = entry.requested_program_id,
                            .resolved_program_id = entry.resolved_program_id,
                        };
                    }
                }
            }

            for (size_t i = 0; i < pending_count; i++) {
                const auto &alias = pending[i];
                const Result persist_result = PersistProgramAlias(alias.requested_program_id, alias.resolved_program_id);
                if (R_SUCCEEDED(persist_result)) {
                    std::scoped_lock lk(g_program_alias_mutex);
                    for (auto &entry : g_cached_program_aliases) {
                        if (entry.valid
                            && entry.requested_program_id == alias.requested_program_id
                            && entry.resolved_program_id == alias.resolved_program_id) {
                            entry.persisted = true;
                        }
                    }
                }
            }
        }

        Result TranslateContentPathToUsb(char *out, size_t out_size, const char *content_path) {
            R_UNLESS(out != nullptr && out_size > 0 && content_path != nullptr, usb::ResultInvalidParameter());

            constexpr const char UsbContentPrefix[] = "usbcontent:";
            constexpr const char StockSdPathPrefix[] = "@Sdcard:/Nintendo/Contents";
            constexpr const char SdContentMountPrefix[] = "@SdCardContent:";

            const size_t content_path_length = strnlen(content_path, ams::fs::EntryNameLengthMax + 1);
            R_UNLESS(content_path_length <= ams::fs::EntryNameLengthMax, ams::fs::ResultTooLongPath());

            const char *suffix = nullptr;
            if (std::strncmp(content_path, UsbContentPrefix, sizeof(UsbContentPrefix) - 1) == 0) {
                suffix = content_path + sizeof(UsbContentPrefix) - 1;
            } else if (std::strncmp(content_path, StockSdPathPrefix, sizeof(StockSdPathPrefix) - 1) == 0) {
                suffix = content_path + sizeof(StockSdPathPrefix) - 1;
            } else if (std::strncmp(content_path, SdContentMountPrefix, sizeof(SdContentMountPrefix) - 1) == 0) {
                suffix = content_path + sizeof(SdContentMountPrefix) - 1;
            } else {
                R_THROW(ams::fs::ResultPathNotFound());
            }

            /* NCM and fsp-srv can expose the content-storage mount as either
             * @SdCardContent:/registered or @SdCardContent://registered.
             * The private provider mount always uses one root separator. */
            while (*suffix == '/') {
                ++suffix;
            }

            const size_t prefix_length = sizeof(UsbContentPrefix) - 1;
            const size_t suffix_length = std::strlen(suffix);
            R_UNLESS(prefix_length + 1 + suffix_length < out_size, ams::fs::ResultTooLongPath());

            std::memcpy(out, UsbContentPrefix, prefix_length);
            out[prefix_length] = '/';
            std::memcpy(out + prefix_length + 1, suffix, suffix_length + 1);
            R_SUCCEED();
        }

        Result EnsureUsbStarted();
        Result CheckImageScanGeneration(u64 expected_generation);

        Result OpenUsbNcaReader(std::shared_ptr<fssystem::NcaReader> *out, const char *mounted_path, bool apply_external_key, bool already_started = false, bool require_signed = false) {
            R_UNLESS(out != nullptr && mounted_path != nullptr, usb::ResultInvalidParameter());
            if (!already_started) { R_TRY(EnsureUsbStarted()); }

            const size_t prefix_length = std::strlen(UsbContentMountPrefix);
            R_UNLESS(std::strncmp(mounted_path, UsbContentMountPrefix, prefix_length) == 0, ams::fs::ResultPathNotFound());
            const char *relative_path = mounted_path + prefix_length;
            R_UNLESS(IsSafeHomebrewPath(relative_path, std::strlen(relative_path)), ams::fs::ResultInvalidPath());

            std::unique_ptr<ams::fs::fsa::IFileSystem> unique_base_fs;
            R_TRY(ams::usb::filesystem::CreateReadOnlyFileSystem(std::addressof(unique_base_fs), std::addressof(g_filesystem_service)));
            std::shared_ptr<ams::fs::fsa::IFileSystem> base_fs(std::move(unique_base_fs));
            R_UNLESS(base_fs != nullptr, ams::fs::ResultAllocationMemoryFailed());

            ams::fs::Path nca_path;
            R_TRY(nca_path.InitializeWithNormalization(relative_path));

            auto file_storage = fssystem::AllocateShared<ams::fs::FileStorageBasedFileSystem>();
            R_UNLESS(file_storage != nullptr, ams::fs::ResultAllocationMemoryFailedInNcaFileSystemServiceImplB());
            R_TRY(file_storage->Initialize(std::move(base_fs), nca_path, ams::fs::OpenMode_Read));

            std::shared_ptr<fssystem::NcaReader> reader;
            /* This file storage is built only from the private USB filesystem
             * above. No SD/NAND or global crypto policy is changed. */
            const bool allow_unsigned = !require_signed && g_allow_unsigned_usb_application.load(std::memory_order_acquire);
            const Result header_result = fssystem::GetFileSystemCreatorInterfaces()->storage_on_nca_creator->CreateNcaReader(
                std::addressof(reader), std::move(file_storage), allow_unsigned);
            if (R_FAILED(header_result) || (reader != nullptr && !reader->GetHeaderSign1VerificationResultForDiagnostics())) {
                ams::usb::content::LoaderTraceInfo signature_trace{};
                signature_trace.stage = 204;
                signature_trace.result = header_result.GetValue();
                signature_trace.program_id = reader != nullptr ? reader->GetProgramId() : 0;
                util::SNPrintf(signature_trace.path, sizeof(signature_trace.path), "USB header allow_unsigned=%u accepted_unsigned=%u %s",
                    allow_unsigned ? 1u : 0u, R_SUCCEEDED(header_result) ? 1u : 0u, mounted_path);
                AppendLoaderTraceRecord(signature_trace);
                if (R_SUCCEEDED(header_result)) { g_unsigned_usb_headers_accepted.fetch_add(1); }
            }
            R_TRY(header_result);

            ams::fs::RightsId rights_id{};
            reader->GetRightsId(rights_id.data, sizeof(rights_id.data));
            if (rights_id != ams::fs::InvalidRightsId && apply_external_key) {
                /* FS registers external title keys while the launch path is
                 * being assembled. USB code can be opened a little earlier
                 * than the stock path, so allow that registration to settle
                 * instead of turning a harmless ordering race into a title
                 * that stays on the Home-menu spinner. */
                constexpr size_t ExternalKeyRetryCount = 20;
                constexpr auto ExternalKeyRetryDelay = TimeSpan::FromMilliSeconds(25);
                spl::AccessKey access_key{};
                Result key_result = ams::fs::ResultNcaExternalKeyUnregistered();
                for (size_t i = 0; i < ExternalKeyRetryCount; i++) {
                    key_result = g_external_key_manager.Find(std::addressof(access_key), rights_id);
                    if (R_SUCCEEDED(key_result)) {
                        break;
                    }
                    os::SleepThread(ExternalKeyRetryDelay);
                }
                if (R_SUCCEEDED(key_result)) { key_stats::hits.fetch_add(1); }
                else { key_stats::misses.fetch_add(1); }
                ams::usb::content::LoaderTraceInfo key_trace{};
                key_trace.program_id = reader->GetProgramId();
                key_trace.stage = 203;
                key_trace.result = key_result.GetValue();
                util::SNPrintf(key_trace.path, sizeof(key_trace.path),
                    "external-key lookup es=%llu reg=%llu accepted=%llu mirrored=%llu removed=%llu",
                    static_cast<unsigned long long>(key_stats::es_sessions.load()),
                    static_cast<unsigned long long>(key_stats::registrations.load()),
                    static_cast<unsigned long long>(key_stats::accepted.load()),
                    static_cast<unsigned long long>(key_stats::mirrored.load()),
                    static_cast<unsigned long long>(key_stats::removals.load()));
                AppendLoaderTraceRecord(key_trace);
                R_TRY(key_result);
                reader->SetExternalDecryptionKey(std::addressof(access_key), sizeof(access_key));
            }

            *out = std::move(reader);
            R_SUCCEED();
        }

        Result OpenUsbNcaStorage(std::shared_ptr<ams::fs::IStorage> *out, std::shared_ptr<fssystem::NcaReader> reader, fssystem::NcaFsHeader::FsType wanted_type) {
            R_UNLESS(out != nullptr && reader != nullptr, usb::ResultInvalidParameter());

            for (s32 index = 0; index < fssystem::NcaHeader::FsCountMax; ++index) {
                if (!reader->HasFsInfo(index)) {
                    continue;
                }

                fssystem::NcaFsHeaderReader header_reader;
                R_TRY(header_reader.Initialize(*reader, index));
                if (header_reader.GetFsType() != wanted_type) {
                    continue;
                }

                std::shared_ptr<ams::fs::IStorage> storage;
                std::shared_ptr<fssystem::IAsynchronousAccessSplitter> splitter;
                R_TRY(fssystem::GetFileSystemCreatorInterfaces()->storage_on_nca_creator->Create(std::addressof(storage), std::addressof(splitter), std::addressof(header_reader), std::move(reader), index));
                *out = std::move(storage);
                R_SUCCEED();
            }

            R_THROW(ams::fs::ResultInvalidNcaFileSystemType());
        }

        Result MakeLaunchContentPath(char *out, size_t size, const ncm::ContentId &id) {
            R_UNLESS(id != ncm::InvalidContentId, ncm::ResultContentNotFound());
            ncm::PathString relative;
            ncm::MakeSha256HierarchicalContentFilePath_ForFat16KCluster(std::addressof(relative), id, "/registered");
            const int length = util::SNPrintf(out, size, "%s%s", UsbContentMountPrefix, relative.Get());
            R_UNLESS(length > 0 && static_cast<size_t>(length) < size, ams::fs::ResultInvalidPath());
            R_SUCCEED();
        }

        Result CheckLaunchGeneration(const UsbLaunchSnapshot &snapshot) {
            u64 generation{};
            R_TRY(g_filesystem_service.GetMediaGeneration(std::addressof(generation)));
            R_UNLESS(generation == snapshot.generation, ams::fs::ResultPathNotFound());
            R_SUCCEED();
        }

        Result OpenLaunchReader(std::shared_ptr<fssystem::NcaReader> *out,
                const UsbLaunchSnapshot &snapshot, const ncm::ContentId &id,
                fssystem::NcaHeader::ContentType type, u64 expected_id, u64 alternate_id = 0) {
            R_TRY(CheckLaunchGeneration(snapshot));
            char path[ams::fs::EntryNameLengthMax + 1]{};
            R_TRY(MakeLaunchContentPath(path, sizeof(path), id));
            R_TRY(OpenUsbNcaReader(out, path, true));
            /* NCM's Data category may refer to an NCA Data OR PublicData
             * archive. Only the add-on path requests Data here: Program
             * readers must still match exactly. Identity, generation, crypto
             * and filesystem-integrity checks are unchanged. */
            const auto actual_type = (*out)->GetContentType();
            R_UNLESS(actual_type == type || (type == fssystem::NcaHeader::ContentType::Data
                && actual_type == fssystem::NcaHeader::ContentType::PublicData), ams::fs::ResultPreconditionViolation());
            R_UNLESS((*out)->GetProgramId() == expected_id
                || (alternate_id != 0 && (*out)->GetProgramId() == alternate_id), ams::fs::ResultInvalidArgument());
            R_RETURN(CheckLaunchGeneration(snapshot));
        }

        /* Use Atmosphere's original/current NCA driver. It retains the base
         * reader for indirect blocks rather than treating an update as a
         * standalone archive. Verification and compression layers stay active. */
        Result OpenLaunchProgramStorage(std::shared_ptr<ams::fs::IStorage> *out,
                ams::fs::CodeVerificationData *verification, const UsbLaunchSnapshot &snapshot,
                fssystem::NcaFsHeader::FsType wanted_type, const LaunchPin &witness_pin = {}) {
            std::shared_ptr<fssystem::NcaReader> base, patch;
            const auto &content = snapshot.content;
            R_TRY(OpenLaunchReader(std::addressof(base), snapshot, content.base,
                fssystem::NcaHeader::ContentType::Program, content.program_id));
            if (content.patch_id != 0) {
                R_TRY(OpenLaunchReader(std::addressof(patch), snapshot, content.patch,
                    fssystem::NcaHeader::ContentType::Program, content.program_id,
                    content.patch_id + (content.program_id - content.application_id)));
            }
            /* An absent section inherits base. A present, invalid patch
             * section is an error and must not fall back to base. */
            auto current = patch ? patch : base;
            for (int pass = 0; pass < (patch ? 2 : 1); ++pass) {
                for (s32 index = 0; index < fssystem::NcaHeader::FsCountMax; ++index) {
                    if (!current->HasFsInfo(index)) continue;
                    fssystem::NcaFsHeaderReader header;
                    R_TRY(header.Initialize(*current, index));
                    if (header.GetFsType() != wanted_type) continue;
                    std::shared_ptr<fssystem::IAsynchronousAccessSplitter> splitter;
                    if (current == patch) {
                        if (base->HasFsInfo(index)) {
                            fssystem::NcaFsHeaderReader original_header;
                            R_TRY(original_header.Initialize(*base, index));
                            R_UNLESS(original_header.GetFsType() == wanted_type, ams::fs::ResultInvalidNcaFileSystemType());
                        }
                        pw::Sample sample;
                        const auto epoch = game_wake::wake_epoch.load(std::memory_order_acquire);
                        const bool observing = wanted_type == fssystem::NcaFsHeader::FsType::RomFs &&
                            witness_pin.get() == &snapshot && BeginPatchWitness(witness_pin, current.get(), sample);
                        const Result created = fssystem::GetFileSystemCreatorInterfaces()->storage_on_nca_creator->CreateWithPatch(
                            out, std::addressof(splitter), std::addressof(header), base, current, index);
                        if (observing) FinishPatchWitness(witness_pin, index, epoch, sample, created, R_SUCCEEDED(CheckLaunchGeneration(snapshot)));
                        R_TRY(created);
                    } else {
                        R_TRY(fssystem::GetFileSystemCreatorInterfaces()->storage_on_nca_creator->Create(
                            out, std::addressof(splitter), std::addressof(header), current, index));
                    }
                    if (verification != nullptr) {
                        *verification = {};
                        current->GetHeaderSign2(verification->signature, sizeof(verification->signature));
                        current->GetHeaderSign2TargetHash(verification->target_hash, sizeof(verification->target_hash));
                        verification->has_data = true;
                    }
                    R_RETURN(CheckLaunchGeneration(snapshot));
                }
                current = base;
            }
            R_THROW(ams::fs::ResultInvalidNcaFileSystemType());
        }

        bool IsRegisteredNcaName(const char *name) {
            if (name == nullptr) {
                return false;
            }
            const size_t name_length = std::strlen(name);
            if (name_length != ncm::ContentIdStringLength + 4 || std::strcmp(name + ncm::ContentIdStringLength, ".nca") != 0) {
                return false;
            }
            return ncm::GetContentIdFromString(name, ncm::ContentIdStringLength).has_value();
        }

        [[maybe_unused]] Result FindUsbNcaPathByProgramAndType(char *out_path, size_t out_size, ncm::ProgramId program_id, fssystem::NcaHeader::ContentType wanted_type) {
            R_UNLESS(out_path != nullptr && out_size > 0, usb::ResultInvalidParameter());
            R_TRY(EnsureUsbStarted());

            u64 media_generation = 0;
            R_TRY(g_filesystem_service.GetMediaGeneration(std::addressof(media_generation)));
            if (wanted_type == fssystem::NcaHeader::ContentType::Control
                && FindControlPath(out_path, out_size, program_id, media_generation)) {
                R_SUCCEED();
            }

            size_t scanned_directories = 0;
            size_t scanned_files = 0;
            size_t skip_directories = 0;
            while (scanned_directories < MaxControlScanDirectories && scanned_files < MaxControlScanFiles) {
                R_TRY(CheckImageScanGeneration(media_generation));

                ams::usb::volume::DirectoryEntryInfo directories[ControlScanBatchSize]{};
                size_t directory_count = 0;
                R_TRY(g_filesystem_service.ReadDirectoryAtGeneration("/registered", directories, util::size(directories), skip_directories, true, false, std::addressof(directory_count), media_generation));
                if (directory_count == 0) {
                    break;
                }

                for (size_t directory_index = 0; directory_index < directory_count; ++directory_index) {
                    ++skip_directories;
                    ++scanned_directories;
                    if (!directories[directory_index].directory || scanned_directories > MaxControlScanDirectories) {
                        continue;
                    }

                    char directory_path[ams::fs::EntryNameLengthMax + 1]{};
                    const int directory_path_length = util::SNPrintf(directory_path, sizeof(directory_path), "/registered/%s", directories[directory_index].name);
                    if (directory_path_length <= 0 || static_cast<size_t>(directory_path_length) >= sizeof(directory_path)) {
                        continue;
                    }

                    size_t skip_files = 0;
                    while (scanned_files < MaxControlScanFiles) {
                        R_TRY(CheckImageScanGeneration(media_generation));

                        ams::usb::volume::DirectoryEntryInfo files[ControlScanBatchSize]{};
                        size_t file_count = 0;
                        const Result directory_result = g_filesystem_service.ReadDirectoryAtGeneration(directory_path, files, util::size(files), skip_files, false, true, std::addressof(file_count), media_generation);
                        if (R_FAILED(directory_result) || file_count == 0) {
                            break;
                        }

                        for (size_t file_index = 0; file_index < file_count; ++file_index) {
                            ++skip_files;
                            ++scanned_files;
                            if (scanned_files > MaxControlScanFiles || files[file_index].directory || !IsRegisteredNcaName(files[file_index].name)) {
                                continue;
                            }

                            char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
                            const int mounted_path_length = util::SNPrintf(mounted_path, sizeof(mounted_path), "%s%s/%s", UsbContentMountPrefix, directory_path, files[file_index].name);
                            if (mounted_path_length <= 0 || static_cast<size_t>(mounted_path_length) >= sizeof(mounted_path)) {
                                continue;
                            }

                            std::shared_ptr<fssystem::NcaReader> reader;
                            if (R_FAILED(OpenUsbNcaReader(std::addressof(reader), mounted_path, false))) {
                                continue;
                            }
                            if (reader->GetContentType() != wanted_type || reader->GetProgramId() != program_id.value) {
                                continue;
                            }

                            util::Strlcpy(out_path, mounted_path, out_size);
                            if (wanted_type == fssystem::NcaHeader::ContentType::Control) {
                                RememberControlPath(program_id, media_generation, mounted_path);
                            }
                            R_SUCCEED();
                        }

                        if (file_count < util::size(files)) {
                            break;
                        }
                    }
                }

                if (directory_count < util::size(directories)) {
                    break;
                }
            }

            R_THROW(ncm::ResultContentNotFound());
        }

        Result OpenUsbCodeFileSystemImpl(std::shared_ptr<ams::fs::fsa::IFileSystem> *out, ams::fs::CodeVerificationData *out_verification, const char *mounted_path, ncm::ProgramId program_id) {
            R_UNLESS(out != nullptr && out_verification != nullptr, usb::ResultInvalidParameter());

            std::shared_ptr<fssystem::NcaReader> reader;
            R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, true));
            R_UNLESS(reader->GetContentType() == fssystem::NcaHeader::ContentType::Program, ams::fs::ResultPreconditionViolation());
            R_UNLESS(reader->GetProgramId() == program_id.value, ams::fs::ResultInvalidArgument());

            std::memset(out_verification, 0, sizeof(*out_verification));
            reader->GetHeaderSign2(out_verification->signature, sizeof(out_verification->signature));
            reader->GetHeaderSign2TargetHash(out_verification->target_hash, sizeof(out_verification->target_hash));
            out_verification->has_data = true;

            std::shared_ptr<ams::fs::IStorage> storage;
            R_TRY(OpenUsbNcaStorage(std::addressof(storage), std::move(reader), fssystem::NcaFsHeader::FsType::PartitionFs));
            R_TRY(fssystem::GetFileSystemCreatorInterfaces()->partition_fs_creator->Create(out, std::move(storage)));

            RememberProgramPath(program_id, mounted_path);
            R_SUCCEED();
        }

        [[maybe_unused]] bool BuildHomebrewPath(char *out, size_t out_size, const char *directory, const char *name) {
            const size_t directory_length = std::strlen(directory);
            const size_t name_length = std::strlen(name);
            const bool is_root = directory_length == 1 && directory[0] == '/';
            const size_t separator_length = is_root ? 0 : 1;
            if (directory_length > SIZE_MAX - separator_length - name_length - 1) {
                return false;
            }

            const size_t required_size = directory_length + separator_length + name_length + 1;
            if (required_size > out_size) {
                return false;
            }

            std::memcpy(out, directory, directory_length);
            size_t offset = directory_length;
            if (!is_root) {
                out[offset++] = '/';
            }
            std::memcpy(out + offset, name, name_length);
            out[offset + name_length] = '\0';
            return true;
        }

        void ResetImageScan(Result result) {
            g_image_scan = {};
            g_image_scan.result = static_cast<Result>(result).GetValue();
        }

        void ResetHomebrewScan(Result result) {
            g_homebrew_scan = {};
            g_homebrew_scan.result = static_cast<Result>(result).GetValue();
        }

        [[maybe_unused]] Result CheckImageScanGeneration(u64 expected_generation) {
            u64 current_generation = 0;
            R_TRY(g_filesystem_service.GetMediaGeneration(std::addressof(current_generation)));
            R_UNLESS(current_generation == expected_generation, usb::ResultMediaChanged());
            R_SUCCEED();
        }

        [[maybe_unused]] Result RefreshImageScan() {
            ams::usb::filesystem::Status filesystem_status{};
            const auto filesystem_result = g_filesystem_service.GetStatus(std::addressof(filesystem_status));
            if (R_FAILED(filesystem_result)) {
                ResetImageScan(filesystem_result);
                R_RETURN(filesystem_result);
            }

            if (!filesystem_status.mounted || !filesystem_status.ready) {
                const auto result = static_cast<Result>(filesystem_status.last_result);
                ResetImageScan(result);
                R_RETURN(result);
            }

            u64 media_generation = 0;
            const auto generation_result = g_filesystem_service.GetMediaGeneration(std::addressof(media_generation));
            if (R_FAILED(generation_result)) {
                ResetImageScan(generation_result);
                R_RETURN(generation_result);
            }

            if (g_image_scan.scanned && g_image_scan.media_generation == media_generation) {
                R_SUCCEED();
            }

            g_image_scan = {};
            g_image_scan.scanned = true;
            g_image_scan.result = ResultSuccess().GetValue();
            g_image_scan.media_generation = media_generation;

            size_t skip_entries = 0;
            while (skip_entries < MaxImageDirectoryEntries) {
                const auto before_read_result = CheckImageScanGeneration(media_generation);
                if (R_FAILED(before_read_result)) {
                    ResetImageScan(before_read_result);
                    R_RETURN(before_read_result);
                }

                ams::usb::volume::DirectoryEntryInfo entries[ImageDirectoryBatchSize]{};
                size_t entry_count = 0;
                const auto directory_result = g_filesystem_service.ReadDirectoryAtGeneration("/", entries, util::size(entries), skip_entries, false, true, std::addressof(entry_count), media_generation);
                if (R_FAILED(directory_result)) {
                    ResetImageScan(directory_result);
                    R_RETURN(directory_result);
                }

                const auto after_read_result = CheckImageScanGeneration(media_generation);
                if (R_FAILED(after_read_result)) {
                    ResetImageScan(after_read_result);
                    R_RETURN(after_read_result);
                }
                if (entry_count == 0) {
                    break;
                }

                for (size_t i = 0; i < entry_count; ++i) {
                    skip_entries++;
                    if (!IsImageCandidate(entries[i].name)) {
                        continue;
                    }

                    ++g_image_scan.candidate_count;

                    char path[ams::fs::EntryNameLengthMax + 1]{};
                    const size_t name_length = std::strlen(entries[i].name);
                    if (name_length + 2 > sizeof(path)) {
                        ++g_image_scan.invalid_count;
                        continue;
                    }
                    path[0] = '/';
                    std::memcpy(path + 1, entries[i].name, name_length);
                    path[name_length + 1] = '\0';

                    ams::usb::game_image::Reader reader;
                    const auto open_result = reader.Open(std::addressof(g_filesystem_service), path);
                    auto result = open_result;
                    ams::usb::game_image::Info image_info{};
                    if (R_SUCCEEDED(result)) {
                        result = reader.GetInfo(std::addressof(image_info));
                    }

                    if (R_SUCCEEDED(result)) {
                        ++g_image_scan.valid_count;
                    } else if (result.GetValue() == ams::usb::ResultGameImageUnsupported().GetValue()) {
                        ++g_image_scan.unsupported_count;
                    } else {
                        ++g_image_scan.invalid_count;
                    }

                    if (g_image_scan.record_count < MaxIndexedImages) {
                        auto &record = g_image_scan.records[g_image_scan.record_count++];
                        record.result = static_cast<Result>(result).GetValue();
                        record.format = R_SUCCEEDED(result) ? image_info.format : ams::usb::game_image::Format::Unknown;
                        record.entry_count = R_SUCCEEDED(result) ? image_info.entry_count : 0;
                        record.size = R_SUCCEEDED(result) ? image_info.image_size : entries[i].size;
                        std::memcpy(record.path, path, sizeof(record.path));
                    }
                    reader.Close();
                }

                if (entry_count < util::size(entries)) {
                    break;
                }
            }

            if (skip_entries >= MaxImageDirectoryEntries) {
                g_image_scan.truncated = true;
            }
            R_SUCCEED();
        }

        [[maybe_unused]] Result RefreshHomebrewScan() {
            ams::usb::filesystem::Status filesystem_status{};
            const auto filesystem_result = g_filesystem_service.GetStatus(std::addressof(filesystem_status));
            if (R_FAILED(filesystem_result)) {
                ResetHomebrewScan(filesystem_result);
                R_RETURN(filesystem_result);
            }

            if (!filesystem_status.mounted || !filesystem_status.ready) {
                const auto result = static_cast<Result>(filesystem_status.last_result);
                ResetHomebrewScan(result);
                R_RETURN(result);
            }

            u64 media_generation = 0;
            const auto generation_result = g_filesystem_service.GetMediaGeneration(std::addressof(media_generation));
            if (R_FAILED(generation_result)) {
                ResetHomebrewScan(generation_result);
                R_RETURN(generation_result);
            }

            if (g_homebrew_scan.scanned && g_homebrew_scan.media_generation == media_generation) {
                R_SUCCEED();
            }

            g_homebrew_scan = {};
            g_homebrew_scan.scanned = true;
            g_homebrew_scan.result = ResultSuccess().GetValue();
            g_homebrew_scan.media_generation = media_generation;

            char directory_paths[MaxHomebrewDirectories][ams::fs::EntryNameLengthMax + 1]{};
            std::memcpy(directory_paths[0], "/", 2);
            size_t directory_count = 1;
            size_t next_directory = 0;
            size_t scanned_entries = 0;

            while (next_directory < directory_count && scanned_entries < MaxHomebrewDirectoryEntries) {
                const char *directory_path = directory_paths[next_directory++];
                size_t skip_entries = 0;

                while (skip_entries < MaxHomebrewDirectoryEntries && scanned_entries < MaxHomebrewDirectoryEntries) {
                    const auto before_read_result = CheckImageScanGeneration(media_generation);
                    if (R_FAILED(before_read_result)) {
                        ResetHomebrewScan(before_read_result);
                        R_RETURN(before_read_result);
                    }

                    ams::usb::volume::DirectoryEntryInfo entries[HomebrewDirectoryBatchSize]{};
                    size_t entry_count = 0;
                    const auto directory_result = g_filesystem_service.ReadDirectoryAtGeneration(directory_path, entries, util::size(entries), skip_entries, true, true, std::addressof(entry_count), media_generation);
                    if (R_FAILED(directory_result)) {
                        ResetHomebrewScan(directory_result);
                        R_RETURN(directory_result);
                    }

                    const auto after_read_result = CheckImageScanGeneration(media_generation);
                    if (R_FAILED(after_read_result)) {
                        ResetHomebrewScan(after_read_result);
                        R_RETURN(after_read_result);
                    }
                    if (entry_count == 0) {
                        break;
                    }

                    for (size_t i = 0; i < entry_count; ++i) {
                        ++skip_entries;
                        ++scanned_entries;

                        char path[ams::fs::EntryNameLengthMax + 1]{};
                        if (!BuildHomebrewPath(path, sizeof(path), directory_path, entries[i].name)) {
                            g_homebrew_scan.truncated = true;
                            continue;
                        }

                        if (entries[i].directory) {
                            if (directory_count < MaxHomebrewDirectories) {
                                std::memcpy(directory_paths[directory_count++], path, sizeof(path));
                            } else {
                                g_homebrew_scan.truncated = true;
                            }
                            continue;
                        }

                        if (!IsHomebrewCandidate(entries[i].name)) {
                            continue;
                        }

                        ++g_homebrew_scan.candidate_count;

                        ams::usb::homebrew::NroReader reader;
                        const auto open_result = reader.Open(std::addressof(g_filesystem_service), path);
                        auto result = open_result;
                        ams::usb::homebrew::NroInfo nro_info{};
                        if (R_SUCCEEDED(result)) {
                            result = reader.GetInfo(std::addressof(nro_info));
                        }

                        if (R_SUCCEEDED(result)) {
                            ++g_homebrew_scan.valid_count;
                        } else {
                            ++g_homebrew_scan.invalid_count;
                        }

                        if (g_homebrew_scan.record_count < MaxIndexedHomebrew) {
                            auto &record = g_homebrew_scan.records[g_homebrew_scan.record_count++];
                            record.result = static_cast<Result>(result).GetValue();
                            record.version = R_SUCCEEDED(result) ? nro_info.version : 0;
                            record.flags = R_SUCCEEDED(result) ? nro_info.flags : 0;
                            record.image_size = R_SUCCEEDED(result) ? nro_info.image_size : 0;
                            record.file_size = R_SUCCEEDED(result) ? nro_info.file_size : entries[i].size;
                            std::memcpy(record.path, path, sizeof(record.path));
                        }
                        reader.Close();
                    }

                    if (entry_count < util::size(entries)) {
                        break;
                    }
                }
            }

            if (scanned_entries >= MaxHomebrewDirectoryEntries) {
                g_homebrew_scan.truncated = true;
            }
            R_SUCCEED();
        }

        // Read-only descriptor evidence. Separate, bounded SD log; no USB IPC
        // or volume locks here. Keep a previous complete file for recovery if
        // the rename step fails. Only changed snapshots are written.
        usb::mapping::SnapshotData g_device_map;
        u32 g_device_map_published_revision = 0;
        u32 g_device_map_log_result = 0;
        Result PublishDeviceMapSnapshot() {
            static usb::mapping::SnapshotData next;
            if (!usb::mapping::Snapshot(next)) R_RETURN(usb::ResultOperationDenied());
            g_device_map = next;
            if (!g_device_map.revision || g_device_map.revision == g_device_map_published_revision) R_SUCCEED();
            static char buffer[usb::mapping::ReportBytes];
            const int count = usb::mapping::Format(buffer, sizeof(buffer), g_device_map);
            R_UNLESS(count > 0 && static_cast<size_t>(count) < sizeof(buffer), usb::ResultInvalidParameter());
            constexpr const char *temp = "/logs/usb_device_map.txt.tmp";
            static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory("/logs"));
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(temp));
            R_TRY(mitm::fs::CreateAtmosphereSdFile(temp, 0, ams::fs::CreateOption_None));
            {
                FsFile file{};
                R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), temp, ams::fs::OpenMode_ReadWrite));
                ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
                R_TRY(fsFileSetSize(std::addressof(file), count));
                R_TRY(fsFileWrite(std::addressof(file), 0, buffer, count, FsWriteOption_Flush));
            }
            char source[ams::fs::EntryNameLengthMax + 1]{};
            char target[ams::fs::EntryNameLengthMax + 1]{};
            char previous[ams::fs::EntryNameLengthMax + 1]{};
            util::SNPrintf(source, sizeof(source), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, temp);
            util::SNPrintf(target, sizeof(target), "%s:/atmosphere/logs/usb_device_map.txt", ams::fs::impl::SdCardFileSystemMountName);
            util::SNPrintf(previous, sizeof(previous), "%s:/atmosphere/logs/usb_device_map.previous.txt", ams::fs::impl::SdCardFileSystemMountName);
            static_cast<void>(ams::fs::DeleteFile(previous));
            const auto backup_rc = ams::fs::RenameFile(target, previous);
            // A locked public snapshot stays intact; never delete it to force
            // replacement. The next monitor pass retries the complete file.
            R_UNLESS(R_SUCCEEDED(backup_rc) || ams::fs::ResultPathNotFound::Includes(backup_rc), backup_rc);
            const auto rc = ams::fs::RenameFile(source, target);
            if (R_FAILED(rc) && R_SUCCEEDED(backup_rc)) static_cast<void>(ams::fs::RenameFile(previous, target));
            R_TRY(rc);
            g_device_map_published_revision = g_device_map.revision;
            R_SUCCEED();
        }

        int AppendDeviceMapSummary(char *out, size_t size) {
            g_device_map_log_result = PublishDeviceMapSnapshot().GetValue();
            const auto id = g_device_map.attempt.words[0];
            return util::SNPrintf(out,size,
                "usb_device_map_log=atmosphere/logs/usb_device_map.txt\n"
                "usb_device_map_log_result=0x%08X\nusb_device_map_revision=%u\n"
                "usb_device_map_saved_revision=%u\nusb_device_map_limit=%u\nusb_device_map_limited=%u\n"
                "usb_device_map_identity=last_attempt_not_presence\n"
                "usb_device_vid=0x%04X\nusb_device_pid=0x%04X\nusb_device_identity_valid=%u\n"
                "usb_device_bus_id=0x%08X\nusb_device_device_id=0x%08X\nusb_device_physical_port=unknown\n",
                g_device_map_log_result,g_device_map.revision,g_device_map_published_revision,usb::mapping::Limit,g_device_map.limited,
                id>>16,id&65535,g_device_map.attempt_valid,g_device_map.attempt.words[11],g_device_map.attempt.words[12]);
        }

        Result PublishStatusSnapshot(const char *status, size_t written,
                                     const char *published_path = StatusPath,
                                     const char *temp_path = StatusTempPath) {
            R_UNLESS(status != nullptr && written > 0 && written < StatusBufferSize, usb::ResultInvalidParameter());
            static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory("/logs"));
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(temp_path));
            R_TRY(mitm::fs::CreateAtmosphereSdFile(temp_path, 0, ams::fs::CreateOption_None));

            {
                FsFile file{};
                R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), temp_path, ams::fs::OpenMode_ReadWrite));
                ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
                R_TRY(fsFileSetSize(std::addressof(file), static_cast<s64>(written)));
                R_TRY(fsFileWrite(std::addressof(file), 0, status, written, FsWriteOption_Flush));
            }

            /* Close the temporary file before touching the public path. If a
             * reader currently has the public snapshot open, leave the last
             * complete snapshot in place and retry on the next poll. */
            char old_path[ams::fs::EntryNameLengthMax + 1]{};
            char new_path[ams::fs::EntryNameLengthMax + 1]{};
            util::SNPrintf(old_path, sizeof(old_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, temp_path);
            util::SNPrintf(new_path, sizeof(new_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, published_path);
            static_cast<void>(ams::fs::DeleteFile(new_path));
            R_RETURN(ams::fs::RenameFile(old_path, new_path));
        }

        // Only the admitted monitor writes physical SD. A failed prepare can
        // prevent a wake job from starting, but still needs a durable report.
        void PublishLastWake() {
            static u32 saved{};
            static char report[4096];
            const u32 revision = system_power::ReportRevision();
            if (!revision || revision == saved) return;
            const int size = system_power::Format(report, sizeof(report));
            if (size > 0 && static_cast<size_t>(size) < sizeof(report) &&
                R_SUCCEEDED(PublishStatusSnapshot(report, size, "/logs/usb_sleep_last_wake.txt",
                                                   "/logs/usb_sleep_last_wake.txt.tmp"))) {
                saved = revision;
            }
        }

        // This failure snapshot uses only physical SD and atomic counters.
        // The monitor admits it only after a successful known-awake PSC ACK,
        // even when USB remains quarantined after failed resume validation.
        Result PublishSystemPowerFailure() {
            static char status[4096]{};
            auto written = util::SNPrintf(status, sizeof(status),
                "enabled=1\nready=0\nrunning=0\nstate=Error\nbuild_id=%s\n"
                "usb_disabled_reason=system_power_coordination_failed\n", BuildId);
            R_UNLESS(written > 0 && static_cast<size_t>(written) < sizeof(status), usb::ResultInvalidParameter());
            const int power_written = system_power::Format(status + written, sizeof(status) - written);
            R_UNLESS(power_written > 0 && static_cast<size_t>(power_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += power_written;
            R_RETURN(PublishStatusSnapshot(status, static_cast<size_t>(written)));
        }

        Result PublishLastReadyStatusSnapshot(const char *status, size_t written) {
            R_UNLESS(status != nullptr && written > 0 && written < StatusBufferSize, usb::ResultInvalidParameter());
            static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory("/logs"));
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(LastReadyStatusTempPath));
            R_TRY(mitm::fs::CreateAtmosphereSdFile(LastReadyStatusTempPath, 0, ams::fs::CreateOption_None));

            {
                FsFile file{};
                R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), LastReadyStatusTempPath, ams::fs::OpenMode_ReadWrite));
                ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
                R_TRY(fsFileSetSize(std::addressof(file), static_cast<s64>(written)));
                R_TRY(fsFileWrite(std::addressof(file), 0, status, written, FsWriteOption_Flush));
            }

            char old_path[ams::fs::EntryNameLengthMax + 1]{};
            char new_path[ams::fs::EntryNameLengthMax + 1]{};
            util::SNPrintf(old_path, sizeof(old_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, LastReadyStatusTempPath);
            util::SNPrintf(new_path, sizeof(new_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, LastReadyStatusPath);
            static_cast<void>(ams::fs::DeleteFile(new_path));
            static_cast<void>(ams::fs::RenameFile(old_path, new_path));
            R_SUCCEED();
        }

        Result PublishLoaderTraceSnapshot() {
            /* Only LoaderTracePublisherThreadFunction calls this. It never
             * takes g_status_mutex or waits for the NCM/status/cache worker.
             * Capture/format under the trace lock; never hold it for FS IPC. */
            size_t written = 0;
            static impl::TraceFileSystem trace_file_system;
            static memory_diagnostics::History memory_history;
            static u64 memory_published_sequence = 0;
            u64 snapshot_sequence = 0;
            u64 snapshot_progress_revision = 0;
            u64 snapshot_fs_revision = 0;
            const s64 snapshot_ms = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
            const auto capture_progress = hang_capture::GetProgress();
            VirtualSdDiagnostics vsd{};
            const bool vsd_valid=GetVirtualSdDiagnostics(&vsd);
            // Only read-only, already-permitted SVCs. Sample before taking the
            // trace lock; no allocator walk, service IPC, new handle or USB I/O.
            memory_history.Poll(snapshot_ms, [](size_t index, bool used) {
                memory_diagnostics::Value value{};
                if (index < 2) {
                    const auto type = index == 0
                        ? (used ? InfoType_UsedMemorySize : InfoType_TotalMemorySize)
                        : (used ? InfoType_SystemResourceSizeUsed : InfoType_SystemResourceSizeTotal);
                    value.result = ::svcGetInfo(&value.bytes, type, CUR_PROCESS_HANDLE, 0);
                } else if (index < memory_diagnostics::PairCount) {
                    value.result = ::svcGetSystemInfo(&value.bytes, used ? SystemInfoType_UsedPhysicalMemorySize
                        : SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, index - 2);
                } else {
                    value.result = ::svcGetInfo(&value.bytes, InfoType_FreeThreadCount, CUR_PROCESS_HANDLE, 0);
                }
                if (!value.Valid()) value.bytes = 0;
                return value;
            });
            const auto &memory = memory_history.Latest();
            const bool memory_pending = memory.sequence != memory_published_sequence;
            const auto comparison_revision = g_comparison_revision.load(std::memory_order_acquire);
            static u32 last_comparison_revision{}; // Sole trace publisher.
            {
            std::scoped_lock lk(g_loader_trace_mutex);

            const bool pending_refresh = (capture_progress.setup_result == 0 || g_applet_forward_progress.Pending()
                || g_fs_forward_progress.Pending() || g_monitor_progress.Pending())
                && snapshot_ms - g_applet_forward_last_published_ms >= 5000;
            if (g_loader_trace_next_sequence == g_loader_trace_last_published_sequence
                    && g_applet_forward_progress.revision == g_applet_forward_last_published_revision
                    && g_fs_forward_progress.revision == g_fs_forward_last_published_revision
                    && !pending_refresh && !memory_pending && comparison_revision == last_comparison_revision) {
                R_SUCCEED();
            }
            snapshot_sequence = g_loader_trace_next_sequence;
            snapshot_progress_revision = g_applet_forward_progress.revision;
            snapshot_fs_revision = g_fs_forward_progress.revision;

            bool truncated = false;
            const u32 publish_count = std::min<u32>(g_loader_trace_count, MaxPublishedLoaderTraceRecords);
            const u64 first_available_sequence = g_loader_trace_next_sequence - g_loader_trace_count + 1;
            const u32 skip_count = g_loader_trace_count - publish_count;
            const u64 first_published_sequence = first_available_sequence + skip_count;
            const u32 omitted_count = g_loader_trace_count - publish_count;
            const u64 overwritten_count = g_loader_trace_next_sequence > MaxLoaderTraceRecords
                ? g_loader_trace_next_sequence - MaxLoaderTraceRecords : 0;
            const auto header_written = util::SNPrintf(g_loader_trace_buffer, sizeof(g_loader_trace_buffer),
                "build_id=%s\n"
                "record_count=%u\n"
                "published_record_count=%u\n"
                "ring_capacity=%zu\n"
                "ring_first_sequence=%llu\n"
                "first_published_sequence=%llu\n"
                "overwritten_before_ring=%llu\n"
                "omitted_by_text_limit=%u\n"
                "trace_selection=full_chronological_ring\n"
                "ns_pre_pgl_patch_recorded=%u\n"
                "ns_pre_pgl_patch_event=%llu\n"
                "ns_pre_pgl_patch_info=%s\n"
                "last_sequence=%llu\n",
                BuildId,
                g_loader_trace_count,
                publish_count,
                MaxLoaderTraceRecords,
                static_cast<unsigned long long>(first_available_sequence),
                static_cast<unsigned long long>(first_published_sequence),
                static_cast<unsigned long long>(overwritten_count),
                omitted_count,
                g_ns_pre_pgl_patch_diagnostic_valid ? 1u : 0u,
                static_cast<unsigned long long>(g_ns_pre_pgl_patch_diagnostic_record.sequence),
                g_ns_pre_pgl_patch_diagnostic_valid ? g_ns_pre_pgl_patch_diagnostic_record.info.path : "unseen",
                static_cast<unsigned long long>(g_loader_trace_next_sequence));
            R_UNLESS(header_written > 0 && static_cast<size_t>(header_written) < sizeof(g_loader_trace_buffer), usb::ResultInvalidParameter());
            written = static_cast<size_t>(header_written);

            const auto vsd_written=util::SNPrintf(g_loader_trace_buffer+written,sizeof(g_loader_trace_buffer)-written,
                "vsd_cleanup=C26\nvsd_snapshot_valid=%u\nvsd_reaper_running=%u\nvsd_init_result=0x%08X\n"
                "vsd_interval_ms=250\nvsd_watched=%u\nvsd_retained_total=%llu\nvsd_detached_total=%llu\n"
                "vsd_closed_total=%llu\nvsd_polls=%llu\nvsd_last_exit_pid=%llu\nvsd_last_exit_ms=%llu\n"
                "vsd_query_errors=%u\nvsd_last_query_error=0x%08X\n",
                vsd_valid?1u:0u,vsd.running?1u:0u,vsd.init_result,vsd.watched,
                static_cast<unsigned long long>(vsd.retained),static_cast<unsigned long long>(vsd.detached),
                static_cast<unsigned long long>(vsd.closed),static_cast<unsigned long long>(vsd.polls),
                static_cast<unsigned long long>(vsd.last_pid),static_cast<unsigned long long>(vsd.last_ms),
                vsd.query_errors,vsd.last_query_error);
            R_UNLESS(vsd_written>0 && static_cast<size_t>(vsd_written)<sizeof(g_loader_trace_buffer)-written,usb::ResultInvalidParameter());
            written+=static_cast<size_t>(vsd_written);

            const auto memory_header = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                "memory_schema=1\nmemory_patch=ams-1.12.1-selective-C22\n"
                "memory_interval_ms=5000\nmemory_ring_capacity=16\nmemory_sample_ms=%lld\n"
                "memory_sequence=%llu\nmemory_self_used_highwater_bytes=%llu\n"
                "memory_free_threads=%llu\nmemory_free_threads_result=0x%08X\n"
                "memory_heap_arena=not_sampled_no_allocator_lock\n"
                "memory_scope=self_and_physical_pools_not_app_heap_or_leak_proof\n",
                static_cast<long long>(memory.at_ms), static_cast<unsigned long long>(memory.sequence),
                static_cast<unsigned long long>(memory_history.SelfPeak()),
                static_cast<unsigned long long>(memory.free_threads.bytes), memory.free_threads.result);
            R_UNLESS(memory_header > 0 && static_cast<size_t>(memory_header) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
            written += static_cast<size_t>(memory_header);
            constexpr const char *MemoryNames[] = {"self_physical", "self_page_tables", "application_pool", "applet_pool", "system_pool", "system_unsafe_pool"};
            static_assert(util::size(MemoryNames) == memory_diagnostics::PairCount);
            for (size_t i = 0; i < memory_diagnostics::PairCount; ++i) {
                const auto &p = memory.pairs[i];
                const auto line = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                    "memory name=%s valid=%u total=%llu used=%llu free=%llu total_result=0x%08X used_result=0x%08X low_free_valid=%u low_free=%llu\n",
                    MemoryNames[i], p.Valid() ? 1u : 0u, static_cast<unsigned long long>(p.total.bytes),
                    static_cast<unsigned long long>(p.used.bytes), static_cast<unsigned long long>(p.Free()),
                    p.total.result, p.used.result, memory_history.MinimumValid(i) ? 1u : 0u,
                    static_cast<unsigned long long>(memory_history.MinimumFree(i)));
                R_UNLESS(line > 0 && static_cast<size_t>(line) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
                written += static_cast<size_t>(line);
            }
            for (size_t i = 0; i < memory_history.Count(); ++i) {
                const auto &s = memory_history.At(i);
                u32 valid_mask = 0;
                for (size_t j = 0; j < memory_diagnostics::PairCount; ++j) if (s.pairs[j].Valid()) valid_mask |= 1u << j;
                const auto line = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                    "memory_history seq=%llu ms=%lld valid_mask=0x%02X self_used=%llu applet_free=%llu system_free=%llu unsafe_free=%llu\n",
                    static_cast<unsigned long long>(s.sequence), static_cast<long long>(s.at_ms), valid_mask,
                    static_cast<unsigned long long>(s.pairs[0].used.bytes), static_cast<unsigned long long>(s.pairs[3].Free()),
                    static_cast<unsigned long long>(s.pairs[4].Free()), static_cast<unsigned long long>(s.pairs[5].Free()));
                R_UNLESS(line > 0 && static_cast<size_t>(line) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
                written += static_cast<size_t>(line);
            }

            const auto &progress = g_applet_forward_progress;
            const auto progress_written = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                "applet_forward_revision=%llu\n"
                "applet_forward_call=%llu\n"
                "applet_forward_phase=%s\n"
                "applet_forward_phase_age_ms=%lld\n"
                "applet_forward_sample_ms=%lld\n"
                "applet_forward_program_id=%016llX\n"
                "applet_forward_session=%llX\n"
                "applet_forward_decoded=%u\n"
                "applet_forward_command=%u\n"
                "applet_forward_object=%08X\n"
                "applet_forward_input_size=%u\n"
                "applet_forward_input0=%016llX\n"
                "applet_forward_native_result_known=%u\n"
                "applet_forward_native_result=0x%08X\n",
                static_cast<unsigned long long>(progress.revision),
                static_cast<unsigned long long>(progress.call), progress.PhaseName(),
                static_cast<long long>(progress.revision != 0 ? snapshot_ms - progress.changed_ms : 0),
                static_cast<long long>(snapshot_ms),
                static_cast<unsigned long long>(progress.program),
                static_cast<unsigned long long>(progress.session), progress.decoded ? 1u : 0u,
                progress.command, progress.object, progress.input_size,
                static_cast<unsigned long long>(progress.input0), progress.native_result_known ? 1u : 0u,
                progress.native_result);
            R_UNLESS(progress_written > 0 && static_cast<size_t>(progress_written) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
            written += static_cast<size_t>(progress_written);

            // Sample game-facing IPC here as well: this publisher keeps running
            // independently if the ordinary storage status worker is waiting.
            const size_t game_capacity = sizeof(g_loader_trace_buffer) - written;
            const int game_written = game_wake::Format(g_loader_trace_buffer + written, game_capacity);
            R_UNLESS(game_written > 0 && static_cast<size_t>(game_written) < game_capacity, usb::ResultInvalidParameter());
            written += static_cast<size_t>(game_written);
            const size_t compare_capacity = sizeof(g_loader_trace_buffer) - written;
            const int compare_written = wake_compare::Format(g_loader_trace_buffer + written, compare_capacity);
            R_UNLESS(compare_written > 0 && static_cast<size_t>(compare_written) < compare_capacity, usb::ResultInvalidParameter());
            written += static_cast<size_t>(compare_written);

            const auto monitor_written = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                "trace_publisher=independent\ntrace_filesystem=dedicated_non_domain\n"
                "hang_capture_mode=disabled_r126_kernel_recorder\nhang_capture_worker_sampling=disabled\nhang_capture_r125_state=%u\nhang_capture_r125_setup_result=0x%08X\n"
                "hang_capture_supervisor_phase=%u\nhang_capture_supervisor_cycles=%llu\nhang_capture_supervisor_beat_ms=%llu\n"
                "hang_capture_selection_ms=%llu\nhang_capture_start_ms=%llu\nhang_capture_deadline_ms=%llu\ntrace_publisher_cycles=%llu\n"
                "trace_publisher_failures=%llu\ntrace_publisher_previous_result=0x%08X\n"
                "monitor_revision=%llu\nmonitor_phase=%s\nmonitor_phase_age_ms=%lld\n"
                "monitor_status_result_known=%u\nmonitor_last_status_result=0x%08X\n",
                capture_progress.state, capture_progress.setup_result, capture_progress.phase,
                static_cast<unsigned long long>(capture_progress.cycles), static_cast<unsigned long long>(capture_progress.beat_ms),
                static_cast<unsigned long long>(capture_progress.selected_ms), static_cast<unsigned long long>(capture_progress.started_ms),
                static_cast<unsigned long long>(capture_progress.selected_ms ? capture_progress.selected_ms + hang_capture::SelectionDelayMs : 0),
                static_cast<unsigned long long>(g_trace_publisher_cycles),
                static_cast<unsigned long long>(g_trace_publisher_failures), g_trace_publisher_last_result,
                static_cast<unsigned long long>(g_monitor_progress.revision), g_monitor_progress.PhaseName(),
                static_cast<long long>(g_monitor_progress.revision != 0 ? snapshot_ms - g_monitor_progress.changed_ms : 0),
                g_monitor_progress.status_result_known ? 1u : 0u, g_monitor_progress.last_status_result);
            R_UNLESS(monitor_written > 0 && static_cast<size_t>(monitor_written) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
            written += static_cast<size_t>(monitor_written);

            const auto fs_header_written = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                "fs_forward_revision=%llu\nfs_forward_calls=%llu\nfs_forward_tracking_drops=%llu\n"
                "fs_forward_success_details_suppressed=%llu\nfs_forward_failure_details_suppressed=%llu\n",
                static_cast<unsigned long long>(g_fs_forward_progress.revision),
                static_cast<unsigned long long>(g_fs_forward_progress.calls),
                static_cast<unsigned long long>(g_fs_forward_progress.tracking_drops),
                static_cast<unsigned long long>(g_fs_forward_progress.success_details_suppressed),
                static_cast<unsigned long long>(g_fs_forward_progress.failure_details_suppressed));
            R_UNLESS(fs_header_written > 0 && static_cast<size_t>(fs_header_written) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
            written += static_cast<size_t>(fs_header_written);
            for (const auto &entry : g_fs_forward_progress.entries) {
                if (!entry.used) continue;
                const auto &fs_progress = entry.progress;
                const auto fs_written = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                    "fs_forward client=%016llX program_id=%016llX call=%llu session=%llX phase=%s age_ms=%lld "
                    "decoded=%u command=%u object=%08X input_size=%u input0=%016llX "
                    "native_known=%u native_result=0x%08X cmif_known=%u cmif_result=0x%08X\n",
                    static_cast<unsigned long long>(entry.client),
                    static_cast<unsigned long long>(fs_progress.program),
                    static_cast<unsigned long long>(fs_progress.call),
                    static_cast<unsigned long long>(fs_progress.session), fs_progress.PhaseName(),
                    static_cast<long long>(snapshot_ms - fs_progress.changed_ms), fs_progress.decoded ? 1u : 0u,
                    fs_progress.command, fs_progress.object, fs_progress.input_size,
                    static_cast<unsigned long long>(fs_progress.input0), fs_progress.native_result_known ? 1u : 0u,
                    fs_progress.native_result, entry.cmif_known ? 1u : 0u, entry.cmif_result);
                R_UNLESS(fs_written > 0 && static_cast<size_t>(fs_written) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
                written += static_cast<size_t>(fs_written);
            }

            u32 emitted_count = 0;
            for (u32 i = skip_count; i < g_loader_trace_count && emitted_count < publish_count; ++i) {
                const u64 sequence = first_available_sequence + i;
                const auto &record = g_loader_trace_records[(sequence - 1) % MaxLoaderTraceRecords];
                char content_id[ams::util::Uuid::StringSize]{};
                ams::util::Uuid uuid{};
                std::memcpy(uuid.data, record.info.content_id, sizeof(uuid.data));
                uuid.ToString(content_id, sizeof(content_id));

                const auto line_written = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written,
                    "event=%llu stage=%u result=0x%08X program_id=%016llX content_id=%s location_storage=%u metadata_storage=%u content_storage=%u media_generation=%llu flags=0x%08X path=%s\n",
                    static_cast<unsigned long long>(record.sequence),
                    record.info.stage,
                    record.info.result,
                    static_cast<unsigned long long>(record.info.program_id),
                    content_id,
                    record.info.location_storage,
                    record.info.metadata_storage,
                    record.info.content_storage,
                    static_cast<unsigned long long>(record.info.media_generation),
                    record.info.flags,
                    record.info.path);
                if (line_written <= 0 || static_cast<size_t>(line_written) >= sizeof(g_loader_trace_buffer) - written) {
                    truncated = true;
                    break;
                }
                written += static_cast<size_t>(line_written);
                ++emitted_count;
            }

            if (truncated) {
                const auto marker_written = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written, "trace_truncated=1\n");
                if (marker_written > 0 && static_cast<size_t>(marker_written) < sizeof(g_loader_trace_buffer) - written) {
                    written += static_cast<size_t>(marker_written);
                }
            } else {
                const auto marker_written = util::SNPrintf(g_loader_trace_buffer + written, sizeof(g_loader_trace_buffer) - written, "trace_truncated=0\n");
                R_UNLESS(marker_written > 0 && static_cast<size_t>(marker_written) < sizeof(g_loader_trace_buffer) - written, usb::ResultInvalidParameter());
                written += static_cast<size_t>(marker_written);
            }
            } // Release g_loader_trace_mutex before any filesystem operation.

            /* All operations, including rename, use this writer's private
             * native FS connection rather than the process-wide session pool. */
            R_TRY(trace_file_system.Publish(g_loader_trace_buffer, written));
            memory_published_sequence = memory.sequence;
            {
                std::scoped_lock lk(g_loader_trace_mutex);
                /* A callback may append while disk I/O is in progress.
                 * Acknowledge only the snapshot actually written. */
                g_loader_trace_last_published_sequence = snapshot_sequence;
                g_applet_forward_last_published_revision = snapshot_progress_revision;
                g_fs_forward_last_published_revision = snapshot_fs_revision;
                last_comparison_revision = comparison_revision;
                g_applet_forward_last_published_ms = snapshot_ms;
            }
            R_SUCCEED();
        }

        const char *GetNcmLookupOperationName(u32 operation) {
            switch (static_cast<ncm_trace::LookupOperation>(operation)) {
                case ncm_trace::LookupOperation::MetadataContentInfo: return "metadata_content_info";
                case ncm_trace::LookupOperation::MetadataLatestKey:   return "metadata_latest_key";
                case ncm_trace::LookupOperation::OverlayHas:          return "overlay_has";
                case ncm_trace::LookupOperation::OverlayPath:         return "overlay_path";
                case ncm_trace::LookupOperation::NativeHas:           return "native_has";
                case ncm_trace::LookupOperation::NativePath:          return "native_path";
                case ncm_trace::LookupOperation::OpenStorage:         return "open_storage";
                case ncm_trace::LookupOperation::OpenMetadata:        return "open_metadata";
                case ncm_trace::LookupOperation::OverlaySize:         return "overlay_size";
                case ncm_trace::LookupOperation::OverlayRead:         return "overlay_read";
                case ncm_trace::LookupOperation::OverlayRights:       return "overlay_rights";
                case ncm_trace::LookupOperation::OverlayProgramId:    return "overlay_program_id";
                case ncm_trace::LookupOperation::OverlayCount:        return "overlay_count";
                case ncm_trace::LookupOperation::OverlayList:         return "overlay_list";
                default:                                              return "unknown";
            }
        }

        u32 GetNcmLookupTraceStage(u32 operation) {
            /* Stages 124-131 are the original R78 NCM operations. Stage 132
             * is the snapshot summary and stage 133 is the HOC/NS patch
             * proof, so extended operations start at 134. */
            if (operation >= 1 && operation <= 8) {
                return 123 + operation;
            }
            if (operation >= 9 && operation <= 14) {
                return 125 + operation;
            }
            return 134;
        }

        void FormatNcmContentId(char *out, size_t out_size, const u8 *content_id) {
            constexpr char Hex[] = "0123456789abcdef";
            if (out == nullptr || out_size < 33 || content_id == nullptr) {
                return;
            }
            for (size_t i = 0; i < 16; ++i) {
                out[2 * i] = Hex[content_id[i] >> 4];
                out[2 * i + 1] = Hex[content_id[i] & 0x0F];
            }
            out[32] = '\0';
        }

        Result PublishNcmLaunchTraceSnapshot() {
            std::scoped_lock lk(g_ncm_launch_trace_mutex);
            if (!g_ncm_launch_trace.valid
                    || g_ncm_launch_trace.generation == g_ncm_launch_trace_last_published_generation) {
                R_SUCCEED();
            }

            const auto &state = g_ncm_launch_trace;
            const auto &snapshot = state.snapshot;
            const bool capture_valid = ncm_trace::HasValidShape(snapshot)
                && snapshot.session == state.session
                && snapshot.title_id == state.application_id.value;
            const u32 record_count = capture_valid ? snapshot.count : 0;

            const Result begin_result = static_cast<Result>(state.begin_result);
            const Result end_result = static_cast<Result>(state.end_result);
            const Result launch_result = static_cast<Result>(state.launch_result);
            const int header_written = util::SNPrintf(g_ncm_launch_trace_buffer, sizeof(g_ncm_launch_trace_buffer),
                "build_id=%s\n"
                "application_id=%016llX\n"
                "session=%llu\n"
                "generation=%llu\n"
                "begin_result=0x%08X\n"
                "begin_module=%u\n"
                "begin_description=%u\n"
                "end_result=0x%08X\n"
                "end_module=%u\n"
                "end_description=%u\n"
                "launch_result=0x%08X\n"
                "launch_module=%u\n"
                "launch_description=%u\n"
                "snapshot_valid=%u\n"
                "snapshot_magic=0x%08X\n"
                "snapshot_version=%u\n"
                "snapshot_record_size=%u\n"
                "snapshot_title_id=%016llX\n"
                "snapshot_flags=0x%08X\n"
                "record_count=%u\n"
                "dropped_record_count=%u\n",
                BuildId,
                static_cast<unsigned long long>(state.application_id.value),
                static_cast<unsigned long long>(state.session),
                static_cast<unsigned long long>(state.generation),
                state.begin_result, begin_result.GetModule(), begin_result.GetDescription(),
                state.end_result, end_result.GetModule(), end_result.GetDescription(),
                state.launch_result, launch_result.GetModule(), launch_result.GetDescription(),
                capture_valid ? 1u : 0u,
                snapshot.magic,
                snapshot.version,
                snapshot.record_size,
                static_cast<unsigned long long>(snapshot.title_id),
                snapshot.flags,
                record_count,
                capture_valid ? snapshot.dropped : 0u);
            R_UNLESS(header_written > 0 && static_cast<size_t>(header_written) < sizeof(g_ncm_launch_trace_buffer), usb::ResultInvalidParameter());
            size_t written = static_cast<size_t>(header_written);

            for (u32 i = 0; i < record_count; ++i) {
                const auto &record = snapshot.records[i];
                const Result record_result = static_cast<Result>(record.result);
                char content_id[33]{};
                char detail[sizeof(record.detail) + 1]{};
                FormatNcmContentId(content_id, sizeof(content_id), record.content_id);
                std::memcpy(detail, record.detail, sizeof(record.detail));

                const int line_written = util::SNPrintf(g_ncm_launch_trace_buffer + written,
                    sizeof(g_ncm_launch_trace_buffer) - written,
                    "record=%llu operation=%u operation_name=%s result=0x%08X module=%u description=%u title_id=%016llX metadata_version=%u storage_id=%u content_type=%u id_offset=%u flags=0x%02X content_id=%s detail=%s\n",
                    static_cast<unsigned long long>(record.sequence),
                    record.operation,
                    GetNcmLookupOperationName(record.operation),
                    record.result,
                    record_result.GetModule(),
                    record_result.GetDescription(),
                    static_cast<unsigned long long>(record.title_id),
                    record.metadata_version,
                    record.storage_id,
                    record.content_type,
                    record.id_offset,
                    record.flags,
                    content_id,
                    detail);
                R_UNLESS(line_written > 0 && static_cast<size_t>(line_written) < sizeof(g_ncm_launch_trace_buffer) - written, usb::ResultInvalidParameter());
                written += static_cast<size_t>(line_written);
            }

            static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory("/logs"));
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(NcmLaunchTraceTempPath));
            R_TRY(mitm::fs::CreateAtmosphereSdFile(NcmLaunchTraceTempPath, 0, ams::fs::CreateOption_None));

            {
                FsFile file{};
                R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), NcmLaunchTraceTempPath, ams::fs::OpenMode_ReadWrite));
                ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
                R_TRY(fsFileSetSize(std::addressof(file), static_cast<s64>(written)));
                R_TRY(fsFileWrite(std::addressof(file), 0, g_ncm_launch_trace_buffer, written, FsWriteOption_Flush));
            }

            char old_path[ams::fs::EntryNameLengthMax + 1]{};
            char new_path[ams::fs::EntryNameLengthMax + 1]{};
            util::SNPrintf(old_path, sizeof(old_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, NcmLaunchTraceTempPath);
            util::SNPrintf(new_path, sizeof(new_path), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, NcmLaunchTracePath);
            static_cast<void>(ams::fs::DeleteFile(new_path));
            R_TRY(ams::fs::RenameFile(old_path, new_path));
            g_ncm_launch_trace_last_published_generation = state.generation;
            R_SUCCEED();
        }

        Result PublishContentSettingBridge(bool enabled) {
            const char *value = enabled ? "1\n" : "0\n";
            constexpr size_t value_size = 2;

            static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory("/logs"));
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(ContentSettingBridgePath));
            R_TRY(mitm::fs::CreateAtmosphereSdFile(ContentSettingBridgePath, 0, ams::fs::CreateOption_None));

            FsFile file{};
            R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), ContentSettingBridgePath, ams::fs::OpenMode_ReadWrite));
            ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
            R_TRY(fsFileSetSize(std::addressof(file), value_size));
            R_TRY(fsFileWrite(std::addressof(file), 0, value, value_size, FsWriteOption_Flush));
            R_SUCCEED();
        }

        bool HasContentStatusRefreshCommand() {
            FsFile file{};
            if (R_FAILED(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), ContentStatusRefreshCommandPath, ams::fs::OpenMode_Read))) {
                return false;
            }
            static_cast<void>(fsFileClose(std::addressof(file)));
            return true;
        }

        void CopyNotificationField(char *dst, size_t dst_size, const char *src) {
            if (dst_size == 0) {
                return;
            }

            size_t copied = 0;
            if (src != nullptr) {
                while (copied + 1 < dst_size && src[copied] != '\0') {
                    const auto c = static_cast<unsigned char>(src[copied]);
                    /* Keep notification JSON ASCII-safe and escape-free. */
                    dst[copied] = (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') ? static_cast<char>(c) : '_';
                    ++copied;
                }
            }
            dst[copied] = '\0';
        }

        void WriteUltrahandNotification(const char *title, const char *text) {
            /* Ultrahand consumes API notifications from SD-root
             * /config/ultrahand/notifications. The AtmosphereSd helpers add an
             * /atmosphere prefix and therefore write to a directory Ultrahand
             * never watches. Use the raw SD helpers for this one integration. */
            static_cast<void>(mitm::fs::CreateSdDirectory("/config"));
            static_cast<void>(mitm::fs::CreateSdDirectory("/config/ultrahand"));
            static_cast<void>(mitm::fs::CreateSdDirectory(UltrahandNotificationDirectory));

            char notification[NotificationBufferSize]{};
            const auto written = util::SNPrintf(notification, sizeof(notification),
                "{\n"
                "  \"text\": \"%s\",\n"
                "  \"title\": \"%s\",\n"
                "  \"duration\": 5000,\n"
                "  \"show_time\": \"true\",\n"
                "  \"priority\": 20\n"
                "}\n",
                text,
                title);
            if (written <= 0 || static_cast<size_t>(written) >= sizeof(notification)) {
                return;
            }

            char path[ams::fs::EntryNameLengthMax + 1]{};
            util::SNPrintf(path, sizeof(path), "%s/ams-usb-%u.notify", UltrahandNotificationDirectory, g_notification_sequence++);

            FsFile file{};
            if (R_FAILED(mitm::fs::OpenSdFile(std::addressof(file), path, ams::fs::OpenMode_ReadWrite))) {
                if (R_FAILED(mitm::fs::CreateSdFile(path, 0, ams::fs::CreateOption_None)) ||
                    R_FAILED(mitm::fs::OpenSdFile(std::addressof(file), path, ams::fs::OpenMode_ReadWrite))) {
                    return;
                }
            }
            ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
            if (R_FAILED(fsFileSetSize(std::addressof(file), static_cast<s64>(written)))) {
                return;
            }
            static_cast<void>(fsFileWrite(std::addressof(file), 0, notification, static_cast<size_t>(written), FsWriteOption_Flush));
        }

        void SetContentRouteAppliedState(bool enabled) {
            g_content_route_applied_enabled.store(enabled, std::memory_order_release);
            g_content_install_target_enabled.store(enabled, std::memory_order_release);
            g_content_launch_path_enabled.store(enabled, std::memory_order_release);
            g_content_route_applied_once.store(true, std::memory_order_release);
        }

        Result ApplyContentRouteState(bool enabled) {
            // P4: after Horizon initialization, arm NCM's read classification
            // even when booting with no USB attached. false still means writes
            // go to SD; it must not skip the first post-boot notification.
            if (g_content_route_applied_once.load(std::memory_order_acquire)
                && g_content_route_applied_enabled.load(std::memory_order_acquire) == enabled) {
                R_SUCCEED();
            }

            const Result result = ncm::SetUsbContentRouteEnabled(enabled);
            if (R_FAILED(result)) {
                g_last_module_result = result.GetValue();
                R_RETURN(result);
            }

            SetContentRouteAppliedState(enabled);
            R_SUCCEED();
        }

        void ReconcileAutomaticContentRoute(const ncm::UsbContentStatus &status) {
            /* USB is an automatic target, not a sticky permission. Only a
             * mounted, ready and writable medium may own logical-SD content
             * operations. A disconnected, mounting, failed or read-only
             * device leaves Horizon on its normal SD-card path. */
            const bool usb_target_available = status.mounted != 0
                && status.ready != 0
                && status.writable != 0;
            const bool desired_usb_route = g_content_route_auto_enabled && usb_target_available;
            static_cast<void>(ApplyContentRouteState(desired_usb_route));
        }

        void ApplyContentRouteCommand() {
            FsFile file{};
            if (R_FAILED(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), ContentRouteCommandPath, ams::fs::OpenMode_Read))) {
                return;
            }

            char command[16]{};
            u64 read_size = 0;
            const auto read_result = fsFileRead(std::addressof(file), 0, command, sizeof(command) - 1, FsReadOption_None, std::addressof(read_size));
            static_cast<void>(fsFileClose(std::addressof(file)));
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(ContentRouteCommandPath));
            if (R_FAILED(read_result) || read_size == 0) {
                return;
            }

            bool request_usb = false;
            bool request_sd = false;
            if (read_size >= 3 && std::memcmp(command, "usb", 3) == 0) {
                request_usb = true;
            } else if (read_size >= 2 && std::memcmp(command, "sd", 2) == 0) {
                request_sd = true;
            }
            if (!request_usb && !request_sd) {
                return;
            }

            if (request_sd) {
                g_content_route_auto_enabled = false;
                const Result result = ApplyContentRouteState(false);
                if (R_FAILED(result)) {
                    WriteUltrahandNotification("USB content storage", "SD route selection failed");
                    return;
                }
                WriteUltrahandNotification("USB content storage", "Install target: SD card");
                return;
            }

            g_content_route_auto_enabled = true;
            /* WriteContentStatus owns the status sample and will activate USB
             * during this monitor tick if the medium is genuinely usable. */
            WriteUltrahandNotification("USB content storage", "Automatic USB target enabled");
        }

        void ApplyContentStatusRefreshCommand() {
            if (!HasContentStatusRefreshCommand()) {
                return;
            }

            /* The overlay command is intentionally a harmless acknowledgement
             * now. Starting NCM's USB worker from an asynchronous monitor IPC
             * can freeze Horizon while usb:hs is being acquired. Real content
             * placement owns the guarded startup path; status polling must not
             * probe or mount the drive. */
            static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(ContentStatusRefreshCommandPath));
            WriteUltrahandNotification("USB content storage", "Manual USB probe disabled for safety");
        }

        void UpdateUltrahandNotificationState(const ams::usb::storage::Status &storage_status, const ams::usb::filesystem::Status &filesystem_status) {
            const bool mounted = filesystem_status.mounted;
            const bool ready = filesystem_status.ready;
            const u64 generation = storage_status.media_generation;
            const u8 state = static_cast<u8>(storage_status.state);

            if (!g_notification_state_initialized) {
                g_notification_state_initialized = true;
                g_notification_last_mounted = mounted;
                g_notification_last_ready = ready;
                g_notification_last_generation = generation;
                g_notification_last_state = state;
                g_notification_last_result = storage_status.last_result;
                CopyNotificationField(g_notification_last_partition_name, sizeof(g_notification_last_partition_name), storage_status.partition_name);
                CopyNotificationField(g_notification_last_vendor, sizeof(g_notification_last_vendor), storage_status.vendor);
                CopyNotificationField(g_notification_last_product, sizeof(g_notification_last_product), storage_status.product);

                /* The monitor can already be mounted before the first status
                 * refresh. Emit the initial event so Ultrahand does not miss
                 * the service-start transition. */
                if (mounted) {
                    char text[192]{};
                    if (storage_status.state == ams::usb::storage::State::Error) {
                        util::SNPrintf(text, sizeof(text), "USB startup failed: 0x%08X", storage_status.last_result);
                    } else if (ready) {
                        util::SNPrintf(text, sizeof(text), "Connected: USB storage (%s)", GetFilesystemName(storage_status.filesystem_type));
                    } else {
                        util::SNPrintf(text, sizeof(text), "USB homebrew service ready; waiting for a drive");
                    }
                    char safe_text[sizeof(text)]{};
                    CopyNotificationField(safe_text, sizeof(safe_text), text);
                    WriteUltrahandNotification("USB storage", safe_text);
                }
                return;
            }

            const bool service_started = mounted && !g_notification_last_mounted;
            const bool connected = ready && !g_notification_last_ready;
            const bool disconnected = !ready && g_notification_last_ready;
            const bool media_changed = ready && g_notification_last_ready && generation != g_notification_last_generation;
            const bool startup_error = storage_status.state == ams::usb::storage::State::Error
                && (g_notification_last_state != state || g_notification_last_result != storage_status.last_result);

            if (service_started || connected || disconnected || media_changed || startup_error) {
                char device[96]{};
                const char *vendor = connected || media_changed ? storage_status.vendor : g_notification_last_vendor;
                const char *product = connected || media_changed ? storage_status.product : g_notification_last_product;
                const char *partition = connected || media_changed ? storage_status.partition_name : g_notification_last_partition_name;

                if (vendor[0] != '\0' || product[0] != '\0') {
                    util::SNPrintf(device, sizeof(device), "%s %s", vendor, product);
                } else if (partition[0] != '\0') {
                    util::SNPrintf(device, sizeof(device), "%s", partition);
                } else {
                    util::SNPrintf(device, sizeof(device), "USB storage");
                }

                char safe_device[sizeof(device)]{};
                CopyNotificationField(safe_device, sizeof(safe_device), device);

                char text[192]{};
                const char *title = "USB storage";
                if (startup_error) {
                    util::SNPrintf(text, sizeof(text), "USB startup failed: 0x%08X", storage_status.last_result);
                } else if (connected) {
                    util::SNPrintf(text, sizeof(text), "Connected: %s (%s)", safe_device, GetFilesystemName(storage_status.filesystem_type));
                } else if (disconnected) {
                    util::SNPrintf(text, sizeof(text), "Disconnected: %s", safe_device);
                } else {
                    util::SNPrintf(text, sizeof(text), "USB homebrew service ready; waiting for a drive");
                }

                char safe_text[sizeof(text)]{};
                CopyNotificationField(safe_text, sizeof(safe_text), text);
                WriteUltrahandNotification(title, safe_text);
            }

            g_notification_last_mounted = mounted;
            g_notification_last_ready = ready;
            g_notification_last_generation = generation;
            g_notification_last_state = state;
            g_notification_last_result = storage_status.last_result;
            CopyNotificationField(g_notification_last_partition_name, sizeof(g_notification_last_partition_name), storage_status.partition_name);
            CopyNotificationField(g_notification_last_vendor, sizeof(g_notification_last_vendor), storage_status.vendor);
            CopyNotificationField(g_notification_last_product, sizeof(g_notification_last_product), storage_status.product);
        }

        void UpdateContentUltrahandNotification(const ncm::UsbContentStatus &status) {
            const bool ready = status.ready != 0;
            if (!g_content_notification_state_initialized) {
                g_content_notification_state_initialized = true;
                g_content_notification_last_ready = ready;
                g_content_notification_last_result = status.last_result;
                g_content_notification_last_generation = status.media_generation;
                g_content_notification_last_filesystem = status.filesystem_type;
                g_content_notification_last_writable = status.writable != 0;
                CopyNotificationField(g_content_notification_last_partition_name, sizeof(g_content_notification_last_partition_name), status.partition_name);
                CopyNotificationField(g_content_notification_last_vendor, sizeof(g_content_notification_last_vendor), status.vendor);
                CopyNotificationField(g_content_notification_last_product, sizeof(g_content_notification_last_product), status.product);
                g_content_notification_last_free_space = status.free_space;
                g_content_notification_last_total_space = status.total_space;

                /* Do not show a false disconnect event during boot. If the
                 * drive is already ready, emit the initial connected event so
                 * the overlay still reflects the current state. */
                if (ready) {
                    char device[96]{};
                    char total[32]{};
                    char text[256]{};
                    GetContentDeviceName(device, sizeof(device), status.partition_name, status.vendor, status.product);
                    FormatCapacity(total, sizeof(total), status.total_space);
                    util::SNPrintf(text, sizeof(text), "Mounted: %s %s", device, total);
                    char safe_text[sizeof(text)]{};
                    CopyNotificationField(safe_text, sizeof(safe_text), text);
                    WriteUltrahandNotification("USB content storage", safe_text);
                }
                return;
            }

            const bool connected = ready && !g_content_notification_last_ready;
            const bool disconnected = !ready && g_content_notification_last_ready;
            const bool media_changed = ready && g_content_notification_last_ready
                && status.media_generation != g_content_notification_last_generation;
            if (connected || disconnected || media_changed) {
                char device[96]{};
                char total[32]{};
                char text[256]{};
                const char *partition = connected || media_changed ? status.partition_name : g_content_notification_last_partition_name;
                const char *vendor = connected || media_changed ? status.vendor : g_content_notification_last_vendor;
                const char *product = connected || media_changed ? status.product : g_content_notification_last_product;
                GetContentDeviceName(device, sizeof(device), partition, vendor, product);

                if (disconnected) {
                    FormatCapacity(total, sizeof(total), g_content_notification_last_total_space);
                    util::SNPrintf(text, sizeof(text), "Ejected: %s %s", device, total);
                } else {
                    FormatCapacity(total, sizeof(total), status.total_space);
                    util::SNPrintf(text, sizeof(text), "Mounted: %s %s", device, total);
                }

                char safe_text[sizeof(text)]{};
                CopyNotificationField(safe_text, sizeof(safe_text), text);
                WriteUltrahandNotification("USB content storage", safe_text);
            }

            g_content_notification_last_ready = ready;
            g_content_notification_last_result = status.last_result;
            g_content_notification_last_generation = status.media_generation;
            g_content_notification_last_filesystem = status.filesystem_type;
            g_content_notification_last_writable = status.writable != 0;
            CopyNotificationField(g_content_notification_last_partition_name, sizeof(g_content_notification_last_partition_name), status.partition_name);
            CopyNotificationField(g_content_notification_last_vendor, sizeof(g_content_notification_last_vendor), status.vendor);
            CopyNotificationField(g_content_notification_last_product, sizeof(g_content_notification_last_product), status.product);
            g_content_notification_last_free_space = status.free_space;
            g_content_notification_last_total_space = status.total_space;
        }

        Result WriteContentStatus() {
            std::scoped_lock lk(g_status_mutex);
            // Serialized by g_status_mutex. Keep the expanded descriptor log
            // off the monitor's 32-KiB stack, including nested filesystem calls.
            static char status[StatusBufferSize]{};

            /* v61 restores the known-good v16 behavior: after the monitor's
             * post-boot delay, the matching NCM status IPC starts the guarded
             * NCM-owned worker so the overlay can publish live drive info. */
            ncm::UsbContentStatus usb_status{};
            SetMonitorPhase(impl::MonitorProgress::StatusQuery);
            const Result status_query_result = ncm::GetUsbContentStatus(std::addressof(usb_status));
            if (R_FAILED(status_query_result)) {
                /* Keep a diagnostic snapshot even when the matching NCM
                 * status IPC is unavailable. Previously this error was
                 * discarded, leaving only usb_content_enabled.txt and making
                 * an ABI/service failure indistinguishable from no logging. */
                g_last_module_result = static_cast<Result>(status_query_result).GetValue();
                auto written = util::SNPrintf(status, sizeof(status),
                    "enabled=1\n"
                    "install_route=%u\n"
                    "route_policy_auto=%u\n"
                    "route_applied=%u\n"
                    "build_id=%s\n"
                    "mode=content\n"
                    "status_query_ok=0\n"
                    "status_query_result=0x%08X\n"
                    "module_result=0x%08X\n"
                    "loader_trace_path=%s\n"
                    "ncm_launch_trace_path=%s\n",
                    g_content_install_target_enabled.load(std::memory_order_acquire) ? 1u : 0u,
                    g_content_route_auto_enabled ? 1u : 0u,
                    g_content_route_applied_enabled.load(std::memory_order_acquire) ? 1u : 0u,
                    BuildId,
                    status_query_result.GetValue(),
                    g_last_module_result,
                    LoaderTracePath,
                    NcmLaunchTracePath);
                if (written > 0 && static_cast<size_t>(written) < sizeof(status)) {
                    const int saved = FormatStartupTimingAutosaveStatus(status + written, sizeof(status) - written);
                    if(saved > 0 && static_cast<size_t>(saved) < sizeof(status) - written)written += saved;
                    const int optimized = FormatUsbMetadataOptimizationStatus(status + written, sizeof(status) - written);
                    if(optimized > 0 && static_cast<size_t>(optimized) < sizeof(status) - written)written += optimized;
                    const int power = system_power::Format(status + written, sizeof(status) - written);
                    if(power > 0 && static_cast<size_t>(power) < sizeof(status) - written)written += power;
                    SetMonitorPhase(impl::MonitorProgress::StatusPublish);
                    static_cast<void>(PublishStatusSnapshot(status, static_cast<size_t>(written)));
                }
                R_RETURN(status_query_result);
            }
            const bool ready = usb_status.ready != 0;
            SetMonitorPhase(impl::MonitorProgress::RouteReconcile);
            ReconcileAutomaticContentRoute(usb_status);
            u32 loader_trace_records;
            u64 loader_trace_last_sequence;
            {
                std::scoped_lock trace_lk(g_loader_trace_mutex);
                loader_trace_records = g_loader_trace_count;
                loader_trace_last_sequence = g_loader_trace_next_sequence;
            }
            const u64 content_changes = content_stats::changes.load(std::memory_order_acquire);
            auto written = util::SNPrintf(status, sizeof(status),
                "enabled=%u\n"
                "install_route=%u\n"
                "route_policy_auto=%u\n"
                "route_applied=%u\n"
                "native_path_enabled=%u\n"
                "metadata_route_enabled=%u\n"
                "build_id=%s\n"
                "io_read_cache_slots=8\n"
                "io_same_drive_cache=S1\n"
                "io_performance=S2\n"
                "io_shared_transfer_max=4194304\n"
                "io_exfat_contiguous_io=1\n"
                "io_file_lock_slots=16\n"
                "io_write_through=1\n"
                "io_cached_opens=%llu\n"
                "io_read_cache_hits=%llu\n"
                "io_write_cache_hits=%llu\n"
                "io_write_syncs=%llu\n"
                "io_read_bytes=%llu\n"
                "io_written_bytes=%llu\n"
                "io_timing_version=1\n"
                "io_read_calls=%llu\nio_write_calls=%llu\n"
                "io_read_us=%llu\nio_write_us=%llu\nio_sync_us=%llu\n"
                "io_disk_read_calls=%llu\nio_disk_write_calls=%llu\n"
                "io_disk_read_bytes=%llu\nio_disk_write_bytes=%llu\n"
                "io_disk_read_us=%llu\nio_disk_write_us=%llu\n"
                "usb_interface_protocol=0x%02X\n"
                "usb_bulk_in_max_packet=%u\n"
                "usb_bulk_out_max_packet=%u\n"
                "usb_large_drive=L1\n"
                "usb_startup=U2\n"
                "usb_home_badges=B2\n"
                "usb_nca_reader=E2\n"
                "usb_startup_attempts=%llu\nusb_startup_failures=%llu\nusb_startup_last_ns=%llu\n"
                "usb_startup_stage=%u\nusb_startup_polls=%u\nusb_startup_starts=%u\n"
                "usb_startup_result=0x%08X\nusb_startup_sense=0x%08X\n"
                "usb_startup_failure_stage=%u\nusb_startup_failure_opcode=0x%02X\n"
                "usb_startup_failure_result=0x%08X\nusb_startup_failure_sense=0x%08X\n"
                "usb_startup_vid=0x%04X\nusb_startup_pid=0x%04X\nusb_startup_protocol=0x%02X\nusb_startup_lun=%u\n"
                "usb_reader_lun_count=%u\nusb_reader_empty_lun_mask=0x%04X\nusb_reader_max_lun_result=0x%08X\nusb_reader_insertion_retries=%u\n"
                "usb_startup_endpoints=0x%04X\nusb_startup_packets=0x%08X\n"
                "usb_startup_recoveries=%u\nusb_startup_recovery_stage=%u\nusb_startup_recovery_result=0x%08X\n"
                "usb_probe_failed_cycles=%llu\nusb_probe_cooldown_ms=%u\n"
                "usb_startup_xfer_stage=%u\nusb_startup_xfer_opcode=0x%02X\nusb_startup_xfer_result=0x%08X\n"
                "usb_startup_xfer_expected=%u\nusb_startup_xfer_actual=%u\nusb_startup_xfer_tag=%u\n"
                "usb_startup_csw_signature=0x%08X\nusb_startup_csw_tag=%u\nusb_startup_csw_residue=%u\nusb_startup_csw_status=%u\n"
                "usb_csw_recovery=C4\nusb_csw_recovery_attempts=%u\nusb_csw_recovery_validated=%u\nusb_csw_recovery_stage=%u\n"
                "usb_csw_recovery_lun=%u\nusb_csw_recovery_tag=%u\nusb_csw_recovery_endpoint=0x%02X\n"
                "usb_csw_recovery_vid=0x%04X\nusb_csw_recovery_pid=0x%04X\nusb_csw_recovery_interface_id=0x%08X\nusb_csw_recovery_time_ns=%llu\n"
                "usb_csw_recovery_original=0x%08X\nusb_csw_recovery_query_result=0x%08X\nusb_csw_recovery_query_bytes=%u\n"
                "usb_csw_recovery_halt_status=0x%08X\nusb_csw_recovery_clear_result=0x%08X\n"
                "usb_csw_recovery_retry_result=0x%08X\nusb_csw_recovery_retry_bytes=%u\nusb_csw_recovery_final_result=0x%08X\n"
                "usb_slot_recovery=C5\nusb_slot_recovery_attempts=%u\nusb_slot_recovery_stage=%u\n"
                "usb_slot_recovery_from_lun=%u\nusb_slot_recovery_to_lun=%u\n"
                "usb_slot_recovery_vid=0x%04X\nusb_slot_recovery_pid=0x%04X\nusb_slot_recovery_device_id=0x%08X\n"
                "usb_slot_recovery_time_ns=%llu\nusb_slot_recovery_result=0x%08X\nusb_slot_recovery_reset_result=0x%08X\n"
                "usb_fatfs=R0.16\n"
                "usb_lba_bits=64\n"
                "usb_mount_stage=%u\n"
                "usb_mount_fatfs_result=0x%08X\n"
                "usb_mount_sector_size=%u\n"
                "usb_mount_first_lba=%llu\n"
                "usb_mount_sector_count=%llu\n"
                "usb_keep_alive=K1\n"
                "usb_stability=T4\n"
                "usb_keep_alive_idle_ms=5000\n"
                "usb_keep_alive_wait_budget_ms=15000\n"
                "usb_keep_alive_attempts=%llu\n"
                "usb_keep_alive_successes=%llu\n"
                "usb_keep_alive_failures=%llu\n"
                "usb_keep_alive_busy_skips=%llu\n"
                "usb_keep_alive_last_result=0x%08X\n"
                "usb_keep_alive_last_ns=%llu\n"
                "usb_keep_alive_last_duration_ns=%llu\n"
                "usb_keep_alive_max_duration_ns=%llu\n"
                "usb_keep_alive_slow_successes=%llu\n"
                "usb_last_command_ns=%llu\n"
                "usb_transport_failures=%llu\n"
                "usb_last_failed_opcode=0x%02X\n"
                "usb_last_failed_result=0x%08X\n"
                "usb_last_failure_ns=%llu\n"
                "usb_link_query_failures=%llu\n"
                "usb_link_missing_events=%llu\n"
                "usb_reconnect_requests=%llu\n"
                "usb_empty_completion_notifications=%llu\n"
                "usb_invalid_completion_reports=%llu\n"
                "usb_ready_read_retries=%llu\n"
                "usb_ready_read_recoveries=%llu\n"
                "usb_last_bot_failure_stage=%u\n"
                "usb_last_bot_failure_opcode=0x%02X\n"
                "usb_last_bot_failure_result=0x%08X\n"
                "usb_last_sense_action=%u\n"
                "usb_disconnect_reason=%u\n"
                "usb_disconnect_reason_legend=1:io,2:keepalive,3:host-query,4:host-interface-absent\n"
                "mode=content\n"
                "status_query_ok=1\n"
                "status_query_result=0x%08X\n"
                "mounted=%u\n"
                "registered=%u\n"
                "ready=%u\n"
                "writable=%u\n"
                "state=%s\n"
                "filesystem=%s\n"
                "running=%u\n"
                "last_result=0x%08X\n"
                "last_io_result=0x%08X\n"
                "media_generation=%llu\n"
                "module_result=0x%08X\n"
                "free_space=%llu\n"
                "total_space=%llu\n"
                "route_requests=%u\n"
                "route_successes=%u\n"
                "route_fallbacks=%u\n"
                "last_requested_storage=%u\n"
                "last_selected_storage=%u\n"
                "last_route_result=0x%08X\n"
                "probe_stage=%u\n"
                "probe_result=0x%08X\n"
                "available_interface_count=%u\n"
                "acquired_interface_count=%u\n"
                "probe_attempts=%u\n"
                "runtime_code_attempts=%u\n"
                "runtime_code_successes=%u\n"
                "runtime_code_last_result=0x%08X\n"
                "runtime_romfs_attempts=%u\n"
                "runtime_romfs_successes=%u\n"
                "runtime_romfs_last_result=0x%08X\n"
                "loader_probe_program_id=%016llX\n"
                "loader_probe_stage=%u\n"
                "loader_probe_result=0x%08X\n"
                "loader_probe_location_storage=%u\n"
                "loader_probe_metadata_storage=%u\n"
                "loader_trace_records=%u\n"
                "loader_trace_last_sequence=%llu\n"
                "loader_trace_path=%s\n"
                "ncm_launch_trace_path=%s\n"
                "partition_name=%s\n"
                "vendor=%s\n"
                "product=%s\n"
                "revision=%s\n"
                "registration_attempts=%u\n"
                "registration_successes=%u\n"
                "registration_failures=%u\n"
                "last_registration_result=0x%08X\n"
                "ticket_import_attempts=%u\n"
                "ticket_import_successes=%u\n"
                "ticket_import_failures=%u\n"
                "last_ticket_import_result=0x%08X\n"
                "content_bridge_opens=%llu\n"
                "usb_fs_create_attempts=%llu\n"
                "usb_fs_creates=%llu\n"
                "usb_fs_writes=%llu\n"
                "usb_fs_bytes_written=%llu\n"
                "usb_fs_renames=%llu\n"
                "usb_fs_last_result=0x%08X\n"
                "key_es_sessions=%llu\n"
                "key_registrations=%llu\n"
                "key_native_accepted=%llu\n"
                "key_mirrored=%llu\n"
                "key_removals=%llu\n"
                "key_hits=%llu\n"
                "key_misses=%llu\n"
                "key_last_client=%016llX\n"
                "key_native_result=0x%08X\n"
                "key_mirror_result=0x%08X\n"
                "allow_unsigned_usb_headers=%u\n"
                "usb_header_policy=embedded-sc1\n"
                "usb_header_policy_requires_flag=0\n"
                "unsigned_usb_headers_accepted=%llu\n"
                "shared_status_queries=%llu\n"
                "shared_ready_replies=%llu\n"
                "shared_status_result=0x%08X\n"
                "shared_status_flags=0x%X\n"
                "shared_file_info_queries=%llu\n"
                "shared_directory_queries=%llu\n",
                usb_status.enabled ? 1u : 0u,
                g_content_install_target_enabled.load(std::memory_order_acquire) ? 1u : 0u,
                g_content_route_auto_enabled ? 1u : 0u,
                g_content_route_applied_enabled.load(std::memory_order_acquire) ? 1u : 0u,
                ncm::IsUsbContentNativePathEnabled() ? 1u : 0u,
                ncm::IsUsbContentMetadataRouteEnabled() ? 1u : 0u,
                BuildId,
                static_cast<unsigned long long>(usb::io_diagnostics::file_opens.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::read_hits.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::write_hits.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::write_syncs.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::read_bytes.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::written_bytes.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::read_calls.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::write_calls.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::read_us.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::write_us.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::sync_us.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::disk_read_calls.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::disk_write_calls.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::disk_read_bytes.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::disk_write_bytes.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::disk_read_us.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::disk_write_us.load()),
                usb::io_diagnostics::protocol.load(),
                usb::io_diagnostics::bulk_in_packet.load(),
                usb::io_diagnostics::bulk_out_packet.load(),
                static_cast<unsigned long long>(usb::io_diagnostics::startup_attempts.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::startup_failures.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::startup_last_ns.load()),
                usb::io_diagnostics::startup_stage.load(),
                usb::io_diagnostics::startup_polls.load(),
                usb::io_diagnostics::startup_starts.load(),
                usb::io_diagnostics::startup_result.load(),
                usb::io_diagnostics::startup_sense.load(),
                usb::io_diagnostics::startup_failure_stage.load(),
                usb::io_diagnostics::startup_failure_opcode.load(),
                usb::io_diagnostics::startup_failure_result.load(),
                usb::io_diagnostics::startup_failure_sense.load(),
                usb::io_diagnostics::startup_vid.load(),
                usb::io_diagnostics::startup_pid.load(),
                usb::io_diagnostics::startup_protocol.load(),
                usb::io_diagnostics::startup_lun.load(),
                usb::io_diagnostics::reader_lun_count.load(),
                usb::io_diagnostics::reader_empty_lun_mask.load(),
                usb::io_diagnostics::reader_max_lun_result.load(),
                usb::io_diagnostics::reader_insertion_retries.load(),
                usb::io_diagnostics::startup_endpoints.load(),
                usb::io_diagnostics::startup_packets.load(),
                usb::io_diagnostics::startup_recoveries.load(),
                usb::io_diagnostics::startup_recovery_stage.load(),
                usb::io_diagnostics::startup_recovery_result.load(),
                static_cast<unsigned long long>(usb::io_diagnostics::probe_failed_cycles.load()),
                usb::io_diagnostics::probe_cooldown_ms.load(),
                usb::io_diagnostics::startup_xfer_stage.load(),
                usb::io_diagnostics::startup_xfer_opcode.load(),
                usb::io_diagnostics::startup_xfer_result.load(),
                usb::io_diagnostics::startup_xfer_expected.load(),
                usb::io_diagnostics::startup_xfer_actual.load(),
                usb::io_diagnostics::startup_xfer_tag.load(),
                usb::io_diagnostics::startup_csw_signature.load(),
                usb::io_diagnostics::startup_csw_tag.load(),
                usb::io_diagnostics::startup_csw_residue.load(),
                usb::io_diagnostics::startup_csw_status.load(),
                usb::io_diagnostics::csw_recovery_attempts.load(),
                usb::io_diagnostics::csw_recovery_validated.load(),
                usb::io_diagnostics::csw_recovery_stage.load(),
                usb::io_diagnostics::csw_recovery_lun.load(),
                usb::io_diagnostics::csw_recovery_tag.load(),
                usb::io_diagnostics::csw_recovery_endpoint.load(),
                usb::io_diagnostics::csw_recovery_vid.load(),
                usb::io_diagnostics::csw_recovery_pid.load(),
                usb::io_diagnostics::csw_recovery_interface_id.load(),
                static_cast<unsigned long long>(usb::io_diagnostics::csw_recovery_time_ns.load()),
                usb::io_diagnostics::csw_recovery_original.load(),
                usb::io_diagnostics::csw_recovery_query_result.load(),
                usb::io_diagnostics::csw_recovery_query_bytes.load(),
                usb::io_diagnostics::csw_recovery_halt_status.load(),
                usb::io_diagnostics::csw_recovery_clear_result.load(),
                usb::io_diagnostics::csw_recovery_retry_result.load(),
                usb::io_diagnostics::csw_recovery_retry_bytes.load(),
                usb::io_diagnostics::csw_recovery_final_result.load(),
                usb::io_diagnostics::slot_recovery_attempts.load(),
                usb::io_diagnostics::slot_recovery_stage.load(),
                usb::io_diagnostics::slot_recovery_from_lun.load(),
                usb::io_diagnostics::slot_recovery_to_lun.load(),
                usb::io_diagnostics::slot_recovery_vid.load(),
                usb::io_diagnostics::slot_recovery_pid.load(),
                usb::io_diagnostics::slot_recovery_device_id.load(),
                static_cast<unsigned long long>(usb::io_diagnostics::slot_recovery_time_ns.load()),
                usb::io_diagnostics::slot_recovery_result.load(),
                usb::io_diagnostics::slot_recovery_reset_result.load(),
                usb::io_diagnostics::mount_stage.load(),
                usb::io_diagnostics::mount_fatfs_result.load(),
                usb::io_diagnostics::mount_sector_size.load(),
                static_cast<unsigned long long>(usb::io_diagnostics::mount_first_lba.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::mount_sector_count.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_attempts.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_successes.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_failures.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_busy_skips.load()),
                usb::io_diagnostics::keep_alive_last_result.load(),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_last_ns.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_last_duration_ns.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_max_duration_ns.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::keep_alive_slow_successes.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::last_command_ns.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::transport_failures.load()),
                usb::io_diagnostics::last_failed_opcode.load(),
                usb::io_diagnostics::last_failed_result.load(),
                static_cast<unsigned long long>(usb::io_diagnostics::last_failure_ns.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::link_query_failures.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::link_missing_events.load()),
                static_cast<unsigned long long>(usb::io_diagnostics::reconnect_requests.load()),
                static_cast<unsigned long long>(usb::runtime_diagnostics::empty_reports.load()),
                static_cast<unsigned long long>(usb::runtime_diagnostics::invalid_reports.load()),
                static_cast<unsigned long long>(usb::runtime_diagnostics::ready_retries.load()),
                static_cast<unsigned long long>(usb::runtime_diagnostics::ready_recovered.load()),
                usb::runtime_diagnostics::last_stage.load(),
                usb::runtime_diagnostics::last_opcode.load(),
                usb::runtime_diagnostics::last_result.load(),
                usb::runtime_diagnostics::last_sense_action.load(),
                usb::runtime_diagnostics::disconnect_reason.load(),
                status_query_result.GetValue(),
                usb_status.mounted ? 1u : 0u,
                usb_status.registered ? 1u : 0u,
                ready ? 1u : 0u,
                usb_status.writable ? 1u : 0u,
                GetStateName(static_cast<ams::usb::storage::State>(usb_status.state)),
                GetFilesystemName(static_cast<ams::usb::storage::FileSystemType>(usb_status.filesystem_type)),
                usb_status.running ? 1u : 0u,
                usb_status.last_result,
                usb_status.last_io_result,
                static_cast<unsigned long long>(usb_status.media_generation),
                g_last_module_result,
                static_cast<unsigned long long>(usb_status.free_space),
                static_cast<unsigned long long>(usb_status.total_space),
                usb_status.route_requests,
                usb_status.route_successes,
                usb_status.route_fallbacks,
                usb_status.last_requested_storage,
                usb_status.last_selected_storage,
                usb_status.last_route_result,
                usb_status.probe_stage,
                usb_status.probe_result,
                usb_status.available_interface_count,
                usb_status.acquired_interface_count,
                usb_status.probe_attempts,
                g_runtime_code_attempts.load(std::memory_order_relaxed),
                g_runtime_code_successes.load(std::memory_order_relaxed),
                g_runtime_code_last_result.load(std::memory_order_relaxed),
                g_runtime_romfs_attempts.load(std::memory_order_relaxed),
                g_runtime_romfs_successes.load(std::memory_order_relaxed),
                g_runtime_romfs_last_result.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(g_loader_probe_program_id.load(std::memory_order_relaxed)),
                g_loader_probe_stage.load(std::memory_order_relaxed),
                g_loader_probe_result.load(std::memory_order_relaxed),
                g_loader_probe_location_storage.load(std::memory_order_relaxed),
                g_loader_probe_metadata_storage.load(std::memory_order_relaxed),
                loader_trace_records,
                static_cast<unsigned long long>(loader_trace_last_sequence),
                LoaderTracePath,
                NcmLaunchTracePath,
                usb_status.partition_name,
                usb_status.vendor,
                usb_status.product,
                usb_status.revision,
                usb_status.registration_attempts,
                usb_status.registration_successes,
                usb_status.registration_failures,
                usb_status.last_registration_result,
                usb_status.ticket_import_attempts,
                usb_status.ticket_import_successes,
                usb_status.ticket_import_failures,
                usb_status.last_ticket_import_result,
                static_cast<unsigned long long>(content_stats::bridge_opens.load()),
                static_cast<unsigned long long>(content_stats::create_attempts.load()),
                static_cast<unsigned long long>(content_stats::creates.load()),
                static_cast<unsigned long long>(content_stats::writes.load()),
                static_cast<unsigned long long>(content_stats::bytes.load()),
                static_cast<unsigned long long>(content_stats::renames.load()),
                content_stats::last_result.load(),
                static_cast<unsigned long long>(key_stats::es_sessions.load()),
                static_cast<unsigned long long>(key_stats::registrations.load()),
                static_cast<unsigned long long>(key_stats::accepted.load()),
                static_cast<unsigned long long>(key_stats::mirrored.load()),
                static_cast<unsigned long long>(key_stats::removals.load()),
                static_cast<unsigned long long>(key_stats::hits.load()),
                static_cast<unsigned long long>(key_stats::misses.load()),
                static_cast<unsigned long long>(key_stats::last_client.load()),
                key_stats::native_result.load(), key_stats::mirror_result.load(),
                g_allow_unsigned_usb_application.load() ? 1u : 0u,
                static_cast<unsigned long long>(g_unsigned_usb_headers_accepted.load()),
                static_cast<unsigned long long>(g_shared_status_queries.load()),
                static_cast<unsigned long long>(g_shared_ready_replies.load()),
                g_shared_status_result.load(), g_shared_status_flags.load(),
                static_cast<unsigned long long>(g_shared_info_queries.load()),
                static_cast<unsigned long long>(g_shared_directory_queries.load()));
            R_UNLESS(written > 0 && static_cast<size_t>(written) < sizeof(status), usb::ResultInvalidParameter());

            const int discovery_written = usb::reader::FormatInventory(status + written, sizeof(status) - written);
            R_UNLESS(discovery_written > 0 && static_cast<size_t>(discovery_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += discovery_written;

            const int map_written = AppendDeviceMapSummary(status + written, sizeof(status) - written);
            R_UNLESS(map_written > 0 && static_cast<size_t>(map_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += map_written;

            const int preference_written = slot_preference_sd::Format(status + written, sizeof(status) - written);
            R_UNLESS(preference_written > 0 && static_cast<size_t>(preference_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += preference_written;

            const int profile_written = usb::startup_profile::Format(status + written, sizeof(status) - written);
            R_UNLESS(profile_written > 0 && static_cast<size_t>(profile_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += profile_written;
            const int endpoint_written = usb::endpoint_profile::Format(status + written, sizeof(status) - written);
            R_UNLESS(endpoint_written > 0 && static_cast<size_t>(endpoint_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += endpoint_written;
            const int compatibility_written = compatibility_sd::Format(status + written, sizeof(status) - written, g_device_map);
            R_UNLESS(compatibility_written > 0 && static_cast<size_t>(compatibility_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += compatibility_written;
            const int recovery_written = usb::interface_recovery::Format(status + written, sizeof(status) - written);
            R_UNLESS(recovery_written > 0 && static_cast<size_t>(recovery_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += recovery_written;
            const int trace_written = usb::probe_trace::Format(status + written, sizeof(status) - written);
            R_UNLESS(trace_written > 0 && static_cast<size_t>(trace_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += trace_written;
            const int trial_written = usb::reader_device_reset::Format(status + written, sizeof(status) - written);
            R_UNLESS(trial_written > 0 && static_cast<size_t>(trial_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += trial_written;
            const int halt_written = usb::reader_direct_start::Format(status + written, sizeof(status) - written);
            R_UNLESS(halt_written > 0 && static_cast<size_t>(halt_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += halt_written;
            const int history_written = usb::startup_history::Format(status + written, sizeof(status) - written);
            R_UNLESS(history_written > 0 && static_cast<size_t>(history_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += history_written;
            const int sense_written = usb::startup_sense_csw::Format(status + written, sizeof(status) - written);
            R_UNLESS(sense_written > 0 && static_cast<size_t>(sense_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += sense_written;
            const int fixed_sense_written = usb::reader_fixed_sense::Format(status + written, sizeof(status) - written);
            R_UNLESS(fixed_sense_written > 0 && static_cast<size_t>(fixed_sense_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += fixed_sense_written;

            const int startup_save_written = FormatStartupTimingAutosaveStatus(status + written, sizeof(status) - written);
            R_UNLESS(startup_save_written > 0 && static_cast<size_t>(startup_save_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += startup_save_written;

            const int optimization_written = FormatUsbMetadataOptimizationStatus(status + written, sizeof(status) - written);
            R_UNLESS(optimization_written > 0 && static_cast<size_t>(optimization_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += optimization_written;
            const int power_written = system_power::Format(status + written, sizeof(status) - written);
            R_UNLESS(power_written > 0 && static_cast<size_t>(power_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += power_written;

            SetMonitorPhase(impl::MonitorProgress::StatusPublish);
            R_TRY(PublishStatusSnapshot(status, static_cast<size_t>(written)));
            const u32 runtime_code_attempts = g_runtime_code_attempts.load(std::memory_order_relaxed);
            const u32 runtime_code_successes = g_runtime_code_successes.load(std::memory_order_relaxed);
            const u32 runtime_code_last_result = g_runtime_code_last_result.load(std::memory_order_relaxed);
            const u64 unsigned_headers_accepted = g_unsigned_usb_headers_accepted.load(std::memory_order_relaxed);
            const u64 read_bytes = usb::io_diagnostics::read_bytes.load(std::memory_order_relaxed);
            const u64 written_bytes = usb::io_diagnostics::written_bytes.load(std::memory_order_relaxed);
            const bool last_ready_snapshot_changed = ready && (!g_last_ready_snapshot_valid
                || g_last_ready_content_changes != content_changes
                || g_last_ready_snapshot_generation != usb_status.media_generation
                || g_last_ready_snapshot_free_space != usb_status.free_space
                || g_last_ready_snapshot_total_space != usb_status.total_space
                || g_last_ready_snapshot_route_requests != usb_status.route_requests
                || g_last_ready_snapshot_route_successes != usb_status.route_successes
                || g_last_ready_snapshot_route_fallbacks != usb_status.route_fallbacks
                || g_last_ready_snapshot_runtime_code_attempts != runtime_code_attempts
                || g_last_ready_snapshot_runtime_code_successes != runtime_code_successes
                || g_last_ready_snapshot_runtime_code_last_result != runtime_code_last_result
                || g_last_ready_snapshot_unsigned_headers_accepted != unsigned_headers_accepted
                || g_last_ready_snapshot_read_bytes != read_bytes
                || g_last_ready_snapshot_written_bytes != written_bytes);
            SetMonitorPhase(impl::MonitorProgress::LastReadyPublish);
            if (last_ready_snapshot_changed && R_SUCCEEDED(PublishLastReadyStatusSnapshot(status, static_cast<size_t>(written)))) {
                g_last_ready_snapshot_valid = true;
                g_last_ready_content_changes = content_changes;
                g_last_ready_snapshot_generation = usb_status.media_generation;
                g_last_ready_snapshot_free_space = usb_status.free_space;
                g_last_ready_snapshot_total_space = usb_status.total_space;
                g_last_ready_snapshot_route_requests = usb_status.route_requests;
                g_last_ready_snapshot_route_successes = usb_status.route_successes;
                g_last_ready_snapshot_route_fallbacks = usb_status.route_fallbacks;
                g_last_ready_snapshot_runtime_code_attempts = runtime_code_attempts;
                g_last_ready_snapshot_runtime_code_successes = runtime_code_successes;
                g_last_ready_snapshot_runtime_code_last_result = runtime_code_last_result;
                g_last_ready_snapshot_unsigned_headers_accepted = unsigned_headers_accepted;
                g_last_ready_snapshot_read_bytes = read_bytes;
                g_last_ready_snapshot_written_bytes = written_bytes;
            }
            SetMonitorPhase(impl::MonitorProgress::NcmTracePublish);
            static_cast<void>(PublishNcmLaunchTraceSnapshot());
            SetMonitorPhase(impl::MonitorProgress::Notification);
            UpdateContentUltrahandNotification(usb_status);
            R_SUCCEED();
        }

        Result WriteStatus() {
            std::scoped_lock lk(g_status_mutex);
            ams::usb::storage::Status storage_status{};
            ams::usb::filesystem::Status filesystem_status{};
            R_TRY(g_storage_service.GetStatus(std::addressof(storage_status)));
            R_TRY(g_filesystem_service.GetStatus(std::addressof(filesystem_status)));
            // Protected by g_status_mutex; keep the larger buffer off the stack.
            static char status[StatusBufferSize]{};
            auto written = util::SNPrintf(status, sizeof(status),
                "enabled=1\n"
                "build_id=%s\n"
                "mounted=%u\n"
                "registered=%u\n"
                "ready=%u\n"
                "state=%s\n"
                "filesystem=%s\n"
                "running=%u\n"
                "last_result=0x%08X\n"
                "last_io_result=0x%08X\n"
                "media_generation=%llu\n"
                "module_result=0x%08X\n"
                "partition_index=%u\n"
                "partition_count=%u\n"
                "interface_id=%d\n"
                "lun=%u\n"
                "block_count=%llu\n"
                "block_size=%u\n"
                "partition_first_lba=%llu\n"
                "partition_sector_count=%llu\n"
                "partition_name=%s\n"
                "vendor=%s\n"
                "product=%s\n"
                "revision=%s\n"
                "runtime_code_attempts=%u\n"
                "runtime_code_successes=%u\n"
                "runtime_code_last_result=0x%08X\n"
                "runtime_romfs_attempts=%u\n"
                "runtime_romfs_successes=%u\n"
                "runtime_romfs_last_result=0x%08X\n"
                "image_scan_result=0x%08X\n"
                "image_scan_generation=%llu\n"
                "image_scan_truncated=%u\n"
                "image_candidates=%u\n"
                "image_valid=%u\n"
                "image_invalid=%u\n"
                "image_unsupported=%u\n"
                "image_0_result=0x%08X\n"
                "image_0_format=%s\n"
                "image_0_entries=%u\n"
                "image_0_size=%llu\n"
                "image_0_path=%s\n"
                "image_1_result=0x%08X\n"
                "image_1_format=%s\n"
                "image_1_entries=%u\n"
                "image_1_size=%llu\n"
                "image_1_path=%s\n"
                "homebrew_scan_result=0x%08X\n"
                "homebrew_scan_generation=%llu\n"
                "homebrew_scan_truncated=%u\n"
                "homebrew_candidates=%u\n"
                "homebrew_valid=%u\n"
                "homebrew_invalid=%u\n"
                "homebrew_0_result=0x%08X\n"
                "homebrew_0_version=%u\n"
                "homebrew_0_flags=0x%08X\n"
                "homebrew_0_image_size=%llu\n"
                "homebrew_0_file_size=%llu\n"
                "homebrew_0_path=%s\n"
                "homebrew_1_result=0x%08X\n"
                "homebrew_1_version=%u\n"
                "homebrew_1_flags=0x%08X\n"
                "homebrew_1_image_size=%llu\n"
                "homebrew_1_file_size=%llu\n"
                "homebrew_1_path=%s\n",
                BuildId,
                filesystem_status.mounted ? 1u : 0u,
                g_filesystem_registered ? 1u : 0u,
                filesystem_status.ready ? 1u : 0u,
                GetStateName(storage_status.state),
                GetFilesystemName(storage_status.filesystem_type),
                storage_status.running ? 1u : 0u,
                storage_status.last_result,
                storage_status.last_io_result,
                static_cast<unsigned long long>(storage_status.media_generation),
                g_last_module_result,
                storage_status.partition_index,
                storage_status.partition_count,
                storage_status.interface_id,
                storage_status.lun,
                static_cast<unsigned long long>(storage_status.block_count),
                storage_status.block_size,
                static_cast<unsigned long long>(storage_status.partition_first_lba),
                static_cast<unsigned long long>(storage_status.partition_sector_count),
                storage_status.partition_name,
                storage_status.vendor,
                storage_status.product,
                storage_status.revision,
                g_runtime_code_attempts.load(std::memory_order_relaxed),
                g_runtime_code_successes.load(std::memory_order_relaxed),
                g_runtime_code_last_result.load(std::memory_order_relaxed),
                g_runtime_romfs_attempts.load(std::memory_order_relaxed),
                g_runtime_romfs_successes.load(std::memory_order_relaxed),
                g_runtime_romfs_last_result.load(std::memory_order_relaxed),
                g_image_scan.result,
                static_cast<unsigned long long>(g_image_scan.media_generation),
                g_image_scan.truncated ? 1u : 0u,
                g_image_scan.candidate_count,
                g_image_scan.valid_count,
                g_image_scan.invalid_count,
                g_image_scan.unsupported_count,
                g_image_scan.records[0].result,
                GetImageFormatName(g_image_scan.records[0].format),
                g_image_scan.records[0].entry_count,
                static_cast<unsigned long long>(g_image_scan.records[0].size),
                g_image_scan.records[0].path,
                g_image_scan.records[1].result,
                GetImageFormatName(g_image_scan.records[1].format),
                g_image_scan.records[1].entry_count,
                static_cast<unsigned long long>(g_image_scan.records[1].size),
                g_image_scan.records[1].path,
                g_homebrew_scan.result,
                static_cast<unsigned long long>(g_homebrew_scan.media_generation),
                g_homebrew_scan.truncated ? 1u : 0u,
                g_homebrew_scan.candidate_count,
                g_homebrew_scan.valid_count,
                g_homebrew_scan.invalid_count,
                g_homebrew_scan.records[0].result,
                g_homebrew_scan.records[0].version,
                g_homebrew_scan.records[0].flags,
                static_cast<unsigned long long>(g_homebrew_scan.records[0].image_size),
                static_cast<unsigned long long>(g_homebrew_scan.records[0].file_size),
                g_homebrew_scan.records[0].path,
                g_homebrew_scan.records[1].result,
                g_homebrew_scan.records[1].version,
                g_homebrew_scan.records[1].flags,
                static_cast<unsigned long long>(g_homebrew_scan.records[1].image_size),
                static_cast<unsigned long long>(g_homebrew_scan.records[1].file_size),
                g_homebrew_scan.records[1].path);
            R_UNLESS(written > 0 && static_cast<size_t>(written) < sizeof(status), usb::ResultInvalidParameter());

            const int startup_save_written = FormatStartupTimingAutosaveStatus(status + written, sizeof(status) - written);
            R_UNLESS(startup_save_written > 0 && static_cast<size_t>(startup_save_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += startup_save_written;

            const int optimization_written = FormatUsbMetadataOptimizationStatus(status + written, sizeof(status) - written);
            R_UNLESS(optimization_written > 0 && static_cast<size_t>(optimization_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += optimization_written;
            const int power_written = system_power::Format(status + written, sizeof(status) - written);
            R_UNLESS(power_written > 0 && static_cast<size_t>(power_written) < sizeof(status) - written, usb::ResultInvalidParameter());
            written += power_written;

            R_TRY(PublishStatusSnapshot(status, static_cast<size_t>(written)));
            UpdateUltrahandNotificationState(storage_status, filesystem_status);
            R_SUCCEED();
        }

        #include "usb_eject_commands.hpp"

        Result EnsureUsbStarted() {
            const ams::usb::pause_diagnostics::Scope pause(ams::usb::pause_diagnostics::Stage::StartupGate);
            R_UNLESS(IsEnabled(), usb::ResultOperationDenied());
            std::scoped_lock lk(g_start_mutex);
            // A failed power coordinator is sticky for this boot. Starting a
            // fresh worker must not bypass the monitor's quarantined state.
            R_UNLESS(!system_power::HasFailed(), usb::ResultNotInitialized());
            if (g_filesystem_registered && g_storage_service.IsRunning() && g_filesystem_service.IsMounted()) {
                R_SUCCEED();
            }

            const auto start_result = g_storage_service.Start();
            if (R_FAILED(start_result)) {
                g_last_module_result = static_cast<Result>(start_result).GetValue();
                R_RETURN(start_result);
            }

            const auto mount_result = g_filesystem_service.Mount(std::addressof(g_storage_service));
            if (R_FAILED(mount_result)) {
                g_last_module_result = static_cast<Result>(mount_result).GetValue();
                static_cast<void>(g_storage_service.Stop());
                R_RETURN(mount_result);
            }

            std::unique_ptr<ams::fs::fsa::IFileSystem> usb_filesystem;
            const auto filesystem_result = ams::usb::filesystem::CreateReadOnlyFileSystem(std::addressof(usb_filesystem), std::addressof(g_filesystem_service));
            if (R_FAILED(filesystem_result)) {
                g_last_module_result = static_cast<Result>(filesystem_result).GetValue();
                static_cast<void>(g_filesystem_service.Unmount());
                static_cast<void>(g_storage_service.Stop());
                R_RETURN(filesystem_result);
            }

            const auto register_result = ams::fs::fsa::Register(ams::usb::filesystem::MountName, std::move(usb_filesystem));
            if (R_FAILED(register_result)) {
                g_last_module_result = static_cast<Result>(register_result).GetValue();
                static_cast<void>(g_filesystem_service.Unmount());
                static_cast<void>(g_storage_service.Stop());
                R_RETURN(register_result);
            }

            g_filesystem_registered = true;
            R_SUCCEED();
        }

    }

    bool IsUsbContentLaunchPathEnabled() {
        if constexpr (ams::usb::content::UseFileSystemContentRoute) {
            /* The R97 route keeps Horizon's stock SD ProgramLocation and no
             * longer consumes the retired Host/title-map compatibility path. */
            return false;
        }
        return g_content_launch_path_enabled.load(std::memory_order_acquire);
    }

    bool IsUsbMappedTitleLaunchEnabled() {
        /* Do not reuse IsUsbContentLaunchPathEnabled() here. The clean
         * filesystem route intentionally makes that legacy gate false at
         * compile time, while an exact mapped title still needs its Program
         * byte path primed before qlaunch hands it to Loader. Route state is
         * maintained by the post-boot monitor and is cleared on eject. */
        return g_content_launch_path_enabled.load(std::memory_order_acquire);
    }

    bool IsUsbContentFileSystemRouteReady(u64 *out_media_generation, bool require_writable) {
        if constexpr (!ams::usb::content::UseFileSystemContentRoute) {
            AMS_UNUSED(out_media_generation, require_writable);
            return false;
        }

        /* This is intentionally a snapshot only. The monitor owns Start() and
         * mount detection after Horizon initialization; an NCM command-110
         * filesystem call must never start USB or wait on usb:hs. */
        if (!g_content_install_target_enabled.load(std::memory_order_acquire)) {
            return false;
        }

        ams::usb::storage::Status status{};
        if (R_FAILED(g_storage_service.GetStatus(std::addressof(status)))) {
            return false;
        }
        if (status.state != ams::usb::storage::State::Ready || !status.running
            || (require_writable && status.writable == 0)) {
            return false;
        }

        if (out_media_generation != nullptr) {
            *out_media_generation = status.media_generation;
        }
        return status.media_generation != 0;
    }

    ams::usb::storage::Service *GetUsbContentStorageService() {
        return std::addressof(g_storage_service);
    }

    namespace {

        /*
         * Most Horizon application ids are in the normal 0x0100... range,
         * but installed titles can also use a non-standard program id.  The
         * FireRed test title in the launch trace is one of those ids.  The
         * content/meta and Host storage APIs still identify it by ProgramId,
         * so the path lookup must not reject it before those APIs are queried.
         */
        Result FindHostContentId(ncm::ContentId *out_content_id, ncm::ContentStorage *host_storage, ncm::ProgramId program_id, ncm::ContentType content_type) {
            R_UNLESS(out_content_id != nullptr && host_storage != nullptr, usb::ResultInvalidParameter());

            fssystem::NcaHeader::ContentType wanted_type;
            switch (content_type) {
                case ncm::ContentType::Meta:
                    wanted_type = fssystem::NcaHeader::ContentType::Meta;
                    break;
                case ncm::ContentType::Program:
                    wanted_type = fssystem::NcaHeader::ContentType::Program;
                    break;
                case ncm::ContentType::Control:
                    wanted_type = fssystem::NcaHeader::ContentType::Control;
                    break;
                default:
                    R_THROW(ncm::ResultContentNotFound());
            }

            s32 content_count = 0;
            R_TRY(host_storage->GetContentCount(std::addressof(content_count)));
            constexpr size_t ContentBatchSize = 32;
            std::array<ncm::ContentId, ContentBatchSize> content_ids{};
            for (s32 offset = 0; offset < content_count; offset += static_cast<s32>(ContentBatchSize)) {
                s32 written = 0;
                R_TRY(host_storage->ListContentId(std::addressof(written), content_ids.data(), content_ids.size(), offset));
                for (s32 i = 0; i < written; ++i) {
                    ncm::Path candidate_path;
                    if (R_FAILED(host_storage->GetPath(std::addressof(candidate_path), content_ids[static_cast<size_t>(i)]))) {
                        continue;
                    }

                    char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
                    if (R_FAILED(TranslateContentPathToUsb(mounted_path, sizeof(mounted_path), candidate_path.str))) {
                        continue;
                    }

                    /* ProgramId alone is not enough: Program, Control,
                     * Manual, and Meta NCAs for one title share it. Inspect
                     * the NCA header so each launch-boundary request gets
                     * the requested content type. */
                    std::shared_ptr<fssystem::NcaReader> reader;
                    if (R_FAILED(OpenUsbNcaReader(std::addressof(reader), mounted_path, false))) {
                        continue;
                    }
                    if (reader->GetContentType() == wanted_type && reader->GetProgramId() == program_id.value) {
                        *out_content_id = content_ids[static_cast<size_t>(i)];
                        R_SUCCEED();
                    }
                }
                if (written <= 0) {
                    break;
                }
            }

            R_THROW(ncm::ResultContentNotFound());
        }

        Result CopyHostContentPath(char *out_path, size_t out_size, ncm::ContentStorage *host_storage, ncm::ContentId content_id, ncm::ProgramId program_id) {
            R_UNLESS(out_path != nullptr && out_size > 0 && host_storage != nullptr, usb::ResultInvalidParameter());

            ncm::Path content_path;
            const Result path_result = host_storage->GetPath(std::addressof(content_path), content_id);
            RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::HostPathLookup, program_id, 0, path_result, R_SUCCEEDED(path_result) ? content_path.str : nullptr, static_cast<u8>(ncm::StorageId::Host), static_cast<u8>(ncm::StorageId::Host));
            R_TRY(path_result);

            /* HostContentStorage intentionally exposes a stock-looking path
             * for ordinary NCM callers. Launch-boundary callers must receive
             * the explicit provider mount, otherwise NS/PGL will resolve the
             * returned path against the SD card again. */
            R_RETURN(TranslateContentPathToUsb(out_path, out_size, content_path.str));
        }

        Result GetUsbProgramContentPathImpl(char *out_path, size_t out_size, ncm::ProgramId program_id, ncm::ContentType content_type) {
            R_UNLESS(out_path != nullptr && out_size > 0, usb::ResultInvalidParameter());
            R_UNLESS(content_type == ncm::ContentType::Meta
                || content_type == ncm::ContentType::Program
                || content_type == ncm::ContentType::Control, ncm::ResultContentNotFound());

            /* This function is called from ns:am/ns:web for ordinary title
             * enumeration as well as for a real launch. Do not start or query
             * the USB Host storage for every title that DBI asks about: only a
             * title explicitly recorded in the USB map is eligible for the
             * external route. Unmapped titles immediately fall through to
             * Horizon's normal SD/NAND path, which keeps database refreshes
             * independent of USB latency. */
            R_UNLESS(IsUsbContentLaunchPathEnabled(), ncm::ResultUnknownContentStorageNotActive());
            R_UNLESS(IsCachedMappedTitle(program_id), ncm::ResultContentNotFound());

            /* The association is written after the normal metadata commit.
             * When present, use the exact persisted ContentId for this
             * content type. This prevents a stale SD path or a different NCA
             * with the same ProgramId from being selected. Legacy v1 maps do
             * not contain Meta, so those continue through the metadata
             * fallback below and are upgraded on the next install. */
            ncm::ContentId mapped_meta_content_id{};
            ncm::ContentId mapped_control_content_id{};
            ncm::ContentId mapped_program_content_id{};
            const Result map_result = ncm::GetUsbTitleContentIds(program_id,
                std::addressof(mapped_meta_content_id),
                std::addressof(mapped_control_content_id),
                std::addressof(mapped_program_content_id));
            if (R_SUCCEEDED(map_result)) {
                RememberMappedTitleContentIds(program_id, mapped_meta_content_id,
                    mapped_control_content_id, mapped_program_content_id);
            }
            RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::MappedTitleContentCacheSeed,
                program_id, static_cast<u8>(content_type), map_result, "persistent title-content map");

            ncm::ContentId mapped_content_id = ncm::InvalidContentId;
            switch (content_type) {
                case ncm::ContentType::Meta:
                    mapped_content_id = mapped_meta_content_id;
                    break;
                case ncm::ContentType::Program:
                    mapped_content_id = mapped_program_content_id;
                    break;
                case ncm::ContentType::Control:
                    mapped_content_id = mapped_control_content_id;
                    break;
                default:
                    break;
            }

            ncm::ContentStorage host_storage;
            const Result host_open_result = ncm::OpenContentStorage(std::addressof(host_storage), ncm::StorageId::Host);
            RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::HostOpenAttempt, program_id, static_cast<u8>(content_type), host_open_result, nullptr, static_cast<u8>(ncm::StorageId::Host), static_cast<u8>(ncm::StorageId::Host));
            R_TRY(host_open_result);

            if (R_SUCCEEDED(map_result) && mapped_content_id != ncm::InvalidContentId) {
                bool present = false;
                const Result has_result = host_storage.Has(std::addressof(present), mapped_content_id);
                RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::HostContentCheck, program_id, static_cast<u8>(content_type), has_result, nullptr, static_cast<u8>(ncm::StorageId::Host), static_cast<u8>(ncm::StorageId::Host));
                R_TRY(has_result);
                R_UNLESS(present, ncm::ResultContentNotFound());
                R_RETURN(CopyHostContentPath(out_path, out_size, std::addressof(host_storage), mapped_content_id, program_id));
            }

            const std::array metadata_candidates = {
                ncm::StorageId::SdCard,
                ncm::StorageId::BuiltInUser,
            };

            ncm::ContentId metadata_content_id = ncm::InvalidContentId;
            ncm::ContentMetaDatabase content_meta_database;
            Result metadata_result = ncm::ResultContentMetaNotFound();
            for (const auto candidate : metadata_candidates) {
                metadata_result = ncm::OpenContentMetaDatabase(std::addressof(content_meta_database), candidate);
                if (R_FAILED(metadata_result)) {
                    continue;
                }

                if (content_type == ncm::ContentType::Meta) {
                    ncm::ContentMetaKey latest_key{};
                    metadata_result = content_meta_database.GetLatest(std::addressof(latest_key), program_id.value);
                    if (R_SUCCEEDED(metadata_result)) {
                        metadata_result = content_meta_database.GetContentIdByType(std::addressof(metadata_content_id), latest_key, ncm::ContentType::Meta);
                    }
                } else if (content_type == ncm::ContentType::Program) {
                    metadata_result = content_meta_database.GetLatestProgram(std::addressof(metadata_content_id), program_id);
                } else {
                    metadata_result = content_meta_database.GetLatestControl(std::addressof(metadata_content_id), ncm::ApplicationId{program_id.value});
                }
                if (R_SUCCEEDED(metadata_result)) {
                    break;
                }
            }
            RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::MetadataLookup, program_id, static_cast<u8>(content_type), metadata_result, nullptr, static_cast<u8>(ncm::StorageId::None), static_cast<u8>(ncm::StorageId::None));

            if (R_SUCCEEDED(metadata_result)) {
                bool present = false;
                const Result has_result = host_storage.Has(std::addressof(present), metadata_content_id);
                RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::HostContentCheck, program_id, static_cast<u8>(content_type), has_result, nullptr, static_cast<u8>(ncm::StorageId::Host), static_cast<u8>(ncm::StorageId::Host));
                if (R_SUCCEEDED(has_result) && present) {
                    R_RETURN(CopyHostContentPath(out_path, out_size, std::addressof(host_storage), metadata_content_id, program_id));
                }
            }

            /* If the stock metadata record is stale or absent, identify the
             * registered program NCA directly from the Host volume.  This is
             * required for titles whose program id is outside Horizon's
             * normal application-id range. */
            R_TRY(EnsureUsbStarted());
            ncm::ContentId scanned_content_id{};
            const Result scan_result = FindHostContentId(std::addressof(scanned_content_id), std::addressof(host_storage), program_id, content_type);
            RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::HostContentCheck, program_id, static_cast<u8>(content_type), scan_result, nullptr, static_cast<u8>(ncm::StorageId::Host), static_cast<u8>(ncm::StorageId::Host));
            R_TRY(scan_result);

            R_RETURN(CopyHostContentPath(out_path, out_size, std::addressof(host_storage), scanned_content_id, program_id));
        }

    }

    Result GetUsbApplicationContentPath(char *out_path, size_t out_size, ncm::ProgramId application_id, ncm::ContentType content_type) {
        R_UNLESS(out_path != nullptr && out_size > 0, usb::ResultInvalidParameter());
        R_UNLESS(!ncm::IsSystemProgramId(application_id), usb::ResultInvalidParameter());
        R_RETURN(GetUsbProgramContentPathImpl(out_path, out_size, application_id, content_type));
    }

    bool EnsureUsbProgramPath(ncm::ProgramId program_id) {
        char cached_path[ams::fs::EntryNameLengthMax + 1]{};
        if (FindProgramPath(cached_path, sizeof(cached_path), program_id)) {
            return true;
        }

        /* The loader normally discovers and caches this path while mounting
         * the code NCA.  Some launch paths ask fs.mitm for the current
         * process's RomFS first, however.  Resolve the exact title from the
         * stock metadata and Host content storage before that first request,
         * so the process does not fall through to an SD-only data lookup. */
        /*
         * Do not use IsApplicationId here.  The FS service supplies the
         * actual process ProgramId, and a valid installed title may use a
         * non-standard id.  System ids remain excluded; the metadata + Host
         * presence checks below are the authoritative USB-content guard.
         */
        if (ncm::IsSystemProgramId(program_id)) {
            return false;
        }

        char stock_path[ams::fs::EntryNameLengthMax + 1]{};
        if (R_FAILED(GetUsbProgramContentPathImpl(stock_path, sizeof(stock_path), program_id, ncm::ContentType::Program))) {
            return false;
        }

        char usb_path[ams::fs::EntryNameLengthMax + 1]{};
        if (R_FAILED(TranslateContentPathToUsb(usb_path, sizeof(usb_path), stock_path))) {
            return false;
        }

        RememberProgramPath(program_id, usb_path);
        return true;
    }

    bool HasUsbProgramPath(ncm::ProgramId program_id) {
        return EnsureUsbProgramPath(program_id);
    }

    bool HasCachedUsbProgramPath(ncm::ProgramId program_id) {
        char cached_path[ams::fs::EntryNameLengthMax + 1]{};
        return FindProgramPath(cached_path, sizeof(cached_path), program_id);
    }

    bool PrimeUsbProgramPathFromPersistentMap(ncm::ProgramId program_id) {
        if (program_id == ncm::InvalidProgramId || ncm::IsSystemProgramId(program_id)) {
            return false;
        }
        if (HasCachedUsbProgramPath(program_id)) {
            return true;
        }

        /* This is used only by NS/applet observers after their stock request
         * has completed. Reading these two tiny files from SD is safe there;
         * FS hooks continue to use HasCachedUsbProgramPath() exclusively and
         * therefore cannot recurse into fsp-srv. */
        if (!ncm::IsUsbTitleMapped(program_id)) {
            return false;
        }

        ncm::ContentId meta_content_id{};
        ncm::ContentId control_content_id{};
        ncm::ContentId program_content_id{};
        if (R_FAILED(ncm::GetUsbTitleContentIds(program_id,
                std::addressof(meta_content_id),
                std::addressof(control_content_id),
                std::addressof(program_content_id)))
            || program_content_id == ncm::InvalidContentId) {
            return false;
        }

        RememberMappedTitleContentIds(program_id, meta_content_id,
            control_content_id, program_content_id);
        return CacheMappedProgramPath(program_id);
    }

    ncm::ProgramId GetLastCachedUsbProgramIdForDiagnostics() {
        return ncm::ProgramId{g_last_cached_program_id.load(std::memory_order_acquire)};
    }

    Result OpenUsbProgramRomFs(std::shared_ptr<ams::fs::IStorage> *out, ncm::ProgramId program_id) {
        g_runtime_romfs_attempts.fetch_add(1, std::memory_order_relaxed);
        const Result open_result = [&]() -> Result {
            R_UNLESS(out != nullptr, usb::ResultInvalidParameter());

            if (const auto snapshot = FindLaunchSnapshot(program_id); snapshot) {
                R_RETURN(OpenLaunchProgramStorage(out, nullptr, *snapshot, fssystem::NcaFsHeader::FsType::RomFs));
            }

            char path[ams::fs::EntryNameLengthMax + 1]{};
            R_UNLESS(EnsureUsbProgramPath(program_id), ams::fs::ResultPathNotFound());
            R_UNLESS(FindProgramPath(path, sizeof(path), program_id), ams::fs::ResultPathNotFound());

            std::shared_ptr<fssystem::NcaReader> reader;
            R_TRY(OpenUsbNcaReader(std::addressof(reader), path, true));
            R_UNLESS(reader->GetContentType() == fssystem::NcaHeader::ContentType::Program, ams::fs::ResultPreconditionViolation());
            R_UNLESS(reader->GetProgramId() == program_id.value, ams::fs::ResultInvalidArgument());
            R_RETURN(OpenUsbNcaStorage(out, std::move(reader), fssystem::NcaFsHeader::FsType::RomFs));
        }();

        g_runtime_romfs_last_result.store(open_result.GetValue(), std::memory_order_relaxed);
        if (R_SUCCEEDED(open_result)) {
            g_runtime_romfs_successes.fetch_add(1, std::memory_order_relaxed);
        }
        R_RETURN(open_result);
    }

    bool HasUsbProcessContent(u64 process_id, ncm::ProgramId program_id) {
        return FindProcessContent(process_id, program_id) != nullptr;
    }

    Result OpenPinnedProcessRomFs(std::shared_ptr<ams::fs::IStorage> *out, LaunchPin *pin,
                                  u64 process_id, ncm::ProgramId program_id) {
        const auto snapshot = FindProcessContent(process_id, program_id);
        R_UNLESS(out != nullptr && snapshot != nullptr, ams::fs::ResultPathNotFound());
        /* No fallback on ejection, integrity or IO errors: code and resources
         * must remain from the same immutable USB launch. */
        R_TRY(OpenLaunchProgramStorage(out, nullptr, *snapshot, fssystem::NcaFsHeader::FsType::RomFs, pin ? snapshot : LaunchPin{}));
        if (pin) *pin = snapshot;
        R_SUCCEED();
    }

    Result OpenUsbProcessRomFs(std::shared_ptr<ams::fs::IStorage> *out, u64 process_id, ncm::ProgramId program_id) {
        R_RETURN(OpenPinnedProcessRomFs(out, nullptr, process_id, program_id));
    }

    void QueueWakeComparison(const LaunchPin &pin, u32 open_epoch, u32 read_epoch,
                             s64 offset, size_t size, Result original) {
        using namespace wake_compare;
        if (!pin) return;
        std::scoped_lock lock(g_comparison_mutex);
        // A failed read may return just after the USB gate opens but before
        // the worker publishes its proof. Only the queue can run this early.
        if (!Eligible(open_epoch, read_epoch, game_wake::wake_epoch.load(std::memory_order_acquire), offset, size)) return;
        // A large error is recorded. A later eligible read in the same epoch
        // can still be compared, but no second comparison can run.
        if (g_comparison.epoch == read_epoch && (g_comparison.stage != Stage::TooLarge || size > MaxRead)) return;
        g_comparison = {pin, read_epoch, offset, size, original.GetValue(),
                        size > MaxRead ? Stage::TooLarge : Stage::Queued, {}};
        g_comparison_revision.fetch_add(1, std::memory_order_release);
    }

    namespace {
        void RunWakeComparison() {
            using namespace wake_compare;
            Comparison job;
            {
                std::scoped_lock lock(g_comparison_mutex);
                if (g_comparison.stage != Stage::Queued) return;
                g_comparison.stage = Stage::Running;
                g_comparison_revision.fetch_add(1, std::memory_order_release);
                job = g_comparison;
            }
            // Never hold the report mutex during USB I/O. A new wake may
            // supersede this sample, and the independent logger must stay live.
            const auto valid = [&] {
                return game_wake::wake_epoch.load(std::memory_order_acquire) == job.epoch &&
                    ready_epoch.load(std::memory_order_acquire) == job.epoch &&
                    !system_power::IsBackgroundPaused() && !system_power::HasFailed() &&
                    R_SUCCEEDED(CheckLaunchGeneration(*job.pin));
            };
            std::unique_ptr<u8[]> scratch(new (std::nothrow) u8[job.size]);
            std::shared_ptr<ams::fs::IStorage> fresh;
            job.stage = scratch ? Compare(job.fresh, valid,
                [&] { return OpenLaunchProgramStorage(&fresh, nullptr, *job.pin, fssystem::NcaFsHeader::FsType::RomFs, job.pin).GetValue(); },
                [&] { return fresh->Read(job.offset, scratch.get(), job.size).GetValue(); }) : Stage::NoMemory;
            std::scoped_lock lock(g_comparison_mutex);
            if (g_comparison.epoch == job.epoch) {
                g_comparison = std::move(job);
                g_comparison_revision.fetch_add(1, std::memory_order_release);
            }
        }
    }

    namespace {
        void RunPatchWitness() {
            const u32 epoch = game_wake::wake_epoch.load(std::memory_order_acquire);
            if (epoch == 0 || epoch != wake_compare::ready_epoch.load(std::memory_order_acquire) ||
                system_power::IsBackgroundPaused() || system_power::HasFailed()) return;
            LaunchPin pin;
            {
                std::scoped_lock lock(g_comparison_mutex);
                auto &w = g_patch_witness;
                if (!w.pin || w.cold_epoch >= epoch || w.attempted_epoch == epoch || w.sample_epoch == epoch || g_active_sample) return;
                w.attempted_epoch = epoch;
                w.sample_epoch = 0;
                w.open_result = UINT32_MAX;
                w.same_extent = w.same_expected = w.same_cipher = w.same_plain = w.verified = -1;
                g_comparison_revision.fetch_add(1, std::memory_order_release);
                pin = w.pin;
            }
            // Only probe content still bound to a live USB launch. The reader
            // validates the original generation, base and patch content IDs.
            bool bound = false;
            {
                std::scoped_lock lock(g_process_content_mutex);
                for (const auto &entry : g_process_contents) {
                    if (entry.process_id && entry.snapshot == pin) { bound = true; break; }
                }
            }
            if (!bound || R_FAILED(CheckLaunchGeneration(*pin))) return;
            std::shared_ptr<ams::fs::IStorage> fresh;
            const auto result = OpenLaunchProgramStorage(&fresh, nullptr, *pin, fssystem::NcaFsHeader::FsType::RomFs, pin);
            std::scoped_lock lock(g_comparison_mutex);
            auto &w = g_patch_witness;
            if (w.pin == pin && w.attempted_epoch == epoch && game_wake::wake_epoch.load(std::memory_order_acquire) == epoch) {
                w.open_result = result.GetValue();
                g_comparison_revision.fetch_add(1, std::memory_order_release);
            }
        }

        int AppendPatchWitness(char *out, size_t capacity, int used) {
            if (used < 0 || static_cast<size_t>(used) >= capacity) return -1;
            PatchWitness w;
            {
                std::scoped_lock lock(g_comparison_mutex);
                w = g_patch_witness;
            }
            const auto patch = ncm::GetContentIdString(w.pin ? w.pin->content.patch : ncm::InvalidContentId);
            const auto room = capacity - static_cast<size_t>(used);
            const int count = util::SNPrintf(out + used, room,
                "usb_patch_witness_baseline=%u\nusb_patch_witness_patch=%s\nusb_patch_witness_section=%d\n"
                "usb_patch_witness_offset=%lld\nusb_patch_witness_size=%zu\nusb_patch_witness_cold_epoch=%u\n"
                "usb_patch_witness_attempt_epoch=%u\nusb_patch_witness_sample_epoch=%u\nusb_patch_witness_open=0x%08X\n"
                "usb_patch_witness_same_extent=%d\nusb_patch_witness_same_header_hash=%d\nusb_patch_witness_same_cipher=%d\n"
                "usb_patch_witness_same_plain=%d\nusb_patch_witness_wake_verified=%d\n",
                w.pin ? 1u : 0u, patch.data, w.section, static_cast<long long>(w.baseline.offset), w.baseline.size,
                w.cold_epoch, w.attempted_epoch, w.sample_epoch, w.open_result, w.same_extent,
                w.same_expected, w.same_cipher, w.same_plain, w.verified);
            return count < 0 || static_cast<size_t>(count) >= room ? -1 : used + count;
        }
    }

    int wake_compare::Format(char *out, size_t capacity) {
        constexpr const char *names[] = {"idle", "queued", "running", "too_large", "stale", "open_failed",
                                        "read_failed", "verified", "no_memory"};
        Comparison job;
        {
            std::scoped_lock lock(g_comparison_mutex);
            job = g_comparison;
        }
        const auto base = ncm::GetContentIdString(job.pin ? job.pin->content.base : ncm::InvalidContentId);
        const auto patch = ncm::GetContentIdString(job.pin ? job.pin->content.patch : ncm::InvalidContentId);
        const int used = util::SNPrintf(out, capacity,
            "usb_romfs_compare=%s\nusb_romfs_compare_epoch=%u\nusb_romfs_compare_title=%016llX\n"
            "usb_romfs_compare_base=%s\nusb_romfs_compare_patch=%s\n"
            "usb_romfs_compare_generation=%llu\nusb_romfs_compare_offset=%lld\nusb_romfs_compare_size=%zu\n"
            "usb_romfs_compare_original=0x%08X\nusb_romfs_compare_open=0x%08X\nusb_romfs_compare_read=0x%08X\n",
            names[static_cast<u32>(job.stage)], job.epoch,
            static_cast<unsigned long long>(job.pin ? job.pin->content.program_id : 0),
            base.data, patch.data,
            static_cast<unsigned long long>(job.pin ? job.pin->generation : 0), static_cast<long long>(job.offset),
            job.size, job.original, job.fresh.open, job.fresh.read);
        return AppendPatchWitness(out, capacity, used);
    }

    bool HasUsbAddOnContent(ncm::ProgramId client, ncm::DataId data_id, u8 storage_id, u64 process_id) {
        const auto snapshot = FindProcessContent(process_id, client);
        if (!snapshot) return false;
        for (u32 i = 0; i < snapshot->content.add_on_count; ++i) {
            const auto &addon = snapshot->content.add_ons[i];
            if (addon.data_id == data_id.value && (addon.storage_id == storage_id
                || storage_id == static_cast<u8>(ncm::StorageId::None))) return true;
        }
        return false;
    }

    Result OpenUsbAddOnRomFs(std::shared_ptr<ams::fs::IStorage> *out, ncm::ProgramId client,
                            ncm::DataId data_id, u8 storage_id, u64 process_id) {
        const auto snapshot = FindProcessContent(process_id, client);
        R_UNLESS(snapshot != nullptr, ams::fs::ResultPathNotFound());
        for (u32 i = 0; i < snapshot->content.add_on_count; ++i) {
            const auto &addon = snapshot->content.add_ons[i];
            if (addon.data_id != data_id.value || (addon.storage_id != storage_id
                && storage_id != static_cast<u8>(ncm::StorageId::None))) continue;
            std::shared_ptr<fssystem::NcaReader> reader;
            R_TRY(OpenLaunchReader(std::addressof(reader), *snapshot, addon.content_id,
                fssystem::NcaHeader::ContentType::Data, data_id.value));
            R_TRY(OpenUsbNcaStorage(out, std::move(reader), fssystem::NcaFsHeader::FsType::RomFs));
            R_RETURN(CheckLaunchGeneration(*snapshot));
        }
        R_THROW(ams::fs::ResultPathNotFound());
    }

    Result ReadMappedUsbNcaHeader(UsbNcaHeaderInfo *out, const char *content_path) {
        R_UNLESS(out != nullptr && content_path != nullptr, usb::ResultInvalidParameter());
        *out = {};
        R_UNLESS(IsUsbContentLaunchPathEnabled(), sm::mitm::ResultShouldForwardToSession());

        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        R_UNLESS(R_SUCCEEDED(TranslateContentPathToUsb(mounted_path, sizeof(mounted_path), content_path)),
            sm::mitm::ResultShouldForwardToSession());
        ncm::ContentId requested_content_id{};
        R_UNLESS(TryGetRegisteredPathContentId(std::addressof(requested_content_id), mounted_path),
            sm::mitm::ResultShouldForwardToSession());

        ncm::ProgramId expected_program_id{};
        fssystem::NcaHeader::ContentType expected_type{};
        bool matched = false;
        {
            /* Cache only: never call NCM or read the persistent SD map while
             * servicing an FS request. Drop the cache lock before USB I/O. */
            std::scoped_lock lk(g_mapped_title_mutex);
            for (size_t i = 0; i < g_cached_mapped_title_count; ++i) {
                const auto &entry = g_cached_mapped_titles[i];
                const auto match = [&](bool present, const ncm::ContentId &id) {
                    return present && impl::MatchesUsbContentIdentity(entry.program_id.value, id, 0, requested_content_id);
                };
                fssystem::NcaHeader::ContentType type{};
                if (match(entry.has_expected_program_content_id, entry.expected_program_content_id)) {
                    type = fssystem::NcaHeader::ContentType::Program;
                } else if (match(entry.has_expected_control_content_id, entry.expected_control_content_id)) {
                    type = fssystem::NcaHeader::ContentType::Control;
                } else if (match(entry.has_expected_meta_content_id, entry.expected_meta_content_id)) {
                    type = fssystem::NcaHeader::ContentType::Meta;
                } else {
                    continue;
                }
                R_UNLESS(!matched || (expected_program_id == entry.program_id && expected_type == type),
                    sm::mitm::ResultShouldForwardToSession());
                matched = true;
                expected_program_id = entry.program_id;
                expected_type = type;
            }
        }
        const Result identity_result = matched
            ? static_cast<Result>(ResultSuccess())
            : static_cast<Result>(ncm::ResultContentNotFound());
        RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::FsUsbHeaderIdentityResult,
            expected_program_id, matched ? static_cast<u8>(expected_type) : 0,
            identity_result, content_path, static_cast<u8>(ncm::StorageId::SdCard),
            static_cast<u8>(ncm::StorageId::Host));
        R_UNLESS(matched, sm::mitm::ResultShouldForwardToSession());

        /* Only the original NCA header is needed. Do not wait for or fabricate
         * a title key: FS asks for the rights ID before ES supplies that key. */
        std::shared_ptr<fssystem::NcaReader> reader;
        R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, false));
        R_UNLESS(reader->GetProgramId() == expected_program_id.value && reader->GetContentType() == expected_type,
            ams::fs::ResultInvalidArgument());
        UsbNcaHeaderInfo info{};
        info.program_id = expected_program_id;
        reader->GetRightsId(info.rights_id.data, sizeof(info.rights_id.data));
        info.key_generation = reader->GetKeyGeneration();
        *out = info;
        R_SUCCEED();
    }

    Result ReadUsbNcaHeaderByPath(UsbNcaHeaderInfo *out, const char *content_path) {
        R_UNLESS(out != nullptr && content_path != nullptr, usb::ResultInvalidParameter());
        *out = {};
        u64 generation = 0;
        R_UNLESS(IsUsbContentFileSystemRouteReady(std::addressof(generation), false),
            sm::mitm::ResultShouldForwardToSession());
        AMS_UNUSED(generation);

        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        R_UNLESS(R_SUCCEEDED(TranslateContentPathToUsb(
            mounted_path, sizeof(mounted_path), content_path)),
            sm::mitm::ResultShouldForwardToSession());
        constexpr const char RegisteredPrefix[] = "usbcontent:/registered/";
        R_UNLESS(std::strncmp(mounted_path, RegisteredPrefix,
            sizeof(RegisteredPrefix) - 1) == 0,
            sm::mitm::ResultShouldForwardToSession());

        std::shared_ptr<fssystem::NcaReader> reader;
        R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, false));
        UsbNcaHeaderInfo info{};
        info.program_id = ncm::ProgramId{reader->GetProgramId()};
        R_UNLESS(!ncm::IsSystemProgramId(info.program_id),
            sm::mitm::ResultShouldForwardToSession());
        reader->GetRightsId(info.rights_id.data, sizeof(info.rights_id.data));
        info.key_generation = reader->GetKeyGeneration();
        *out = info;
        R_SUCCEED();
    }

    Result ValidateUsbVerificationAddOn(ams::fs::fsa::IFileSystem &filesystem, u64 header_id, u64 title) {
        /* The NCA's already verified local PFS, never an SD/NCM lookup.
         * Bound directory work, require one CNMT, and keep all paths local. */
        ams::fs::Path root;
        R_TRY(root.SetShallowBuffer("/"));
        std::unique_ptr<ams::fs::fsa::IDirectory> directory;
        R_TRY(filesystem.OpenDirectory(&directory, root, ams::fs::OpenDirectoryMode_File));
        s64 count = 0;
        R_TRY(directory->GetEntryCount(&count));
        R_UNLESS(count > 0 && count <= 16, ams::fs::ResultInvalidArgument());
        char name[ams::fs::EntryNameLengthMax+2]{};
        for (s64 i = 0; i < count; ++i) {
            ams::fs::DirectoryEntry entry{};
            s64 read = 0;
            R_TRY(directory->Read(&read, &entry, 1));
            R_UNLESS(read == 1 && std::memchr(entry.name, 0, sizeof(entry.name)), ams::fs::ResultInvalidArgument());
            const size_t length = std::strlen(entry.name);
            R_UNLESS(!std::strchr(entry.name, '/') && !std::strchr(entry.name, '\\') &&
                !std::strchr(entry.name, ':'), ams::fs::ResultInvalidArgument());
            if (length > 5 && std::strcmp(entry.name+length-5, ".cnmt") == 0) {
                R_UNLESS(!name[0], ams::fs::ResultInvalidArgument());
                name[0] = '/';
                std::memcpy(name+1, entry.name, length+1);
            }
        }
        R_UNLESS(name[0], ams::fs::ResultPathNotFound());
        ams::fs::Path path;
        R_TRY(path.SetShallowBuffer(name));
        std::unique_ptr<ams::fs::fsa::IFile> file;
        R_TRY(filesystem.OpenFile(&file, path, ams::fs::OpenMode_Read));
        s64 size = 0;
        R_TRY(file->GetSize(&size));
        R_UNLESS(size >= static_cast<s64>(verification::AddOnPrefixSize) &&
            size <= static_cast<s64>(verification::AddOnMaxSize), ams::fs::ResultInvalidArgument());
        u8 prefix[verification::AddOnPrefixSize]{};
        size_t read = 0;
        R_TRY(file->Read(&read, 0, prefix, sizeof(prefix)));
        R_UNLESS(read == sizeof(prefix) && verification::AddOnAssociation(prefix, read, size, header_id, title),
            ams::fs::ResultInvalidArgument());
        /* Trigger section integrity checks for every CNMT byte, including
         * records and digest, before publishing this filesystem. */
        u8 block[512];
        for (s64 offset = sizeof(prefix); offset < size;) {
            const size_t amount = static_cast<size_t>(std::min<s64>(sizeof(block), size-offset));
            R_TRY(file->Read(&read, offset, block, amount));
            R_UNLESS(read == amount, ams::fs::ResultInvalidArgument());
            offset += amount;
        }
        R_SUCCEED();
    }

    Result OpenUsbVerificationFileSystem(std::shared_ptr<ams::fs::fsa::IFileSystem> *out,
        const char *path, size_t path_bound, ncm::ProgramId requested, ncm::ProgramId title, u32 type) {
        R_UNLESS(out != nullptr && ncm::IsApplicationId(title) && !ncm::IsSystemProgramId(title), ams::fs::ResultInvalidArgument());
        char mounted[ams::fs::EntryNameLengthMax+1]{};
        R_UNLESS(verification::Translate(mounted, sizeof(mounted), path, path_bound), ams::fs::ResultPathNotFound());
        u64 generation = 0;
        R_UNLESS(IsUsbContentFileSystemRouteReady(&generation, false), ams::fs::ResultPathNotFound());
        R_TRY(CheckImageScanGeneration(generation));
        std::shared_ptr<fssystem::NcaReader> reader;
        /* Do not start USB, scan titles, consult SD maps, accept unsigned
         * headers, or apply layered mods during a native integrity check. */
        R_TRY(OpenUsbNcaReader(&reader, mounted, true, true, true));
        const u64 archive_id = reader->GetProgramId();
        const bool related = verification::Identity(archive_id, requested.value, title.value);
        /* Only Meta may prove a DLC relationship from its signed CNMT. Other
         * extra identities remain unsupported. No title-ID prefix shortcut. */
        R_UNLESS(related || (type == 5 && static_cast<u8>(reader->GetContentType()) == 1 &&
            ncm::IsApplicationId(ncm::ProgramId{archive_id}) && !ncm::IsSystemProgramId(ncm::ProgramId{archive_id}) &&
            (requested.value == 0 || requested.value == archive_id)), ams::fs::ResultInvalidArgument());
        const auto section = verification::Select(type, static_cast<u8>(reader->GetContentType()));
        R_UNLESS(section.index >= 0 && reader->HasFsInfo(section.index), ams::fs::ResultInvalidNcaFileSystemType());
        fssystem::NcaFsHeaderReader header;
        R_TRY(header.Initialize(*reader, section.index));
        const auto wanted = section.partition ? fssystem::NcaFsHeader::FsType::PartitionFs : fssystem::NcaFsHeader::FsType::RomFs;
        R_UNLESS(header.GetFsType() == wanted, ams::fs::ResultInvalidNcaFileSystemType());
        std::shared_ptr<ams::fs::IStorage> storage;
        std::shared_ptr<fssystem::IAsynchronousAccessSplitter> splitter;
        auto *creators = fssystem::GetFileSystemCreatorInterfaces();
        /* This is the normal decryption + section hash/integrity stack. */
        R_TRY(creators->storage_on_nca_creator->Create(&storage, &splitter, &header, std::move(reader), section.index));
        std::shared_ptr<ams::fs::fsa::IFileSystem> result;
        if (section.partition) { R_TRY(creators->partition_fs_creator->Create(&result, std::move(storage))); }
        else { R_TRY(creators->rom_fs_creator->Create(&result, std::move(storage))); }
        if (!related) { R_TRY(ValidateUsbVerificationAddOn(*result, archive_id, title.value)); }
        R_TRY(CheckImageScanGeneration(generation));
        u64 current = 0;
        R_UNLESS(IsUsbContentFileSystemRouteReady(&current, false) && current == generation, usb::ResultMediaChanged());
        *out = std::move(result);
        R_SUCCEED();
    }

    Result OpenUsbControlFileSystemByPath(
        std::shared_ptr<ams::fs::fsa::IFileSystem> *out,
        ncm::ProgramId *out_resolved_program_id,
        const char *content_path,
        ncm::ProgramId requested_program_id) {
        R_UNLESS(out != nullptr && out_resolved_program_id != nullptr
            && content_path != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(ncm::IsApplicationId(requested_program_id)
            && !ncm::IsSystemProgramId(requested_program_id), ams::fs::ResultInvalidArgument());
        u64 generation = 0;
        R_UNLESS(IsUsbContentFileSystemRouteReady(std::addressof(generation), false),
            usb::ResultOperationDenied());
        R_TRY(CheckImageScanGeneration(generation));

        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        R_UNLESS(verification::Translate(mounted_path, sizeof(mounted_path), content_path,
            ams::fs::EntryNameLengthMax + 1), ams::fs::ResultPathNotFound());

        std::shared_ptr<fssystem::NcaReader> reader;
        /* Snapshot-only route: do not start USB, scan a library, or consult
         * NCM/SD maps recursively from an FS hook. Keep the existing header
         * policy and the normal decrypted, integrity-checked RomFS reader. */
        R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, true, true));
        R_UNLESS(reader->GetContentType() == fssystem::NcaHeader::ContentType::Control,
            ams::fs::ResultPreconditionViolation());
        const ncm::ProgramId resolved_program_id{reader->GetProgramId()};
        R_UNLESS(resolved_program_id == requested_program_id,
            ams::fs::ResultInvalidArgument());

        std::shared_ptr<ams::fs::IStorage> storage;
        R_TRY(OpenUsbNcaStorage(std::addressof(storage), std::move(reader),
            fssystem::NcaFsHeader::FsType::RomFs));
        std::shared_ptr<ams::fs::fsa::IFileSystem> result;
        R_TRY(fssystem::GetFileSystemCreatorInterfaces()->rom_fs_creator->Create(
            &result, std::move(storage)));
        R_TRY(CheckImageScanGeneration(generation));
        u64 current = 0;
        R_UNLESS(IsUsbContentFileSystemRouteReady(&current, false) && current == generation,
            usb::ResultMediaChanged());
        *out = std::move(result);
        *out_resolved_program_id = resolved_program_id;
        R_SUCCEED();
    }

    Result OpenUsbControlFileSystem(std::shared_ptr<ams::fs::fsa::IFileSystem> *out, ncm::ProgramId *out_resolved_program_id, const char *content_path, ncm::ProgramId requested_program_id) {
        R_UNLESS(out != nullptr && out_resolved_program_id != nullptr && content_path != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(IsUsbContentLaunchPathEnabled(), usb::ResultOperationDenied());
        *out_resolved_program_id = requested_program_id;

        /* Host ContentStorage deliberately exposes either the provider path
         * itself or a stock-looking SD path. Horizon's FS cannot resolve the
         * process-local usbcontent: mount, and the compatibility path would
         * reopen the NCA from the SD card. Translate only the exact NCM
         * content root here; all other paths remain on the stock FS route. */
        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(TranslateContentPathToUsb(mounted_path, sizeof(mounted_path), content_path));

        /* Establish identity before doing USB I/O. In R16, an unrelated SD
         * forwarder's missing USB file was replaced with Farlands solely
         * because Farlands was the only mapped title. That also persisted a
         * false alias. A cache miss now forwards untouched, with no scan and
         * no callback into NCM from this active FS request. */
        ncm::ProgramId mapped_program_id{};
        R_UNLESS(ResolveMappedControlProgramId(std::addressof(mapped_program_id), mounted_path, requested_program_id), ams::fs::ResultPathNotFound());

        std::shared_ptr<fssystem::NcaReader> reader;
        R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, true));
        R_UNLESS(reader->GetContentType() == fssystem::NcaHeader::ContentType::Control, ams::fs::ResultPreconditionViolation());
        const ncm::ProgramId resolved_program_id{reader->GetProgramId()};
        R_UNLESS(resolved_program_id == mapped_program_id, ams::fs::ResultInvalidArgument());
        *out_resolved_program_id = resolved_program_id;
        MarkUsbLaunchActiveForDiagnostics(resolved_program_id);

        std::shared_ptr<ams::fs::IStorage> storage;
        R_TRY(OpenUsbNcaStorage(std::addressof(storage), std::move(reader), fssystem::NcaFsHeader::FsType::RomFs));
        R_TRY(fssystem::GetFileSystemCreatorInterfaces()->rom_fs_creator->Create(out, std::move(storage)));

        /* This exact mapped Control NCA is the provenance for the next opaque
         * Host launch handoff. Do this only after the archive was validated
         * and mounted successfully. */
        const bool program_path_cached = CacheMappedProgramPath(resolved_program_id);
        Result program_path_cache_result = ResultSuccess();
        if (!program_path_cached) {
            program_path_cache_result = ams::fs::ResultPathNotFound();
        }
        RecordLaunchBoundaryTrace(FsMappedProgramPathCache, resolved_program_id,
            static_cast<u8>(ncm::ContentType::Program),
            program_path_cache_result,
            program_path_cached ? "mapped Program path cached" : "mapped Program content ID missing");
        RememberPendingControlProgram(resolved_program_id);
        R_SUCCEED();
    }

    Result CacheExternalKey(const ams::fs::RightsId &rights_id, const spl::AccessKey &access_key) {
        R_RETURN(g_external_key_manager.Register(rights_id, access_key));
    }

    Result RemoveExternalKey(const ams::fs::RightsId &rights_id) {
        R_RETURN(g_external_key_manager.Unregister(rights_id));
    }

    Result RemoveAllExternalKeys() {
        R_RETURN(g_external_key_manager.UnregisterAll());
    }

    Result HomebrewService::Start() {
        R_RETURN(EnsureUsbStarted());
    }

    Result HomebrewService::GetStatus(sf::Out<HomebrewServiceStatus> out) {
        const ams::usb::pause_diagnostics::Scope pause(ams::usb::pause_diagnostics::Stage::RequestStatus);
        g_shared_status_queries.fetch_add(1, std::memory_order_relaxed);
        ams::usb::storage::Status storage_status{};
        ams::usb::filesystem::Status status{};
        const auto storage_result = g_storage_service.GetStatus(std::addressof(storage_status));
        const auto result = g_filesystem_service.GetStatus(std::addressof(status));

        HomebrewServiceStatus service_status{};
        /* Status byte 3, bit 0: commands 7/14 accept up to 4 MiB.
         * Old clients ignore this formerly-reserved byte and remain valid. */
        service_status.reserved = 1;
        service_status.mounted = R_SUCCEEDED(result) && status.mounted;
        service_status.ready = R_SUCCEEDED(result) && status.ready;
        service_status.state = static_cast<u8>(storage_status.state);
        service_status.result = R_SUCCEEDED(storage_result) && R_SUCCEEDED(result) ? status.last_result : static_cast<Result>(R_FAILED(storage_result) ? storage_result : result).GetValue();
        static_cast<void>(g_filesystem_service.GetMediaGeneration(std::addressof(service_status.media_generation)));
        g_shared_status_result.store(service_status.result, std::memory_order_relaxed);
        g_shared_status_flags.store((service_status.mounted ? 1u : 0u)
            | (service_status.ready ? 2u : 0u) | (service_status.media_generation != 0 ? 4u : 0u), std::memory_order_relaxed);
        if (service_status.mounted && service_status.ready && service_status.media_generation != 0) {
            g_shared_ready_replies.fetch_add(1, std::memory_order_relaxed);
        }
        out.SetValue(service_status);

        R_RETURN(result);
    }

    Result HomebrewService::GetNroInfo(sf::Out<ams::usb::homebrew::NroInfo> out, const sf::InBuffer &path) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());

        char nro_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(nro_path, sizeof(nro_path), path));

        ams::usb::homebrew::NroReader reader;
        R_TRY(reader.Open(std::addressof(g_filesystem_service), nro_path));
        ON_SCOPE_EXIT { reader.Close(); };

        ams::usb::homebrew::NroInfo info{};
        R_TRY(reader.GetInfo(std::addressof(info)));
        out.SetValue(info);
        R_SUCCEED();
    }

    Result HomebrewService::ReadNro(sf::Out<u64> out_read, const sf::OutBuffer &out, const sf::InBuffer &path, u64 offset, u64 media_generation) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());

        R_UNLESS(out.GetSize() <= MaxHomebrewReadSize, usb::ResultInvalidParameter());

        char nro_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(nro_path, sizeof(nro_path), path));

        ams::usb::homebrew::NroReader reader;
        R_TRY(reader.Open(std::addressof(g_filesystem_service), nro_path));
        ON_SCOPE_EXIT { reader.Close(); };

        ams::usb::homebrew::NroInfo info{};
        R_TRY(reader.GetInfo(std::addressof(info)));
        R_UNLESS(info.media_generation == media_generation, usb::ResultMediaChanged());

        size_t read_size = 0;
        R_TRY(reader.ReadAt(offset, out.GetPointer(), out.GetSize(), std::addressof(read_size)));
        out_read.SetValue(read_size);
        R_SUCCEED();
    }

    Result HomebrewService::ReadDirectory(sf::Out<u32> out_count, const sf::OutBuffer &out, const sf::InBuffer &path, u32 skip_entries) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());

        R_UNLESS(out.GetSize() % sizeof(HomebrewDirectoryEntry) == 0, usb::ResultInvalidParameter());
        const size_t max_entries = out.GetSize() / sizeof(HomebrewDirectoryEntry);
        R_UNLESS(max_entries > 0 && max_entries <= 32, usb::ResultInvalidParameter());

        char directory_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(directory_path, sizeof(directory_path), path));

        ams::usb::volume::DirectoryEntryInfo entries[32]{};
        size_t entry_count = 0;
        u64 media_generation = 0;
        R_TRY(g_filesystem_service.GetMediaGeneration(std::addressof(media_generation)));
        R_TRY(g_filesystem_service.ReadDirectoryAtGeneration(directory_path, entries, max_entries, skip_entries, true, true, std::addressof(entry_count), media_generation));
        R_UNLESS(entry_count <= max_entries, usb::ResultTransactionError());

        auto *homebrew_entries = reinterpret_cast<HomebrewDirectoryEntry *>(out.GetPointer());
        for (size_t i = 0; i < entry_count; ++i) {
            std::memset(std::addressof(homebrew_entries[i]), 0, sizeof(homebrew_entries[i]));
            std::memcpy(homebrew_entries[i].name, entries[i].name, sizeof(homebrew_entries[i].name));
            homebrew_entries[i].size = entries[i].size;
            homebrew_entries[i].flags = entries[i].directory ? 1 : 0;
        }
        out_count.SetValue(static_cast<u32>(entry_count));
        R_SUCCEED();
    }

    Result HomebrewService::QuerySdIoCounters(const sf::OutBuffer &out) {
        R_UNLESS(out.GetPointer() != nullptr && out.GetSize() == sizeof(NexusSdIoSnapshot), usb::ResultInvalidParameter());
        ams::usb::storage::Status status{};
        const auto rc=g_storage_service.GetStatus(std::addressof(status));
        NexusSdIoSnapshot snapshot{};
        nexus_sd_io::Snapshot(&snapshot,R_SUCCEEDED(rc)?status.media_generation:0);
        std::memcpy(out.GetPointer(),&snapshot,sizeof(snapshot));
        R_SUCCEED();
    }

    Result HomebrewService::QueryUsbPauses(const sf::OutBuffer &out) {
        R_UNLESS(out.GetPointer() != nullptr && out.GetSize() == sizeof(NexusUsbPauseSnapshot), usb::ResultInvalidParameter());
        ams::usb::storage::Status status{};
        R_TRY(g_storage_service.GetStatus(std::addressof(status)));
        NexusUsbPauseSnapshot snapshot{};
        R_UNLESS(ams::usb::pause_diagnostics::Snapshot(std::addressof(snapshot), status.media_generation), usb::ResultResourceBusy());
        std::memcpy(out.GetPointer(), &snapshot, sizeof(snapshot));
        R_SUCCEED();
    }

    Result HomebrewService::QueryUsbCompletion(const sf::OutBuffer &out) {
        R_UNLESS(out.GetPointer() != nullptr && out.GetSize() == sizeof(NexusUsbCompletionSnapshot), usb::ResultInvalidParameter());
        /* Snapshot only: no EnsureUsbStarted, volume lock or drive command. */
        ams::usb::storage::Status status{};
        R_TRY(g_storage_service.GetStatus(std::addressof(status)));
        NexusUsbCompletionSnapshot snapshot{};
        R_UNLESS(ams::usb::completion_diagnostics::Snapshot(std::addressof(snapshot), status.media_generation), usb::ResultResourceBusy());
        std::memcpy(out.GetPointer(), &snapshot, sizeof(snapshot));
        R_SUCCEED();
    }

    Result HomebrewService::QueryUsbWriteCache(const sf::OutBuffer &out) {
        R_UNLESS(out.GetPointer() && out.GetSize()==sizeof(NexusUsbWriteCacheSnapshot),usb::ResultInvalidParameter());
        ams::usb::storage::Status status{};
        R_TRY(g_storage_service.GetStatus(std::addressof(status)));
        NexusUsbWriteCacheSnapshot snapshot{};
        R_UNLESS(ams::usb::write_cache_diagnostics::Snapshot(&snapshot,status.media_generation),usb::ResultResourceBusy());
        R_TRY(g_storage_service.GetStatus(std::addressof(status)));
        R_UNLESS(status.media_generation==snapshot.media_generation,usb::ResultResourceBusy());
        std::memcpy(out.GetPointer(),&snapshot,sizeof(snapshot));
        R_SUCCEED();
    }

    Result HomebrewService::ControlKeepAlive(sf::Out<NexusUsbKeepAliveStatus> out, const sf::ClientProcessId &pid, NexusUsbKeepAliveRequest request) {
        NexusUsbKeepAliveStatus status{};
        R_TRY(g_storage_service.ControlKeepAliveTest(std::addressof(status), pid.GetValue().value, request));
        out.SetValue(status);
        R_SUCCEED();
    }

    Result HomebrewService::ControlWriteTransfer(sf::Out<NexusUsbWriteTransferStatus> out, const sf::ClientProcessId &pid, NexusUsbWriteTransferRequest request) {
        NexusUsbWriteTransferStatus status{};
        R_TRY(g_storage_service.ControlWriteTransferTest(std::addressof(status), pid.GetValue().value, request));
        out.SetValue(status);
        R_SUCCEED();
    }

    Result HomebrewService::ControlCompletionProbe(sf::Out<NexusUsbCompletionProbeStatus> out, const sf::ClientProcessId &pid, NexusUsbCompletionProbeRequest request) {
        NexusUsbCompletionProbeStatus status{};
        R_TRY(g_storage_service.ControlCompletionProbeTest(std::addressof(status), pid.GetValue().value, request));
        out.SetValue(status);
        R_SUCCEED();
    }

    Result HomebrewService::ControlHostWrite(sf::Out<NexusUsbHostWriteStatus> out, const sf::ClientProcessId &pid, NexusUsbHostWriteRequest request) {
        NexusUsbHostWriteStatus status{};
        R_TRY(g_storage_service.ControlHostWriteTest(std::addressof(status), pid.GetValue().value, request));
        out.SetValue(status);
        R_SUCCEED();
    }

    Result HomebrewService::GetStorageStatus(sf::Out<ams::usb::storage::Status> out) {
        ams::usb::storage::Status status{};
        R_TRY(g_storage_service.GetStatus(std::addressof(status)));
        out.SetValue(status);
        R_SUCCEED();
    }

    Result HomebrewService::GetFileInfo(sf::Out<ams::usb::volume::FileInfo> out, const sf::InBuffer &path, ams::usb::content::GenerationRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        g_shared_info_queries.fetch_add(1, std::memory_order_relaxed);
        R_TRY(EnsureUsbStarted());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        ams::usb::volume::FileInfo info{};
        R_TRY(g_storage_service.GetFileInfoAtGeneration(content_path, std::addressof(info), request.media_generation));
        out.SetValue(info);
        R_SUCCEED();
    }

    Result HomebrewService::ReadFile(sf::Out<u64> out_read, const sf::OutBuffer &out, const sf::InBuffer &path, ams::usb::content::IoRequest request) {
        const ams::usb::pause_diagnostics::Scope pause(ams::usb::pause_diagnostics::Stage::RequestRead, out.GetSize());
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        R_UNLESS(out.GetSize() <= MaxContentTransferSize, usb::ResultInvalidParameter());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        size_t read = 0;
        R_TRY(g_storage_service.ReadFileAtGeneration(content_path, request.offset, out.GetPointer(), out.GetSize(), std::addressof(read), request.media_generation));
        out_read.SetValue(static_cast<u64>(read));
        R_SUCCEED();
    }

    Result HomebrewService::ReadContentDirectory(sf::Out<u64> out_count, const sf::OutBuffer &out, const sf::InBuffer &path, ams::usb::content::DirectoryRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        g_shared_directory_queries.fetch_add(1, std::memory_order_relaxed);
        R_TRY(EnsureUsbStarted());
        R_UNLESS(out.GetSize() % sizeof(ams::usb::volume::DirectoryEntryInfo) == 0, usb::ResultInvalidParameter());
        const size_t max_entries = out.GetSize() / sizeof(ams::usb::volume::DirectoryEntryInfo);
        R_UNLESS(max_entries > 0 && max_entries <= 32, usb::ResultInvalidParameter());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        size_t count = 0;
        R_TRY(g_storage_service.ReadDirectoryAtGeneration(content_path,
            reinterpret_cast<ams::usb::volume::DirectoryEntryInfo *>(out.GetPointer()),
            max_entries, request.skip_entries, request.include_directories != 0,
            request.include_files != 0, std::addressof(count), request.media_generation));
        out_count.SetValue(static_cast<u64>(count));
        R_SUCCEED();
    }

    Result HomebrewService::GetSpaceSize(sf::Out<ams::usb::content::SpaceInfo> out) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        ams::usb::content::SpaceInfo info{};
        R_TRY(g_storage_service.GetSpaceSize(std::addressof(info.free_size), std::addressof(info.total_size)));
        out.SetValue(info);
        R_SUCCEED();
    }

    Result HomebrewService::CreateFile(const sf::InBuffer &path, ams::usb::content::SizeRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        R_RETURN(g_storage_service.CreateFileAtGeneration(content_path, request.size, request.media_generation));
    }

    Result HomebrewService::ResizeFile(const sf::InBuffer &path, ams::usb::content::SizeRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        R_RETURN(g_storage_service.ResizeFileAtGeneration(content_path, request.size, request.media_generation));
    }

    Result HomebrewService::DeleteFile(const sf::InBuffer &path, ams::usb::content::GenerationRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        R_RETURN(g_storage_service.DeleteFileAtGeneration(content_path, request.media_generation));
    }

    Result HomebrewService::RenameFile(const sf::InBuffer &old_path, const sf::InBuffer &new_path, ams::usb::content::GenerationRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        char old_content_path[ams::fs::EntryNameLengthMax + 1]{};
        char new_content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(old_content_path, sizeof(old_content_path), old_path));
        R_TRY(CopyHomebrewPath(new_content_path, sizeof(new_content_path), new_path));
        R_RETURN(g_storage_service.RenameFileAtGeneration(old_content_path, new_content_path, request.media_generation));
    }

    Result HomebrewService::WriteFile(sf::Out<u64> out_written, const sf::InBuffer &data, const sf::InBuffer &path, ams::usb::content::IoRequest request) {
        const ams::usb::pause_diagnostics::Scope pause(ams::usb::pause_diagnostics::Stage::RequestWrite, data.GetSize());
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        R_UNLESS(data.GetSize() <= MaxContentTransferSize, usb::ResultInvalidParameter());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        size_t written = 0;
        R_TRY(g_storage_service.WriteFileAtGeneration(content_path, request.offset, data.GetPointer(), data.GetSize(), std::addressof(written), request.media_generation));
        out_written.SetValue(static_cast<u64>(written));
        R_SUCCEED();
    }

    Result HomebrewService::CreateDirectory(const sf::InBuffer &path, ams::usb::content::GenerationRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        char content_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyHomebrewPath(content_path, sizeof(content_path), path));
        R_RETURN(g_storage_service.CreateDirectoryAtGeneration(content_path, request.media_generation));
    }

    Result HomebrewService::Flush(ams::usb::content::GenerationRequest request) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbRequest);
        R_TRY(EnsureUsbStarted());
        R_RETURN(g_storage_service.FlushAtGeneration(request.media_generation));
    }

    Result HomebrewService::OpenCodeFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, const sf::OutBuffer &verification, const sf::InBuffer &path, ncm::ProgramId program_id) {
        R_UNLESS(verification.GetPointer() != nullptr && verification.GetSize() == sizeof(ams::fs::CodeVerificationData), usb::ResultInvalidParameter());

        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyUsbContentPath(mounted_path, sizeof(mounted_path), path));

        ams::usb::content::LoaderTraceInfo trace_info{};
        trace_info.program_id = program_id.value;
        trace_info.stage = static_cast<u32>(ams::usb::content::LoaderProbeStage::RuntimeCodeOpenAttempt);
        trace_info.result = ResultSuccess().GetValue();
        trace_info.location_storage = static_cast<u8>(ncm::StorageId::Host);
        const size_t trace_path_length = std::min(std::strlen(mounted_path), sizeof(trace_info.path) - 1);
        std::memcpy(trace_info.path, mounted_path, trace_path_length);
        trace_info.path[trace_path_length] = '\x00';
        if (trace_path_length < std::strlen(mounted_path)) {
            trace_info.flags |= ams::usb::content::LoaderTraceFlag_PathTruncated;
        }
        AppendLoaderTraceRecord(trace_info);

        std::shared_ptr<ams::fs::fsa::IFileSystem> code_fs;
        g_runtime_code_attempts.fetch_add(1, std::memory_order_relaxed);
        const Result open_result = OpenUsbCodeFileSystemImpl(std::addressof(code_fs), reinterpret_cast<ams::fs::CodeVerificationData *>(verification.GetPointer()), mounted_path, program_id);
        g_runtime_code_last_result.store(open_result.GetValue(), std::memory_order_relaxed);
        trace_info.stage = static_cast<u32>(ams::usb::content::LoaderProbeStage::RuntimeCodeOpenResult);
        trace_info.result = open_result.GetValue();
        AppendLoaderTraceRecord(trace_info);
        R_TRY(open_result);
        g_runtime_code_successes.fetch_add(1, std::memory_order_relaxed);

        R_TRY(ApplyUsbCachedExefs(&code_fs, program_id));

        auto service_object = sf::CreateSharedObjectEmplaced<ams::fssrv::sf::IFileSystem, ams::fssrv::impl::FileSystemInterfaceAdapter>(std::move(code_fs), false);
        R_UNLESS(service_object != nullptr, ams::fs::ResultAllocationMemoryFailed());
        out.SetValue(std::move(service_object));
        R_SUCCEED();
    }

    Result HomebrewService::OpenLaunchCodeFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out,
            const sf::OutBuffer &verification, const sf::InBuffer &content, ncm::ProgramId program_id) {
        R_UNLESS(verification.GetPointer() != nullptr && verification.GetSize() == sizeof(ams::fs::CodeVerificationData)
            && content.GetPointer() != nullptr && content.GetSize() == sizeof(usb::content::LaunchContent), usb::ResultInvalidParameter());
        R_UNLESS(!ncm::IsSystemProgramId(program_id), usb::ResultInvalidParameter());
        auto snapshot = std::make_shared<UsbLaunchSnapshot>();
        R_UNLESS(snapshot != nullptr, ams::fs::ResultAllocationMemoryFailed());
        std::memcpy(std::addressof(snapshot->content), content.GetPointer(), sizeof(snapshot->content));
        R_UNLESS(usb::content::IsValidLaunchContent(snapshot->content)
            && snapshot->content.program_id == program_id.value, usb::ResultInvalidParameter());
        R_TRY(EnsureUsbStarted());
        R_TRY(g_filesystem_service.GetMediaGeneration(std::addressof(snapshot->generation)));
        std::shared_ptr<ams::fs::IStorage> code_storage;
        g_runtime_code_attempts.fetch_add(1, std::memory_order_relaxed);
        const Result result = OpenLaunchProgramStorage(std::addressof(code_storage),
            reinterpret_cast<ams::fs::CodeVerificationData *>(verification.GetPointer()),
            *snapshot, fssystem::NcaFsHeader::FsType::PartitionFs);
        g_runtime_code_last_result.store(result.GetValue(), std::memory_order_relaxed);
        char detail[128]{};
        util::SNPrintf(detail, sizeof(detail), "R159 patch=%016llx v=%u addons=%u gen=%llu",
            static_cast<unsigned long long>(snapshot->content.patch_id), snapshot->content.patch_version,
            snapshot->content.add_on_count, static_cast<unsigned long long>(snapshot->generation));
        RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::RuntimeCodeOpenResult,
            program_id, static_cast<u8>(ncm::ContentType::Program), result, detail);
        R_TRY(result);
        std::shared_ptr<ams::fs::fsa::IFileSystem> code_fs;
        R_TRY(fssystem::GetFileSystemCreatorInterfaces()->partition_fs_creator->Create(std::addressof(code_fs), std::move(code_storage)));
        R_TRY(ApplyUsbCachedExefs(&code_fs, program_id));
        auto object = sf::CreateSharedObjectEmplaced<fssrv::sf::IFileSystem, fssrv::impl::FileSystemInterfaceAdapter>(std::move(code_fs), false);
        R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
        R_TRY(CheckLaunchGeneration(*snapshot));
        char base_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(MakeLaunchContentPath(base_path, sizeof(base_path), snapshot->content.base));
        RememberProgramPath(program_id, base_path);
        RememberLaunchSnapshot(std::move(snapshot));
        out.SetValue(std::move(object));
        g_runtime_code_successes.fetch_add(1, std::memory_order_relaxed);
        R_SUCCEED();
    }

    Result HomebrewService::SetProcessContent(usb::content::ProcessContentBinding binding) {
        const auto result = SetProcessContentBinding(binding);
        char detail[128]{};
        util::SNPrintf(detail, sizeof(detail), "R159 process-resource pid=%llu pin=%llu action=%s",
            static_cast<unsigned long long>(binding.process_id), static_cast<unsigned long long>(binding.pin_id),
            binding.process_id != 0 ? "bind" : "release");
        RecordLaunchBoundaryTrace(FsUsbProcessBinding, ncm::ProgramId{binding.program_id}, 0, result, detail);
        R_RETURN(result);
    }

    Result HomebrewService::OpenSdContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out) {
        /* Explicit post-boot NCM bridge. Native FS open only: do not call NCM,
         * EnsureUsbStarted, wait for a drive, or initialize any worker here.
         * ams_mitm's own native FS connection is excluded from interception. */
        auto open = [&]() -> Result {
            FsFileSystem native{};
            R_TRY(fsOpenContentStorageFileSystem(std::addressof(native), FsContentStorageId_SdCard));
            auto stock = std::make_shared<ams::fs::RemoteFileSystem>(native);
            if (!stock) {
                fsFsClose(std::addressof(native));
                R_THROW(ams::fs::ResultAllocationMemoryFailed());
            }
            std::unique_ptr<ams::fs::fsa::IFileSystem> overlay;
            R_TRY(CreateUsbContentOverlayFileSystem(std::addressof(overlay), std::move(stock)));
            std::shared_ptr<ams::fs::fsa::IFileSystem> shared(std::move(overlay));
            auto object = sf::CreateSharedObjectEmplaced<fssrv::sf::IFileSystem, fssrv::impl::FileSystemInterfaceAdapter>(std::move(shared), false);
            R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
            out.SetValue(std::move(object));
            content_stats::bridge_opens.fetch_add(1, std::memory_order_relaxed);
            R_SUCCEED();
        };
        const Result result = open();
        content_stats::ResultRecorded(result.GetValue());
        R_RETURN(result);
    }

    Result HomebrewService::OpenPinnedUsbContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, u64 generation) {
        std::unique_ptr<ams::fs::fsa::IFileSystem> pinned;
        R_TRY(CreatePinnedUsbContentFileSystem(&pinned, generation));
        std::shared_ptr<ams::fs::fsa::IFileSystem> shared(std::move(pinned));
        auto object = sf::CreateSharedObjectEmplaced<fssrv::sf::IFileSystem, fssrv::impl::FileSystemInterfaceAdapter>(std::move(shared), false);
        R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
        out.SetValue(std::move(object));
        R_SUCCEED();
    }

    Result HomebrewService::OpenNativeSdContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out) {
        // Explicit post-boot request only. No NCM calls, route changes, worker
        // initialization or USB wait. This connection is never intercepted.
        FsFileSystem native{};
        R_TRY(fsOpenContentStorageFileSystem(std::addressof(native), FsContentStorageId_SdCard));
        auto stock = std::make_shared<ams::fs::RemoteFileSystem>(native);
        if (!stock) {
            fsFsClose(std::addressof(native));
            R_THROW(ams::fs::ResultAllocationMemoryFailed());
        }
        auto object = sf::CreateSharedObjectEmplaced<fssrv::sf::IFileSystem, fssrv::impl::FileSystemInterfaceAdapter>(std::move(stock), false);
        R_UNLESS(object != nullptr, ams::fs::ResultAllocationMemoryFailed());
        out.SetValue(std::move(object));
        R_SUCCEED();
    }

    Result HomebrewService::GetSdContentHeader(sf::Out<usb::content::ContentHeaderInfo> out, const sf::InBuffer &path, u32 query) {
        R_UNLESS(query == usb::content::HeaderQuery_Rights || query == usb::content::HeaderQuery_Program,
            usb::ResultInvalidParameter());
        R_UNLESS(path.GetPointer() != nullptr && path.GetSize() > 0
            && path.GetSize() <= ams::fs::EntryNameLengthMax + 1, usb::ResultInvalidParameter());
        const char *input = reinterpret_cast<const char *>(path.GetPointer());
        R_UNLESS(std::memchr(input, '\0', path.GetSize()) != nullptr, usb::ResultInvalidParameter());
        const char *suffix = usb::content::GetSdContentNcaSuffix(input);
        R_UNLESS(suffix != nullptr, ams::fs::ResultInvalidPath());
        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        const int length = util::SNPrintf(mounted_path, sizeof(mounted_path), "usbcontent:/%s", suffix);
        R_UNLESS(length > 0 && static_cast<size_t>(length) < sizeof(mounted_path), ams::fs::ResultTooLongPath());

        usb::content::LoaderTraceInfo trace{};
        trace.stage = usb::content::Trace_NcmHeaderBegin;
        trace.location_storage = static_cast<u8>(ncm::StorageId::SdCard);
        trace.metadata_storage = trace.location_storage;
        trace.content_storage = trace.location_storage;
        util::SNPrintf(trace.path, sizeof(trace.path), "NCM header query=%u %s", query, input);
        AppendLoaderTraceRecord(trace);

        usb::content::ContentHeaderInfo info{};
        auto read = [&]() -> Result {
            u64 generation = 0;
            /* Never start USB or wait for the post-boot monitor from NCM. */
            R_UNLESS(IsUsbContentFileSystemRouteReady(std::addressof(generation), false), ams::fs::ResultPathNotFound());
            trace.media_generation = generation;
            R_TRY(CheckImageScanGeneration(generation));
            std::shared_ptr<fssystem::NcaReader> reader;
            /* Same verified header reader as the working Control path. No
             * external title-key wait and no ticket/signature modifications. */
            R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, false, true));
            R_UNLESS(!ncm::IsSystemProgramId(ncm::ProgramId{reader->GetProgramId()}), ams::fs::ResultInvalidArgument());
            R_TRY(CheckImageScanGeneration(generation));
            u64 current = 0;
            R_UNLESS(IsUsbContentFileSystemRouteReady(std::addressof(current), false)
                && current == generation, usb::ResultMediaChanged());
            info.program_id = reader->GetProgramId();
            reader->GetRightsId(info.rights_id, sizeof(info.rights_id));
            info.key_generation = reader->GetKeyGeneration();
            R_SUCCEED();
        };
        const Result result = read();
        trace.stage = usb::content::Trace_NcmHeaderResult;
        trace.result = result.GetValue();
        trace.program_id = info.program_id;
        AppendLoaderTraceRecord(trace);
        R_TRY(result);
        out.SetValue(info);
        R_SUCCEED();
    }

    Result HomebrewService::GetNcaRightsId(sf::Out<ams::fs::RightsId> out, const sf::InBuffer &path) {
        R_TRY(EnsureUsbStarted());

        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyUsbContentPath(mounted_path, sizeof(mounted_path), path));

        /* Reading the NCA header does not require the title key.  Keep this
         * query separate from OpenUsbNcaReader's normal key lookup so NCM can
         * identify the rights id before ES has opened the title. */
        std::shared_ptr<fssystem::NcaReader> reader;
        R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, false));

        ams::fs::RightsId rights_id{};
        reader->GetRightsId(rights_id.data, sizeof(rights_id.data));
        out.SetValue(rights_id);
        R_SUCCEED();
    }

    Result HomebrewService::GetNcaProgramId(sf::Out<ncm::ProgramId> out, const sf::InBuffer &path) {
        R_TRY(EnsureUsbStarted());

        char mounted_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(CopyUsbContentPath(mounted_path, sizeof(mounted_path), path));

        /* Like GetNcaRightsId, this only decrypts and validates the NCA
         * header. It does not need a title key and does not open content data. */
        std::shared_ptr<fssystem::NcaReader> reader;
        R_TRY(OpenUsbNcaReader(std::addressof(reader), mounted_path, false));

        out.SetValue(ncm::ProgramId{reader->GetProgramId()});
        R_SUCCEED();
    }

    Result HomebrewService::GetApplicationContentPath(const sf::OutBuffer &out_path, ams::usb::content::ApplicationContentPathRequest request) {
        R_UNLESS(out_path.GetPointer() != nullptr && out_path.GetSize() > 0, usb::ResultInvalidParameter());

        const ncm::ProgramId program_id{request.program_id};
        const auto content_type = static_cast<ncm::ContentType>(request.content_type);
        char stock_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(GetUsbApplicationContentPath(stock_path, sizeof(stock_path), program_id, content_type));

        /* NCM's public GetPath() intentionally remains stock-compatible so
         * existing callers keep seeing an ordinary content path. PGL needs
         * the explicit provider mount name, otherwise a redirection would
         * simply point back at the SD card. */
        char usb_path[ams::fs::EntryNameLengthMax + 1]{};
        R_TRY(TranslateContentPathToUsb(usb_path, sizeof(usb_path), stock_path));

        const size_t path_length = std::strlen(usb_path);
        R_UNLESS(path_length + 1 <= out_path.GetSize(), ncm::ResultBufferInsufficient());
        std::memcpy(out_path.GetPointer(), usb_path, path_length + 1);
        R_SUCCEED();
    }

    Result HomebrewService::ResolveProgramIdAlias(sf::Out<ncm::ProgramId> out, ncm::ProgramId requested_program_id) {
        ncm::ProgramId resolved_program_id{};
        R_TRY(ResolveProgramAlias(std::addressof(resolved_program_id), requested_program_id));
        out.SetValue(resolved_program_id);
        R_SUCCEED();
    }

    Result HomebrewService::RecordLoaderProbe(ams::usb::content::LoaderProbeInfo info) {
        g_loader_probe_program_id.store(info.program_id, std::memory_order_relaxed);
        g_loader_probe_stage.store(info.stage, std::memory_order_relaxed);
        g_loader_probe_result.store(info.result, std::memory_order_relaxed);
        g_loader_probe_location_storage.store(info.location_storage, std::memory_order_relaxed);
        g_loader_probe_metadata_storage.store(info.metadata_storage, std::memory_order_relaxed);
        R_SUCCEED();
    }

    namespace {

    void AppendLoaderTraceRecord(const ams::usb::content::LoaderTraceInfo &info) {
        std::scoped_lock lk(g_loader_trace_mutex);
        const u64 sequence = ++g_loader_trace_next_sequence;
        auto &record = g_loader_trace_records[(sequence - 1) % MaxLoaderTraceRecords];
        record.sequence = sequence;
        record.info = info;
        if (info.stage == static_cast<u32>(ams::usb::content::LoaderProbeStage::NsPrePglPatchDiagnostic)) {
            g_ns_pre_pgl_patch_diagnostic_record = record;
            g_ns_pre_pgl_patch_diagnostic_valid = true;
        }
        g_loader_trace_count = std::min<u32>(g_loader_trace_count + 1, MaxLoaderTraceRecords);

        g_loader_probe_program_id.store(info.program_id, std::memory_order_relaxed);
        g_loader_probe_stage.store(info.stage, std::memory_order_relaxed);
        g_loader_probe_result.store(info.result, std::memory_order_relaxed);
        g_loader_probe_location_storage.store(info.location_storage, std::memory_order_relaxed);
        g_loader_probe_metadata_storage.store(info.metadata_storage, std::memory_order_relaxed);
    }

    }

    Result HomebrewService::RecordLoaderTrace(ams::usb::content::LoaderTraceInfo info) {
        AppendLoaderTraceRecord(info);
        R_SUCCEED();
    }

    void RecordLoaderTraceForDiagnostics(const ams::usb::content::LoaderTraceInfo &info) {
        AppendLoaderTraceRecord(info);
    }

    void BeginAppletForwardProgress(ncm::ProgramId program_id, u64 session,
            u32 command, u32 object, u32 input_size, u64 input0, bool decoded) {
        const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
        std::scoped_lock lk(g_loader_trace_mutex);
        g_applet_forward_progress.Begin(program_id.value, session, command, object, input_size, input0, decoded, now);
    }

    void RecordAppletNativeReturn(u64 session, Result result) {
        const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
        std::scoped_lock lk(g_loader_trace_mutex);
        g_applet_forward_progress.NativeReturn(session, result.GetValue(), now);
    }

    void RecordAppletObserverProgress(u64 session, bool completed) {
        const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
        std::scoped_lock lk(g_loader_trace_mutex);
        g_applet_forward_progress.Observer(session, completed, now);
    }

    void BeginFsForwardProgress(ncm::ProgramId client, ncm::ProgramId program, u64 session,
            u32 command, u32 object, u32 input_size, u64 input0, bool decoded) {
        const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
        std::scoped_lock lk(g_loader_trace_mutex);
        g_fs_forward_progress.Begin(client.value, program.value, session, command, object, input_size, input0, decoded, now);
    }

    void RecordFsNativeReturn(u64 session, Result result) {
        const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
        std::scoped_lock lk(g_loader_trace_mutex);
        g_fs_forward_progress.NativeReturn(session, result.GetValue(), now);
    }

    void RecordFsObserverProgress(u64 session, bool completed) {
        const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
        std::scoped_lock lk(g_loader_trace_mutex);
        g_fs_forward_progress.Observer(session, completed, now);
    }

    void RecordFsReplyResult(u64 session, Result result) {
        std::scoped_lock lk(g_loader_trace_mutex);
        g_fs_forward_progress.Reply(session, result.GetValue());
    }

    void RecordFsDetailSuppressed(bool failed) {
        std::scoped_lock lk(g_loader_trace_mutex);
        g_fs_forward_progress.SuppressDetail(failed);
    }

    void StoreNcmLaunchTraceForDiagnostics(const ncm_trace::LookupSnapshot &snapshot,
            ncm::ProgramId application_id, u64 session, Result begin_result,
            Result end_result, Result launch_result) {
        {
            std::scoped_lock lk(g_ncm_launch_trace_mutex);

            /* A later successful utility/forwarder exit must not erase the failed
             * title launch that this build was designed to capture. A newer
             * failure remains authoritative and replaces an older one. */
            if (g_ncm_launch_trace.valid
                    && R_FAILED(static_cast<Result>(g_ncm_launch_trace.launch_result))
                    && R_SUCCEEDED(launch_result)) {
                return;
            }

            g_ncm_launch_trace.snapshot = snapshot;
            g_ncm_launch_trace.application_id = application_id;
            g_ncm_launch_trace.session = session;
            g_ncm_launch_trace.begin_result = begin_result.GetValue();
            g_ncm_launch_trace.end_result = end_result.GetValue();
            g_ncm_launch_trace.launch_result = launch_result.GetValue();
            g_ncm_launch_trace.valid = true;
            ++g_ncm_launch_trace.generation;
            if (g_ncm_launch_trace.generation == 0) {
                ++g_ncm_launch_trace.generation;
            }
        }

        /* Mirror the bounded NCM snapshot into the chronological loader ring.
         * Users consistently retrieve that file, while the separate NCM file
         * can be missed if the SD card is removed before the monitor's next
         * status cycle. Each private NCM operation has a fixed stage so one
         * trace identifies the exact content ID and backend that returned the
         * asynchronous launch failure. */
        const bool valid_snapshot = ncm_trace::HasValidShape(snapshot)
            && snapshot.session == session
            && snapshot.title_id == application_id.value;
        if (valid_snapshot) {
            for (u32 i = 0; i < snapshot.count; ++i) {
                const auto &record = snapshot.records[i];
                ams::usb::content::LoaderTraceInfo info{};
                info.program_id = record.title_id != 0 ? record.title_id : application_id.value;
                std::memcpy(info.content_id, record.content_id, sizeof(info.content_id));
                info.stage = GetNcmLookupTraceStage(record.operation);
                info.result = record.result;
                info.location_storage = record.storage_id;
                info.metadata_storage = record.content_type;
                info.content_storage = static_cast<u8>(record.operation);
                info.flags = record.flags;
                info.media_generation = record.metadata_version;
                util::SNPrintf(info.path, sizeof(info.path),
                    "ncm op=%s seq=%llu sid=%u type=%u off=%u f=%02X %.48s",
                    GetNcmLookupOperationName(record.operation),
                    static_cast<unsigned long long>(record.sequence),
                    record.storage_id, record.content_type, record.id_offset,
                    record.flags, record.detail);
                AppendLoaderTraceRecord(info);
            }
        }

        ams::usb::content::LoaderTraceInfo summary{};
        summary.program_id = application_id.value;
        summary.stage = static_cast<u32>(NcmLaunchTraceSummary);
        summary.result = launch_result.GetValue();
        summary.flags = snapshot.flags;
        summary.media_generation = session;
        util::SNPrintf(summary.path, sizeof(summary.path),
            "ncm snapshot valid=%u count=%u dropped=%u begin=%08X end=%08X launch=%08X",
            valid_snapshot ? 1u : 0u, valid_snapshot ? snapshot.count : 0u,
            valid_snapshot ? snapshot.dropped : 0u, begin_result.GetValue(),
            end_result.GetValue(), launch_result.GetValue());
        AppendLoaderTraceRecord(summary);

        /* Publish immediately from the isolated NCM trace worker. The normal
         * monitor will retry on any SD I/O failure and still owns periodic
         * status/loader publication. */
        const system_power::BackgroundActivity activity;
        if (activity && !system_power::HasFailed()) static_cast<void>(PublishNcmLaunchTraceSnapshot());
    }

    void MarkUsbLaunchActiveForDiagnostics(ncm::ProgramId program_id) {
        if (ncm::IsApplicationId(program_id) && !ncm::IsSystemProgramId(program_id)) {
            g_active_usb_launch_program_id.store(program_id.value, std::memory_order_release);
        }
    }

    void ClearUsbLaunchActiveForDiagnostics(ncm::ProgramId program_id) {
        u64 expected = program_id.value;
        static_cast<void>(g_active_usb_launch_program_id.compare_exchange_strong(expected, 0, std::memory_order_acq_rel));
    }

    ncm::ProgramId GetActiveUsbLaunchProgramIdForDiagnostics() {
        return ncm::ProgramId{g_active_usb_launch_program_id.load(std::memory_order_acquire)};
    }

    void MarkLaunchTraceActiveForDiagnostics(ncm::ProgramId program_id) {
        if (ncm::IsApplicationId(program_id) && !ncm::IsSystemProgramId(program_id)) {
            g_active_launch_trace_program_id.store(program_id.value, std::memory_order_release);
        }
    }

    void ClearLaunchTraceActiveForDiagnostics(ncm::ProgramId program_id) {
        hang_capture::Cancel(program_id);
        u64 expected = program_id.value;
        static_cast<void>(g_active_launch_trace_program_id.compare_exchange_strong(expected, 0, std::memory_order_acq_rel));
    }

    ncm::ProgramId GetActiveLaunchTraceProgramIdForDiagnostics() {
        return ncm::ProgramId{g_active_launch_trace_program_id.load(std::memory_order_acquire)};
    }

    namespace {

        void LoaderTracePublisherThreadFunction(void *) {
            hang_capture::RegisterCurrentThread(hang_capture::Publisher);
            /* Sole trace writer, started after the existing post-boot delay.
             * No NCM/USB status calls, map refresh, notifications or status
             * lock here. SD/native FS itself can still block: this is not an
             * out-of-band crash logger or a claim that every hang is visible. */
            while (true) {
                {
                    const system_power::BackgroundActivity activity;
                    if (activity && !system_power::HasFailed()) {
                        hang_capture::BeatPublisher();
                        ++g_trace_publisher_cycles;
                        const Result result = PublishLoaderTraceSnapshot();
                        hang_capture::BeatPublisher();
                        g_trace_publisher_last_result = result.GetValue();
                        if (R_FAILED(result)) ++g_trace_publisher_failures;
                    }
                }
                os::SleepThread(TimeSpan::FromSeconds(1));
            }
        }

        void HomebrewServiceThreadFunction(void *) {
            hang_capture::RegisterCurrentThread(hang_capture::UsbService);
            mitm::WaitInitialized();
            const auto register_result = g_homebrew_server_manager.RegisterObjectForServer(g_homebrew_service_object.GetShared(), HomebrewServiceName, HomebrewServiceMaxSessions);
            if (R_FAILED(register_result)) {
                g_last_module_result = static_cast<Result>(register_result).GetValue();
                return;
            }
            g_homebrew_server_manager.LoopProcess();
        }

    }

        void MitmModule::ThreadFunction(void *) {
        hang_capture::RegisterCurrentThread(hang_capture::Monitor);
        /* Keep the optional USB monitor isolated from normal Atmosphere boot.
         * The service is registered only after MITM initialization and its
         * USB worker remains lazy until a real Host content request arrives. */
        if constexpr (!EnableContentStatusMonitor) {
            return;
        }

        /* Wait until the SD and settings overlay are ready. */
        mitm::WaitInitialized();

        /* The optional usb-content KIP is the sole ams:usb provider when
         * explicitly selected. Do not register a second provider or acquire
         * usb:hs from this process in that mode. */
        if (IsExternalProviderSelected()) {
            return;
        }

        /* Publish the initial gate before exposing ams:usb. An uninitialized
         * cache denies requests. All subsequent refreshes belong to this
         * monitor, never the USB request thread or its startup lock. */
        RefreshUsbSettings();
        const bool content_enabled = IsContentEnabled();
        /* SC1 embeds the user's USB-application compatibility policy. It is
         * independent of recoverable indexes and never changes SD/NAND policy. */
        g_allow_unsigned_usb_application.store(impl::UseEmbeddedUsbHeaderCompatibility(content_enabled), std::memory_order_release);
        const bool homebrew_enabled = !content_enabled && IsEnabled();
        const bool service_enabled = homebrew_enabled || content_enabled;
            static_cast<void>(PublishContentSettingBridge(content_enabled));

        /* This milestone is opt-in and does not alter normal boot behavior. */
        if (!homebrew_enabled && !content_enabled) {
            return;
        }

        g_last_module_result = ResultSuccess().GetValue();
        slot_preference_sd::Load(); // Before exposing ams:usb or starting detection.
        if (homebrew_enabled) {
            ResetImageScan(usb::ResultNotInitialized());
            ResetHomebrewScan(usb::ResultNotInitialized());
            static_cast<void>(WriteStatus());
        }

        // Optional module thread only; retries PSC startup for a bounded time.
        // Permanent failure quarantines USB without aborting normal Horizon.
        const Result power_result = system_power::Initialize();
        if (R_FAILED(power_result)) g_last_module_result = power_result.GetValue();

        if (service_enabled) {
            InitializeVirtualSdReaper(); // Before any service can retain an app process.
            /* Registration is boot-safe: this thread only exposes ams:usb.
             * The storage worker remains stopped until NCM explicitly sends
             * Start while opening Host storage. */
            const auto service_thread_result = os::CreateThread(std::addressof(g_homebrew_service_thread), HomebrewServiceThreadFunction, nullptr, g_homebrew_service_thread_stack, sizeof(g_homebrew_service_thread_stack), os::GetThreadCurrentPriority(os::GetCurrentThread()));
            if (R_FAILED(service_thread_result)) {
                g_last_module_result = static_cast<Result>(service_thread_result).GetValue();
                const system_power::BackgroundActivity activity;
                if (activity) static_cast<void>(WriteStatus());
                return;
            }
            os::StartThread(std::addressof(g_homebrew_service_thread));
        }

        if (content_enabled) {
            /* Detection must work before an installer is opened. Start the
             * sole worker after Horizon initialization; EnsureUsbStarted is
             * serialized with ams:usb's Start handler by g_start_mutex. */
            os::SleepThread(ContentMonitorBootDelay);
            for (;;) {
            if (system_power::HasFailed()) break;
            const system_power::BackgroundActivity startup_activity;
            if (!startup_activity) { os::SleepThread(TimeSpan::FromMilliSeconds(100)); continue; }
            // R126: deliberately do not start the old forced-fatal diagnostic.
            // The independent kernel recorder is explicitly armed over the LAN.
            SetMonitorPhase(impl::MonitorProgress::Starting);
            const auto trace_thread_result = os::CreateThread(std::addressof(g_trace_publisher_thread),
                LoaderTracePublisherThreadFunction, nullptr, g_trace_publisher_thread_stack,
                sizeof(g_trace_publisher_thread_stack), os::GetThreadCurrentPriority(os::GetCurrentThread()));
            if (R_SUCCEEDED(trace_thread_result)) {
                os::StartThread(std::addressof(g_trace_publisher_thread));
            } else {
                /* Optional diagnostics must not abort normal Horizon boot. */
                g_last_module_result = trace_thread_result.GetValue();
            }
            const auto detector_result = EnsureUsbStarted();
            if (R_FAILED(detector_result)) {
                g_last_module_result = static_cast<Result>(detector_result).GetValue();
            }

            bool native_path_from_settings = false;
            if (TryGetContentNativePathEnabledFromSystemSettings(std::addressof(native_path_from_settings))) {
                const auto native_path_result = ncm::SetUsbContentNativePathEnabled(native_path_from_settings);
                /* Keep AMS's diagnostic copy in sync with the NCM-owned
                 * setting. The service command above is the authoritative
                 * value used by HostContentStorageImpl::GetPath(). */
                ncm::SetUsbContentNativePathEnabledLocal(native_path_from_settings);
                if (R_FAILED(native_path_result)) {
                    g_last_module_result = static_cast<Result>(native_path_result).GetValue();
                }
            }

            bool metadata_route_from_settings = false;
            if (TryGetContentMetadataRouteEnabledFromSystemSettings(std::addressof(metadata_route_from_settings))) {
                const auto metadata_route_result = ncm::SetUsbContentMetadataRouteEnabled(metadata_route_from_settings);
                ncm::SetUsbContentMetadataRouteEnabledLocal(metadata_route_from_settings);
                if (R_FAILED(metadata_route_result)) {
                    g_last_module_result = static_cast<Result>(metadata_route_result).GetValue();
                }
            }

            bool route_from_settings = false;
            if (TryGetContentRouteEnabledFromSystemSettings(std::addressof(route_from_settings)) && route_from_settings) {
                g_content_route_auto_enabled = true;
                /* This setting enables the automatic policy only. The first
                 * successful status sample decides whether USB is actually
                 * mounted/ready/writable; until then the target remains SD. */
            }
            break;
            }
        }

        u32 mapped_title_refresh_countdown = 0;
        while (true) {
            // USB requests have their own sleep admission. Do not hold a
            // background power lease while opening a fresh verified hierarchy.
            if (!system_power::IsBackgroundPaused() || system_power::HasFailed()) RunWakeComparison();
            RunPatchWitness();
            {
            const system_power::BackgroundActivity activity;
            if (activity) PublishLastWake();
            if (activity && system_power::HasFailed()) {
                static_cast<void>(PublishSystemPowerFailure());
            } else if (activity) {
            // Covers both content and homebrew-only mode. This writer uses
            // its own physical-SD session and skips all active captures.
            PollStartupTimingAutosave();
            RefreshUsbSettings();
            ApplyEjectCommand();
            static_cast<void>(PublishEjectStatus());
            if (homebrew_enabled) {
                /* Scanning is client-owned. Sphaira performs the bounded USB
                 * homebrew scan in its progress thread; duplicating that walk
                 * here can serialize slow removable-media reads. */
                static_cast<void>(WriteStatus());
            } else if (content_enabled) {
                /* AMS is the sole USB owner. NCM proxies Host operations over
                 * ams:usb, while this monitor only publishes status and route
                * commands after Horizon has completed startup. */
                SetMonitorPhase(impl::MonitorProgress::RouteCommand);
                ApplyContentRouteCommand();
                SetMonitorPhase(impl::MonitorProgress::RefreshCommand);
                ApplyContentStatusRefreshCommand();
                const Result status_result = WriteContentStatus();
                RecordMonitorStatusResult(status_result);
                /* Do not publish the retired inferred-alias cache. Canonical
                 * title/content maps continue to be owned by NCM. */
                if (g_content_launch_path_enabled.load(std::memory_order_acquire)) {
                    if (mapped_title_refresh_countdown == 0) {
                        SetMonitorPhase(impl::MonitorProgress::CacheRefresh);
                        RefreshMappedTitleCache();
                        mapped_title_refresh_countdown = 5;
                    } else {
                        --mapped_title_refresh_countdown;
                    }
                } else {
                    mapped_title_refresh_countdown = 0;
                }
            }
            SetMonitorPhase(impl::MonitorProgress::Sleeping);
            }
            }
            os::SleepThread(TimeSpan::FromSeconds(1));
        }
    }

}
