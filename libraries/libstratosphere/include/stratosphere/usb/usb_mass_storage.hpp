/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2, as published
 * by the Free Software Foundation.
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
#include <stratosphere/usb/usb_host.hpp>
#include <stratosphere/usb/usb_sleep_core.hpp>

namespace ams::usb::mass_storage {

    constexpr inline u8 InterfaceClass        = 0x08;
    constexpr inline u8 InterfaceSubClass     = 0x06;
    constexpr inline u8 InterfaceProtocolBulk = 0x50;
    constexpr inline u8 InterfaceProtocolUas  = 0x62;

    constexpr inline u8 ScsiTestUnitReady     = 0x00;
    constexpr inline u8 ScsiRequestSense      = 0x03;
    constexpr inline u8 ScsiInquiry            = 0x12;
    constexpr inline u8 ScsiReadCapacity10    = 0x25;
    constexpr inline u8 ScsiRead10            = 0x28;
    constexpr inline u8 ScsiWrite10           = 0x2A;
    constexpr inline u8 ScsiRead16            = 0x88;
    constexpr inline u8 ScsiWrite16           = 0x8A;
    constexpr inline u8 ScsiServiceActionIn   = 0x9E;
    constexpr inline u8 ScsiReadCapacity16    = 0x10;

    constexpr inline u8 BulkOnlyMassStorageReset = 0xFF;
    constexpr inline u8 BulkOnlyGetMaxLun         = 0xFE;

    constexpr inline size_t MaxCommandBlockSize = 16;
    constexpr inline size_t MaxSenseDataSize    = 64;
    constexpr inline size_t InquiryDataSize      = 36;

    struct Geometry {
        u64 block_count;
        u32 block_size;
    };

    struct InquiryData {
        u8 peripheral_device_type;
        bool removable;
        char vendor[9];
        char product[17];
        char revision[5];
    };

    // Pre-mount result, valid even when an empty slot cannot initialize.
    // Default to one LUN; only a validated GET_MAX_LUN may expand it.
    struct LunProbeInfo {
        u8 lun_count{1};
        bool medium_absent{false};
        // Transport failure during readiness, NOT proof of an empty slot.
        bool transport_failed{false};
    };

    /*
     * Make the filter used to find SCSI-transparent BOT interfaces. The
     * returned filter is only meaningful on Horizon; keeping this helper in
     * the public API makes discovery consistent for the future usbfs worker.
     */
    void MakeInterfaceFilter(host::InterfaceFilter *out);

    class Device {
        NON_COPYABLE(Device);
        NON_MOVEABLE(Device);

        private:
            enum class Transport : u8 {
                Bot,
                Uas,
            };

            host::InterfaceSession m_interface;
            host::EndpointSession  m_bulk_in;
            host::EndpointSession  m_bulk_out;
            host::EndpointSession  m_uas_command;
            host::EndpointSession  m_uas_status;
            host::EndpointSession  m_uas_data_in;
            host::EndpointSession  m_uas_data_out;
            bool m_is_initialized{false};
            bool m_keep_alive_failed{false};
            Transport m_transport{Transport::Bot};
            u8 m_interface_number{0};
            u8 m_bulk_in_endpoint_address{0};
            u8 m_bulk_out_endpoint_address{0};
            u8 m_lun{0};
            u32 m_tag{0};
            Geometry m_geometry{};
            u64 m_system_power_deadline{};
            bool m_unsynchronized_writes{};
            bool m_wake_read_only{};

            alignas(0x1000) u8 m_command_buffer[0x1000]{};
            alignas(0x1000) u8 m_data_buffer[0x1000]{};
            alignas(0x1000) u8 m_status_buffer[0x1000]{};

        public:
            Device() = default;
            ~Device();

        public:
            Result Initialize(host::Client *client, host::Interface *interface, u8 lun = 0);
            Result Initialize(host::Client *client, host::Interface *interface, u8 lun, LunProbeInfo *probe);
            Result Initialize(host::Client *client, host::Interface *interface, u8 lun, LunProbeInfo *probe, bool reset_before_probe);
            Result Initialize(host::Client *client, host::Interface *interface, u8 lun, LunProbeInfo *probe, bool reset_before_probe, bool reselect_interface);
            Result Finalize();
            Result InitializeForWake(host::Client *client, host::Interface *interface, u8 lun, u64 deadline);
            Result CaptureWakeSerial(u8 out_digest[32], u8 descriptor_index, u64 deadline);
            void SetWakeReadOnly(bool read_only) { m_wake_read_only = read_only; }

            bool IsInitialized() const { return m_is_initialized; }
            // Stop commands without destroying endpoints. The worker may
            // finalize them, but must not replay dirty filesystem cache data.
            void Quarantine() { m_keep_alive_failed = true; }
            bool IsQuarantined() const { return m_keep_alive_failed; }
            u8 GetLun() const { return m_lun; }
            s32 GetInterfaceId() const { return m_interface.GetId(); }
            const Geometry &GetGeometry() const { return m_geometry; }

            Result TestUnitReady();
            Result KeepAlive();
            Result SynchronizeCacheForEject(u64 deadline_ns = 0);
            void SetSystemPowerDeadline(u64 deadline) { m_system_power_deadline = deadline; }
            bool NeedsSystemSleepSync() const { return m_unsynchronized_writes; }
            Result CaptureSleepFingerprint(u8 *out_hashes, u64 partition_lba, u64 deadline, bool clean_resume);
            Result GetMaxLun(u8 *out_max_lun);
            Result RequestSense(void *out, size_t out_size);
            Result GetInquiry(InquiryData *out);
            Result ReadBlocks(u64 lba, u32 block_count, void *buffer, size_t buffer_size);
            Result WriteBlocks(u64 lba, u32 block_count, const void *buffer, size_t buffer_size);

        private:
            enum class DataDirection : u8 {
                None,
                In,
                Out,
            };

            Result ReadCapacity();
            Result InitializeImpl(host::Client *client, host::Interface *interface, u8 lun,
                                  LunProbeInfo *probe, bool reset_before_probe,
                                  bool reselect_interface, u64 wake_deadline);
            Result ReadWakeCapacity(Geometry *out, u64 deadline, sleep::ReadRetry &retry);
            Result ExecuteSleepRead(const u8 *command, size_t command_size, u32 data_size, u64 deadline, sleep::ReadRetry &retry);
            Result WaitForReady(bool *out_medium_absent = nullptr, bool *out_transport_failed = nullptr);
            Result RecoverStartupTransport(u64 deadline_ns);
            Result StartStopUnit(bool start);
            Result ClearEndpointStall(u8 endpoint_address);
            Result ResetTransport();
            Result ExecuteCommand(const u8 *command, size_t command_size, DataDirection direction, void *data, u32 data_size, u32 *out_transferred_size);
            Result ExecuteCommandOnce(const u8 *command, size_t command_size, DataDirection direction, void *data, u32 data_size, u32 *out_transferred_size, u64 deadline_ns = 0, bool *startup_csw_recovery_used = nullptr);
            Result ExecuteCommandUasOnce(const u8 *command, size_t command_size, DataDirection direction, void *data, u32 data_size, u32 *out_transferred_size, u64 deadline_ns = 0, u32 *wake_sense_size = nullptr);

            static void StoreBigEndian16(u8 *dst, u16 value);
            static void StoreBigEndian32(u8 *dst, u32 value);
            static void StoreBigEndian64(u8 *dst, u64 value);
            static u16 LoadBigEndian16(const u8 *src);
            static u32 LoadBigEndian32(const u8 *src);
            static u64 LoadBigEndian64(const u8 *src);
            static void CopyInquiryString(char *dst, size_t dst_size, const u8 *src, size_t src_size);
    };

}
