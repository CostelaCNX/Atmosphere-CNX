/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#ifndef AMS_STRATOSPHERE_USB_STORAGE_HPP
#define AMS_STRATOSPHERE_USB_STORAGE_HPP

#include <vapours.hpp>
#include <stratosphere/os/os_sdk_mutex.hpp>
#include <stratosphere/os/os_sdk_condition_variable.hpp>
#include <stratosphere/usb/usb_sleep_core.hpp>
#include <stratosphere/os/os_thread_types.hpp>
#include <stratosphere/usb/usb_volume.hpp>
#include <stratosphere/usb/usb_eject.hpp>
#include <stratosphere/usb/usb_keep_alive_test_protocol.h>
#include <stratosphere/usb/usb_write_transfer_protocol.h>
#include <stratosphere/usb/usb_completion_probe_protocol.h>
#include <stratosphere/usb/usb_host_write_protocol.h>

namespace ams::usb::storage {

    constexpr inline size_t MaxInterfaceCount = 8;
    constexpr inline size_t MaxLunCount = 16;
    /* USB host/service calls can use more stack than the normal polling
     * loop. Keep this worker generously sized so host initialization cannot
     * corrupt the module or leave it stuck in Starting. */
    constexpr inline size_t ThreadStackSize = 64_KB;

    enum class State : u8 {
        Stopped,
        Starting,
        WaitingForDevice,
        Mounting,
        Ready,
        Error,
    };

    enum class FileSystemType : u8 {
        Unknown,
        Fat32,
        ExFat,
    };

    enum class ProbeStage : u32 {
        None,
        ClientInitialize,
        QueryAvailableInterfaces,
        QueryAcquiredInterfaces,
        DeviceInitialize,
        GetMaxLun,
        GetInquiry,
        DiscoverPartitions,
        Fat32Mount,
        ExFatMount,
        WritableMount,
    };

    struct Status {
        State state;
        bool running;
        u8 partition_style;
        FileSystemType filesystem_type;
        u8 partition_index;
        u8 partition_count;
        u8 lun;
        u8 writable;
        s32 interface_id;
        u32 last_result;
        u32 last_io_result;
        u64 media_generation;
        u64 block_count;
        u32 block_size;
        u32 reserved_2;
        u64 partition_first_lba;
        u64 partition_sector_count;
        char partition_name[37];
        char vendor[9];
        char product[17];
        char revision[5];
        u32 probe_stage;
        u32 probe_result;
        u32 available_interface_count;
        u32 acquired_interface_count;
        u32 probe_attempts;
    };

    /*
     * Owns the USB host client, one mass-storage device, and one read-only
     * FAT32/exFAT volume from a dedicated worker thread. The class is
     * intentionally opt-in; callers must start it explicitly.
     */
    class Service {
        NON_COPYABLE(Service);
        NON_MOVEABLE(Service);

        private:
            mutable os::SdkMutex m_mutex{};
            mutable os::SdkMutex m_volume_mutex{};
            mutable os::SdkMutex m_power_mutex{};
            os::SdkConditionVariable m_power_cv{};
            sleep::Gate m_power_gate{};
            bool m_sleep_had_media{};
            u64 m_sleep_generation{};
            u64 m_sleep_partition_lba{};
            host::Interface m_sleep_interface{};
            u8 m_sleep_fingerprint[64]{};
            sleep::VolumeMarker m_sleep_marker{};
            u8 m_sleep_serial[32]{};
            bool m_sleep_marker_armed{};
            bool m_sleep_has_serial{};
            bool m_checkpoint_pending{}; // Serialized with all filesystem mutations by m_volume_mutex.
            bool m_checkpoint_valid{};
            bool m_skip_checkpoint_on_next_mount{};
            bool m_resume_in_progress{};
            os::ThreadType m_thread{};
            alignas(os::ThreadStackAlignment) u8 m_thread_stack[ThreadStackSize]{};
            bool m_thread_created{false};
            bool m_stop_requested{false};
            bool m_reconnect_requested{false};
            bool m_running{false};
            u64 m_media_generation{0};
            Status m_status{};
            eject::Gate m_eject{}; // m_mutex; no new threads or global waits.

            host::Client m_client;
            mass_storage::Device m_device;
            volume::Fat32Volume m_fat32_volume;
            volume::ExFatVolume m_exfat_volume;
            volume::WritableVolume m_writable_volume;
            FileSystemType m_filesystem_type{FileSystemType::Unknown};
            bool m_writable_ready{false};
            const bool m_read_only;
            host::Interface m_available_interfaces[MaxInterfaceCount]{};
            host::Interface m_acquired_interfaces[MaxInterfaceCount]{};
            volume::Partition m_partitions[volume::MaxPartitionCount]{};

        public:
            explicit Service(bool read_only = false);
            ~Service();

        public:
            Result Start();
            Result Stop();
            Result PrepareSystemSleep();
            Result ReserveSystemWake();
            Result ResumeSystemSleep();
            void FailSystemPower(Result result);
            bool IsSystemPowerPaused() const;
            bool IsSystemPowerQuiescent() const;
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
            Result IsWritable(bool *out) const;
            Result CreateFileAtGeneration(const char *path, u64 size, u64 media_generation);
            Result ResizeFileAtGeneration(const char *path, u64 size, u64 media_generation);
            Result DeleteFileAtGeneration(const char *path, u64 media_generation);
            Result RenameFileAtGeneration(const char *old_path, const char *new_path, u64 media_generation);
            Result WriteFileAtGeneration(const char *path, u64 offset, const void *buffer, size_t size, size_t *out_written, u64 media_generation);
            Result CreateDirectoryAtGeneration(const char *path, u64 media_generation);
            Result FlushAtGeneration(u64 media_generation);
            bool IsRunning() const;
            Result AcquireEjectLease(u64 generation);
            void ReleaseEjectLease(u64 generation);
            eject::Status GetEjectStatus() const;
            Result RequestEject(u64 request, u64 generation);
            Result ResumeAfterEject();
            Result ControlKeepAliveTest(NexusUsbKeepAliveStatus *out, u64 caller_pid, const NexusUsbKeepAliveRequest &request);
            Result ControlWriteTransferTest(NexusUsbWriteTransferStatus *out, u64 caller_pid, const NexusUsbWriteTransferRequest &request);
            Result ControlCompletionProbeTest(NexusUsbCompletionProbeStatus *out, u64 caller_pid, const NexusUsbCompletionProbeRequest &request);
            Result ControlHostWriteTest(NexusUsbHostWriteStatus *out, u64 caller_pid, const NexusUsbHostWriteRequest &request);

        private:
            static void ThreadEntry(void *arg);
            Result BeginSystemOperation();
            void EndSystemOperation();
            Result CaptureRetainedInterface(host::Interface *out, u64 deadline = 0);
            Result CaptureSleepWitness(const Status &status, bool preparing);
            void TryArmSleepCheckpoint();
            Result VerifySleepMarker();
            Result RebindAfterSleep(const Status &saved, u64 deadline);
            void InvalidateFailedWake(Result result);
            void WorkerThread();
            bool ProcessEject(); // Worker only. Never called from FS/launch IPC.
            void RunIdleKeepAlive();
            bool ShouldStop() const;
            void SetState(State state, Result result);
            void SetWaiting(Result result);
            void SetProbeResult(ProbeStage stage, Result result, u32 available_count = 0, u32 acquired_count = 0);
            void SetReady(const mass_storage::Geometry &geometry, const mass_storage::InquiryData &inquiry, const volume::Partition &partition, FileSystemType filesystem_type, size_t partition_index, size_t partition_count, s32 interface_id, u8 lun, bool writable);
            void SetLastIoResult(Result result);
            /* Caller owns m_volume_mutex. Quarantine before releasing it. */
            void CompleteIoLocked(Result result);
            void RequestReconnect();
            bool ConsumeReconnectRequest();
            Result CheckMediaGenerationLocked(u64 media_generation) const;
            void AdvanceMediaGeneration();
            void Disconnect();
    };

    class EjectLease {
        NON_COPYABLE(EjectLease);
        NON_MOVEABLE(EjectLease);
        Service *service{};
        u64 generation{};
    public:
        EjectLease() = default;
        ~EjectLease() { Reset(); }
        void Reset() { if(service) service->ReleaseEjectLease(generation);service=nullptr;generation=0; }
        Result Acquire(Service *owner, u64 gen) {
            R_UNLESS(!service && owner, usb::ResultInvalidParameter());
            R_TRY(owner->AcquireEjectLease(gen)); service=owner; generation=gen; R_SUCCEED();
        }
    };

}

#endif /* AMS_STRATOSPHERE_USB_STORAGE_HPP */
