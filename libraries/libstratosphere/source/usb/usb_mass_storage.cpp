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
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_wake_identity.hpp>
#include <stratosphere/usb/usb_host_write.hpp>
#include <stratosphere/usb/usb_runtime_diagnostics.hpp>
#include <stratosphere/usb/usb_io_diagnostics.hpp>
#include <stratosphere/usb/usb_pause_diagnostics.hpp>
#include <stratosphere/usb/usb_completion_diagnostics.hpp>
#include <stratosphere/usb/usb_reader_policy.hpp>
#include <stratosphere/usb/usb_reader_startup_profile.hpp>
#include <stratosphere/usb/usb_reader_endpoint_profile.hpp>
#include <stratosphere/usb/usb_compatibility.hpp>
#include <stratosphere/usb/usb_interface_recovery.hpp>
#include <stratosphere/usb/usb_probe_trace.hpp>
#include <stratosphere/usb/usb_reader_direct_start.hpp>
#include <stratosphere/usb/usb_startup_history.hpp>
#include <stratosphere/usb/usb_sleep_diagnostics.hpp>
#include <stratosphere/usb/usb_startup_sense_csw.hpp>
#include <stratosphere/usb/usb_reader_fixed_sense.hpp>
#include <stratosphere/usb/usb_reader_device_reset.hpp>
#include "usb_keep_alive.hpp"
#include "usb_scsi_recovery.hpp"
#include "usb_startup.hpp"

#if defined(ATMOSPHERE_OS_HORIZON)

namespace ams::usb::mass_storage {

    namespace {

        constexpr u32 CommandBlockWrapperSignature = 0x43425355;
        constexpr u32 CommandStatusWrapperSignature = 0x53425355;

        constexpr u8 CommandBlockWrapperLength = 31;
        constexpr u8 CommandStatusWrapperLength = 13;

        constexpr u8 BulkOnlyDirectionIn = 0x80;

        constexpr u8 CswStatusPassed = 0x00;

        void RecordCommandResult(u8 opcode, Result result) {
            const u64 now = impl::UsbNowNs();
            io_diagnostics::last_command_ns.store(now, std::memory_order_relaxed);
            if (R_FAILED(result)) {
                io_diagnostics::transport_failures.fetch_add(1, std::memory_order_relaxed);
                io_diagnostics::last_failed_opcode.store(opcode, std::memory_order_relaxed);
                io_diagnostics::last_failed_result.store(result.GetValue(), std::memory_order_relaxed);
                io_diagnostics::last_failure_ns.store(now, std::memory_order_relaxed);
            }
        }

        constexpr u8 UasCommandIuId = 0x01;
        constexpr u8 UasStatusIuId  = 0x03;

        constexpr u8 RequestTypeHostToDeviceClassInterface = 0x21;
        constexpr u8 RequestTypeDeviceToHostClassInterface = 0xA1;
        constexpr u8 RequestTypeHostToDeviceStandardEndpoint = 0x02;
        constexpr u8 RequestTypeDeviceToHostStandardEndpoint = 0x82;
        constexpr u8 RequestGetStatus = 0x00;
        constexpr u8 RequestClearFeature = 0x01;
        constexpr u16 FeatureEndpointHalt = 0x00;

        constexpr u8 EndpointDirectionMask = 0x80;
        constexpr u8 EndpointTransferTypeMask = 0x03;
        constexpr u8 EndpointTransferTypeBulk = 0x02;

        /* Keep transfers large enough to amortize usb:hs IPC overhead. The
         * endpoint session is still bounded by Horizon and the caller's
         * aligned buffer, so this does not create an unbounded allocation. */
        constexpr u16 MaxUrbCount = 4;
        constexpr u32 MaxTransferSize = 0x400000;
        /* The public usb:hs ABI has no stream-id parameter. Keep UAS
         * serialized so devices that accept non-stream UAS can still be
         * used without pretending that full USB 3 queueing is available. */
        constexpr u16 UasMaxUrbCount = 1;

        struct CommandBlockWrapper {
            u32 signature;
            u32 tag;
            u32 data_transfer_length;
            u8 flags;
            u8 lun;
            u8 command_length;
            u8 command[MaxCommandBlockSize];
        } PACKED;

        struct CommandStatusWrapper {
            u32 signature;
            u32 tag;
            u32 data_residue;
            u8 status;
        } PACKED;

        static_assert(sizeof(CommandBlockWrapper) == CommandBlockWrapperLength);
        static_assert(sizeof(CommandStatusWrapper) == CommandStatusWrapperLength);

        struct UasCommandIu {
            u8 iu_id;
            u8 reserved_1;
            u8 tag[2];
            u8 priority_attribute;
            u8 reserved_5;
            u8 additional_cdb_length;
            u8 reserved_7;
            u8 lun[8];
            u8 cdb[MaxCommandBlockSize];
        } PACKED;

        struct UasStatusIu {
            u8 iu_id;
            u8 reserved_1;
            u8 tag[2];
            u8 status_qualifier[2];
            u8 status;
            u8 reserved_7[7];
            u8 sense_length[2];
            u8 sense[0x100];
        } PACKED;

        static_assert(sizeof(UasCommandIu) == 32);

        bool IsBulkEndpoint(const host::EndpointDescriptor &descriptor) {
            return descriptor.bDescriptorType == USB_DT_ENDPOINT &&
                   (descriptor.bmAttributes & EndpointTransferTypeMask) == EndpointTransferTypeBulk;
        }

        constexpr bool IsValidBlockSize(u32 block_size) {
            return block_size >= 0x200 && block_size <= 0x1000 && util::IsPowerOfTwo(block_size);
        }

    }

    void MakeInterfaceFilter(host::InterfaceFilter *out) {
        if (out == nullptr) {
            return;
        }
        std::memset(out, 0, sizeof(*out));
        /* Match mass-storage class/subclass and inspect BOT versus UAS
         * protocol after acquiring the interface. */
        out->Flags = (1u << 7) | (1u << 8);
        out->bInterfaceClass = InterfaceClass;
        out->bInterfaceSubClass = InterfaceSubClass;
    }

    Device::~Device() {
        if (m_is_initialized) {
            static_cast<void>(this->Finalize());
        }
    }

    Result Device::Initialize(host::Client *client, host::Interface *interface, u8 lun) {
        R_RETURN(this->Initialize(client, interface, lun, nullptr));
    }

    Result Device::Initialize(host::Client *client, host::Interface *interface, u8 lun, LunProbeInfo *probe) {
        R_RETURN(this->Initialize(client, interface, lun, probe, false));
    }

    Result Device::Initialize(host::Client *client, host::Interface *interface, u8 lun, LunProbeInfo *probe, bool reset_before_probe) {
        R_RETURN(this->Initialize(client, interface, lun, probe, reset_before_probe, false));
    }

    Result Device::Initialize(host::Client *client, host::Interface *interface, u8 lun, LunProbeInfo *probe, bool reset_before_probe, bool reselect_interface) {
        R_RETURN(InitializeImpl(client, interface, lun, probe, reset_before_probe, reselect_interface, 0));
    }

    Result Device::InitializeForWake(host::Client *client, host::Interface *interface, u8 lun, u64 deadline) {
        R_UNLESS(lun < 16 && impl::UsbNowNs() < deadline, usb::ResultOperationDenied());
        R_RETURN(InitializeImpl(client, interface, lun, nullptr, false, false, deadline));
    }

    Result Device::InitializeImpl(host::Client *client, host::Interface *interface, u8 lun, LunProbeInfo *probe, bool reset_before_probe, bool reselect_interface, u64 wake_deadline) {
        if (probe != nullptr) *probe = {};
        R_UNLESS(!m_is_initialized, usb::ResultAlreadyInitialized());
        R_UNLESS(client != nullptr && client->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(interface != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(!reselect_interface || (probe != nullptr && lun == 0 && !reset_before_probe &&
                 interface_recovery::Eligible(*interface)), usb::ResultOperationDenied());
        const auto &candidate = interface->inf.interface_desc;
        R_UNLESS(reader::Supported(candidate.bInterfaceClass, candidate.bInterfaceSubClass, candidate.bInterfaceProtocol), usb::ResultOperationDenied());
        R_UNLESS(!reset_before_probe || (probe != nullptr && lun < 16 &&
                 candidate.bInterfaceProtocol == InterfaceProtocolBulk), usb::ResultOperationDenied());

        // Retain the attempted identity even when readiness/capacity fails.
        io_diagnostics::startup_vid.store(interface->device_desc.idVendor);
        io_diagnostics::startup_pid.store(interface->device_desc.idProduct);
        endpoint_profile::Begin(interface->device_desc.idVendor, interface->device_desc.idProduct);
        compatibility::Begin(interface->device_desc, lun, impl::UsbNowNs());
        io_diagnostics::startup_protocol.store(interface->inf.interface_desc.bInterfaceProtocol);
        io_diagnostics::startup_lun.store(lun);
        io_diagnostics::startup_endpoints.store(0);
        io_diagnostics::startup_packets.store(0);

        m_wake_read_only = wake_deadline != 0;
        m_system_power_deadline = wake_deadline;
        bool success = false;
        bool direct_start = false;
        const bool reset_attempt = reader_device_reset::Begin(*interface, reselect_interface);
        u32 reset_ready_result = UINT32_MAX, reset_capacity_result = UINT32_MAX;
        probe_trace::Begin();
        startup_history::Begin(probe_trace::generation.load(), interface->busID, interface->deviceID,
            interface->device_desc.idVendor, interface->device_desc.idProduct);
        startup_history::Record(4,20,0,lun,reset_before_probe,reselect_interface,0);
        ON_SCOPE_EXIT {
            if (direct_start) reader_direct_start::Finish(success,reset_ready_result,reset_capacity_result);
            startup_history::Record(4,21,success ? 0 : UINT32_MAX,reset_ready_result,reset_capacity_result,direct_start,0);
            reader_device_reset::Finish(*interface, success, reset_attempt, reset_ready_result, reset_capacity_result);
            probe_trace::End();
            startup_history::End();
            if (!success) {
                if (m_uas_data_in.IsInitialized()) {
                    static_cast<void>(m_uas_data_in.Finalize());
                }
                if (m_uas_data_out.IsInitialized()) {
                    static_cast<void>(m_uas_data_out.Finalize());
                }
                if (m_uas_status.IsInitialized()) {
                    static_cast<void>(m_uas_status.Finalize());
                }
                if (m_uas_command.IsInitialized()) {
                    static_cast<void>(m_uas_command.Finalize());
                }
                if (m_bulk_in.IsInitialized()) {
                    static_cast<void>(m_bulk_in.Finalize());
                }
                if (m_bulk_out.IsInitialized()) {
                    static_cast<void>(m_bulk_out.Finalize());
                }
                if (m_interface.IsInitialized()) {
                    static_cast<void>(m_interface.Finalize());
                }
                m_is_initialized = false;
                m_wake_read_only = false;
                m_system_power_deadline = 0;
            }
        };

        host::InterfaceInfo interface_info{};
        host::EndpointDescriptor bulk_in_descriptor{};
        host::EndpointDescriptor bulk_out_descriptor{};
        host::EndpointDescriptor uas_data_in_descriptor{};
        host::EndpointDescriptor uas_data_out_descriptor{};
        bool found_bulk_in = false;
        bool found_bulk_out = false;
        size_t input_bulk_count = 0;
        size_t output_bulk_count = 0;

        R_TRY(m_interface.Initialize(client, interface));
        R_TRY(m_interface.GetInterfaceInfo(std::addressof(interface_info)));

        const auto &acquired = interface_info.interface_desc;
        R_UNLESS(reader::Supported(acquired.bInterfaceClass, acquired.bInterfaceSubClass, acquired.bInterfaceProtocol), usb::ResultOperationDenied());
        R_UNLESS(acquired.bInterfaceClass == candidate.bInterfaceClass &&
                 acquired.bInterfaceSubClass == candidate.bInterfaceSubClass &&
                 acquired.bInterfaceProtocol == candidate.bInterfaceProtocol,
                 usb::ResultOperationDenied());
        R_UNLESS((!reset_before_probe && !wake_deadline) || (acquired.bInterfaceNumber == candidate.bInterfaceNumber &&
                 acquired.bAlternateSetting == candidate.bAlternateSetting), usb::ResultOperationDenied());

        if (reselect_interface) {
            // All prior endpoints were closed before reacquisition. Only the
            // same observed alternate is selected, before opening any endpoint.
            R_UNLESS(interface_recovery::SameShape(interface->inf, interface_info), usb::ResultOperationDenied());
            if (reset_attempt) {
                R_UNLESS(endpoint_profile::Matches(interface->device_desc, interface_info), usb::ResultOperationDenied());
                // No endpoints opened and no media operation issued in this
                // fresh session. Reset only its device, never the host/dock.
                reader_device_reset::Calling();
                const u64 reset_start = impl::UsbNowNs();
                const auto reset_result = m_interface.ResetDevice();
                reader_device_reset::Complete(reset_result.GetValue(), impl::UsbNowNs() - reset_start);
                startup_history::Record(4,22,reset_result.GetValue(),0,0,8,
                    static_cast<u32>((impl::UsbNowNs()-reset_start)/1'000'000));
                // Reset may invalidate IDs/descriptors. Even a successful RPC
                // is NOT permission to use this session. Scope cleanup closes
                // it; the worker must discover/acquire everything again.
                R_TRY(reset_result);
                R_THROW(usb::ResultResourceBusy());
            }
            host::InterfaceInfo refreshed{};
            const auto rc = m_interface.SelectInterface(std::addressof(refreshed), acquired.bAlternateSetting);
            interface_recovery::reselect_result.store(rc.GetValue());
            R_TRY(rc);
            R_UNLESS(interface_recovery::SameShape(interface_info, refreshed), usb::ResultOperationDenied());
            interface_info = refreshed;
            os::SleepThread(TimeSpan::FromMilliSeconds(250));
        }

        m_transport = interface_info.interface_desc.bInterfaceProtocol == InterfaceProtocolUas ? Transport::Uas : Transport::Bot;

        m_interface_number = interface_info.interface_desc.bInterfaceNumber;

        for (size_t i = 0; i < util::size(interface_info.input_endpoint_descs); ++i) {
            const auto &descriptor = interface_info.input_endpoint_descs[i];
            if (IsBulkEndpoint(descriptor) &&
                (descriptor.bEndpointAddress & EndpointDirectionMask) != 0) {
                if (input_bulk_count++ == 0) {
                    bulk_in_descriptor = descriptor;
                    found_bulk_in = true;
                } else if (input_bulk_count == 2) {
                    uas_data_in_descriptor = descriptor;
                }
            }
        }

        for (size_t i = 0; i < util::size(interface_info.output_endpoint_descs); ++i) {
            const auto &descriptor = interface_info.output_endpoint_descs[i];
            if (IsBulkEndpoint(descriptor) &&
                (descriptor.bEndpointAddress & EndpointDirectionMask) == 0) {
                if (output_bulk_count++ == 0) {
                    bulk_out_descriptor = descriptor;
                    found_bulk_out = true;
                } else if (output_bulk_count == 2) {
                    uas_data_out_descriptor = descriptor;
                }
            }
        }

        R_UNLESS(found_bulk_in && found_bulk_out, usb::ResultOperationDenied());

        io_diagnostics::startup_endpoints.store((static_cast<u32>(bulk_in_descriptor.bEndpointAddress) << 8) | bulk_out_descriptor.bEndpointAddress);
        io_diagnostics::startup_packets.store((static_cast<u32>(bulk_in_descriptor.wMaxPacketSize) << 16) | bulk_out_descriptor.wMaxPacketSize);

        const auto compatibility_selection = compatibility::Select(interface->device_desc, interface->inf, interface_info);
        static_assert(MaxUrbCount == compatibility::DefaultUrbs && MaxTransferSize == compatibility::DefaultBytes);
        static_assert(UasMaxUrbCount == 1);
        compatibility::Applied(compatibility_selection);
        if (m_transport == Transport::Bot) {
            m_bulk_in_endpoint_address = bulk_in_descriptor.bEndpointAddress;
            m_bulk_out_endpoint_address = bulk_out_descriptor.bEndpointAddress;

            const auto allocation = endpoint_profile::Select(
                compatibility_selection.profile == compatibility::Reader05E30764, MaxUrbCount, MaxTransferSize);
            R_TRY(m_bulk_in.Initialize(std::addressof(m_interface), allocation.urbs, allocation.in_size, std::addressof(bulk_in_descriptor)));
            R_TRY(m_bulk_out.Initialize(std::addressof(m_interface), allocation.urbs, allocation.out_size, std::addressof(bulk_out_descriptor)));
        } else {
            /* UAS has two bulk endpoints in each direction. libnx exposes
             * endpoint descriptors but not the UAS pipe-usage descriptor;
             * standard devices enumerate command/status before data, so use
             * that order and remain serialized. */
            R_UNLESS(input_bulk_count >= 2 && output_bulk_count >= 2, usb::ResultOperationDenied());
            R_TRY(m_uas_command.Initialize(std::addressof(m_interface), UasMaxUrbCount, MaxTransferSize, std::addressof(bulk_out_descriptor)));
            R_TRY(m_uas_status.Initialize(std::addressof(m_interface), UasMaxUrbCount, MaxTransferSize, std::addressof(bulk_in_descriptor)));
            R_TRY(m_uas_data_in.Initialize(std::addressof(m_interface), UasMaxUrbCount, MaxTransferSize, std::addressof(uas_data_in_descriptor)));
            R_TRY(m_uas_data_out.Initialize(std::addressof(m_interface), UasMaxUrbCount, MaxTransferSize, std::addressof(uas_data_out_descriptor)));
        }

        m_lun = lun;
        m_tag = 0;
        m_geometry = {};
        m_is_initialized = true;
        m_keep_alive_failed = false;

        if (wake_deadline != 0) {
            // The sleep transaction already knows its exact LUN. No inventory
            // query, endpoint/device reset, START STOP UNIT or mounted writes
            // are appropriate until the medium has been positively identified.
            R_UNLESS(m_transport == Transport::Bot || lun == 0, usb::ResultOperationDenied());
            sleep::ReadRetry retry{true};
            retry.fresh_session = true;
            R_TRY(ReadWakeCapacity(&m_geometry, wake_deadline, retry));
            success = true;
            R_SUCCEED();
        }

        if (reset_before_probe) {
            // Prior Initialize already destroyed ALL old endpoints/interface.
            // Reset the fresh BOT session before any command to another LUN.
            io_diagnostics::slot_recovery_stage.store(3);
            const auto reset_result = this->RecoverStartupTransport(impl::UsbNowNs() + impl::StartupRecoveryBudgetNs);
            io_diagnostics::slot_recovery_reset_result.store(reset_result.GetValue());
            if (R_FAILED(reset_result)) {
                io_diagnostics::slot_recovery_stage.store(7);
                R_RETURN(reset_result);
            }
            io_diagnostics::slot_recovery_stage.store(4);
            // Let this observed bridge settle after resetting its fresh BOT
            // session. No sector has been accessed and no write can be replayed.
            if (startup_profile::Matches(interface->device_desc, interface_info))
                os::SleepThread(TimeSpan::FromMilliSeconds(startup_profile::SettleMs));
        }

        if (m_transport == Transport::Bot) {
            u8 max_lun = 0;
            direct_start = probe != nullptr && reader_direct_start::Claim(*interface,interface_info,m_lun,
                reset_before_probe,reselect_interface,m_interface.IsInitialized() &&
                m_bulk_in.IsInitialized() && m_bulk_out.IsInitialized(),m_geometry.block_count,probe_trace::generation.load());
            // C17 hardware trial: after a validated failure and the existing
            // one-shot reset, do not immediately repeat the optional query
            // that preceded both channel failures. LUN0 only; normal readiness,
            // capacity validation and filesystem mount remain mandatory.
            const Result max_lun_result = direct_start ? static_cast<Result>(ResultSuccess()) : this->GetMaxLun(std::addressof(max_lun));
            io_diagnostics::reader_max_lun_result.store(direct_start ? UINT32_MAX : max_lun_result.GetValue());
            if (direct_start) startup_history::Record(4,23,0,0,0,0xFE,0);
            if (R_FAILED(max_lun_result)) {
                // A timeout or lost completion can close the interface. That
                // is not an optional-request rejection: don't send a CBW on
                // its remaining child endpoints. The scope guard closes them.
                R_UNLESS(m_interface.IsInitialized(), max_lun_result);
                /* GET_MAX_LUN is optional in practice: a number of single-LUN
                 * flash drives and bridge chips STALL it even though their LUN 0
                 * is perfectly usable. Only fall back for LUN 0; probing a
                 * non-zero LUN still requires a valid max-LUN response. */
                R_UNLESS(m_lun == 0, max_lun_result);
                // C16's preliminary endpoint-status query timed out and consumed
                // the one interface-recovery opportunity before a bulk failure
                // was available to the exact-reader reset gate. Remove that
                // experiment; keep the original two-channel reset evidence gate.
                max_lun = 0;
            }
            R_UNLESS(m_lun <= max_lun, usb::ResultOperationDenied());
            if (probe != nullptr) probe->lun_count = max_lun + 1;

        } else {
            io_diagnostics::reader_max_lun_result.store(0);
            /* UAS carries the LUN in each command IU. Keep the first
             * implementation limited to LUN 0 until the host exposes a
             * stable multi-LUN policy. */
            R_UNLESS(m_lun == 0, usb::ResultOperationDenied());
        }
        // C21: small readiness autosense for validated standard BOT/SCSI/RBC.
        // Exact reader allocation/reset quirks remain separate. USB3, UAS,
        // mounted read/write sizes and timeouts are unchanged.
        bool transport_failed = false;
        static_assert(MaxSenseDataSize == compatibility::DefaultStartupSenseBytes);
        static_assert(reader_fixed_sense::FixedBytes == compatibility::Reader05E30764SenseBytes);
        const auto ready_result = [&]() {
            reader_fixed_sense::Scope fixed_sense(this, static_cast<u32>(m_interface.GetId()),
                compatibility::StartupSenseBytes(compatibility_selection) == reader_fixed_sense::FixedBytes &&
                m_transport == Transport::Bot);
            return this->WaitForReady(probe != nullptr ? std::addressof(probe->medium_absent) : nullptr,
                                      std::addressof(transport_failed));
        }();
        reset_ready_result = ready_result.GetValue();
        if (probe != nullptr) probe->transport_failed = transport_failed && m_transport == Transport::Bot && probe->lun_count > 1;
        R_TRY(ready_result);
        io_diagnostics::startup_stage.store(5);
        const auto capacity_result = this->ReadCapacity();
        reset_capacity_result = capacity_result.GetValue();
        R_TRY(capacity_result);
        compatibility::Capacity(m_geometry.block_count, m_geometry.block_size);
        io_diagnostics::startup_stage.store(6);

        io_diagnostics::protocol.store(interface_info.interface_desc.bInterfaceProtocol);
        io_diagnostics::bulk_in_packet.store(m_transport == Transport::Bot
            ? bulk_in_descriptor.wMaxPacketSize : uas_data_in_descriptor.wMaxPacketSize);
        io_diagnostics::bulk_out_packet.store(m_transport == Transport::Bot
            ? bulk_out_descriptor.wMaxPacketSize : uas_data_out_descriptor.wMaxPacketSize);

        success = true;
        R_SUCCEED();
    }

    Result Device::Finalize() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        io_diagnostics::protocol.store(0);
        io_diagnostics::bulk_in_packet.store(0);
        io_diagnostics::bulk_out_packet.store(0);

        // An ambiguous completion can have already aborted a single endpoint
        // or the interface. Always close every other child before reacquiring.
        Result close_result = ResultSuccess();
        const auto close = [&](auto &session) {
            if (session.IsInitialized()) {
                const Result rc = session.Finalize();
                if (R_SUCCEEDED(close_result)) close_result = rc;
            }
        };
        close(m_bulk_in);
        close(m_bulk_out);
        close(m_uas_data_in);
        close(m_uas_data_out);
        close(m_uas_status);
        close(m_uas_command);
        close(m_interface);

        m_geometry = {};
        m_interface_number = 0;
        m_bulk_in_endpoint_address = 0;
        m_bulk_out_endpoint_address = 0;
        m_transport = Transport::Bot;
        m_lun = 0;
        m_tag = 0;
        m_is_initialized = false;
        m_system_power_deadline = 0;
        m_wake_read_only = false;
        m_unsynchronized_writes = false;
        R_RETURN(close_result);
    }

    Result Device::CaptureWakeSerial(u8 out_digest[32], u8 descriptor_index, u64 deadline) {
        R_UNLESS(out_digest && m_is_initialized && !m_keep_alive_failed, usb::ResultNotInitialized());
        R_UNLESS(descriptor_index != 0, usb::ResultOperationDenied());
        R_UNLESS(impl::UsbNowNs() < deadline, usb::ResultResourceBusy());
        constexpr u8 GetDescriptor = 6;
        constexpr u16 StringDescriptor = 3U << 8;
        u32 received{};
        const auto descriptor = [&](u16 index, u16 language) -> Result {
            const Result rc = m_interface.ControlTransfer(0x80, GetDescriptor, StringDescriptor | index,
                                                           language, 255, m_data_buffer, &received, deadline);
            // A completed STALL rejects the optional descriptor. A timeout,
            // missing report or dead session is never downgraded to "no serial".
            if (rc.GetValue() == 0x0003248Cu && m_interface.IsInitialized())
                R_THROW(usb::ResultOperationDenied());
            R_RETURN(rc);
        };
        R_TRY(descriptor(0, 0));
        const u16 language = sleep::SerialLanguage(m_data_buffer, received);
        R_UNLESS(language != 0, usb::ResultOperationDenied());
        R_UNLESS(impl::UsbNowNs() < deadline, usb::ResultResourceBusy());
        R_TRY(descriptor(descriptor_index, language));
        R_UNLESS(sleep::HasUsefulSerial(m_data_buffer, received), usb::ResultOperationDenied());
        crypto::GenerateSha256(out_digest, 32, m_data_buffer + 2, received - 2);
        R_SUCCEED();
    }

    Result Device::GetMaxLun(u8 *out_max_lun) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_max_lun != nullptr, usb::ResultInvalidParameter());

        if (m_transport == Transport::Uas) {
            *out_max_lun = 0;
            R_SUCCEED();
        }

        std::memset(m_data_buffer, 0, sizeof(m_data_buffer));
        u32 transferred_size = 0;
        R_TRY(m_interface.ControlTransfer(RequestTypeDeviceToHostClassInterface, BulkOnlyGetMaxLun, 0, m_interface_number, 1, m_data_buffer, std::addressof(transferred_size)));
        R_UNLESS(transferred_size == 1, usb::ResultTransactionError());
        // BOT's CBW LUN field is four bits; never probe a malformed inventory.
        R_UNLESS(m_data_buffer[0] <= 15, usb::ResultOperationDenied());

        *out_max_lun = m_data_buffer[0];
        R_SUCCEED();
    }

    Result Device::ClearEndpointStall(u8 endpoint_address) {
        u32 transferred_size = 0;
        R_RETURN(m_interface.ControlTransfer(RequestTypeHostToDeviceStandardEndpoint, RequestClearFeature, FeatureEndpointHalt, endpoint_address, 0, nullptr, std::addressof(transferred_size)));
    }

    Result Device::ResetTransport() {
        R_UNLESS(m_transport == Transport::Bot, usb::ResultOperationDenied());
        startup_command_diagnostics::AttemptScope startup_recovery;
        startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_Unknown);
        u32 transferred_size = 0;
        R_TRY(m_interface.ControlTransfer(RequestTypeHostToDeviceClassInterface, BulkOnlyMassStorageReset, 0, m_interface_number, 0, nullptr, std::addressof(transferred_size)));
        R_UNLESS(transferred_size == 0, usb::ResultTransactionError());

        const auto clear_in_result = this->ClearEndpointStall(m_bulk_in_endpoint_address);
        const auto clear_out_result = this->ClearEndpointStall(m_bulk_out_endpoint_address);
        if (R_FAILED(clear_in_result)) {
            R_RETURN(clear_in_result);
        }
        R_RETURN(clear_out_result);
    }

    Result Device::RecoverStartupTransport(u64 deadline_ns) {
        // Only an unmounted BOT session with live endpoints can be reset.
        // Endpoint timeout closes that session: never resurrect it here.
        R_UNLESS(m_is_initialized && !m_keep_alive_failed && m_geometry.block_count == 0 && m_transport == Transport::Bot,
            usb::ResultOperationDenied());
        R_UNLESS(m_interface.IsInitialized() && m_bulk_in.IsInitialized() && m_bulk_out.IsInitialized(), usb::ResultNotInitialized());
        startup_command_diagnostics::AttemptScope startup_recovery;
        startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_Unknown);
        u32 transferred = 0;
        io_diagnostics::startup_recovery_stage.store(1);
        R_TRY(m_interface.ControlTransfer(RequestTypeHostToDeviceClassInterface, BulkOnlyMassStorageReset, 0, m_interface_number,
            0, nullptr, std::addressof(transferred), deadline_ns));
        R_UNLESS(transferred == 0, usb::ResultTransactionError());
        // BOT specification 5.3.4: reset, clear IN halt, then clear OUT halt.
        io_diagnostics::startup_recovery_stage.store(2);
        R_TRY(m_interface.ControlTransfer(RequestTypeHostToDeviceStandardEndpoint, RequestClearFeature, FeatureEndpointHalt,
            m_bulk_in_endpoint_address, 0, nullptr, std::addressof(transferred), deadline_ns));
        R_UNLESS(transferred == 0, usb::ResultTransactionError());
        io_diagnostics::startup_recovery_stage.store(3);
        R_TRY(m_interface.ControlTransfer(RequestTypeHostToDeviceStandardEndpoint, RequestClearFeature, FeatureEndpointHalt,
            m_bulk_out_endpoint_address, 0, nullptr, std::addressof(transferred), deadline_ns));
        R_UNLESS(transferred == 0, usb::ResultTransactionError());
        io_diagnostics::startup_recovery_stage.store(4);
        // Keep tags increasing, so an old status cannot match the next CBW.
        R_SUCCEED();
    }

    Result Device::WaitForReady(bool *out_medium_absent, bool *out_transport_failed) {
        if (out_medium_absent != nullptr) *out_medium_absent = false;
        if (out_transport_failed != nullptr) *out_transport_failed = false;
        R_UNLESS(m_is_initialized && !m_keep_alive_failed, usb::ResultNotInitialized());
        R_UNLESS(m_geometry.block_count == 0, usb::ResultOperationDenied());
        io_diagnostics::startup_attempts.fetch_add(1);
        io_diagnostics::startup_polls.store(0);
        io_diagnostics::startup_starts.store(0);
        io_diagnostics::startup_sense.store(0);
        io_diagnostics::startup_recoveries.store(0);
        io_diagnostics::startup_recovery_stage.store(0);
        io_diagnostics::startup_recovery_result.store(0);
        const u64 deadline = impl::UsbNowNs() + impl::StartupBudgetNs;
        bool start_sent = false;
        bool recovery_used = false;
        bool csw_recovery_used = false;
        u8 opcode = ScsiTestUnitReady;
        const auto finish = [&](Result result, u32 stage, u8 failed_opcode) -> Result {
            io_diagnostics::startup_result.store(result.GetValue());
            io_diagnostics::startup_last_ns.store(impl::UsbNowNs());
            if (R_FAILED(result)) {
                io_diagnostics::startup_failures.fetch_add(1);
                io_diagnostics::startup_failure_stage.store(stage);
                io_diagnostics::startup_failure_opcode.store(failed_opcode);
                io_diagnostics::startup_failure_result.store(result.GetValue());
                io_diagnostics::startup_failure_sense.store(io_diagnostics::startup_sense.load());
                this->Quarantine();
                // Initialize's scope guard releases endpoints; the worker
                // may reopen a fresh session. Never clear a poisoned stream.
                // Commands are counted once, at submission/completion below.
            }
            return result;
        };
        bool insertion_attention_used = false;
        for (size_t poll = 0; poll < impl::StartupMaxPolls && impl::UsbNowNs() < deadline; ++poll) {
            const u32 stage = opcode == 0x1B ? 2 : 1;
            io_diagnostics::startup_stage.store(stage);
            io_diagnostics::startup_polls.store(static_cast<u32>(poll + 1));
            io_diagnostics::startup_sense.store(0);
            u8 command[6]{};
            command[0] = opcode;
            if (opcode == 0x1B) { command[4] = 1; start_sent = true; io_diagnostics::startup_starts.store(1); }
            u32 transferred = 0;
            const u64 command_deadline = std::min(deadline, impl::UsbNowNs() + impl::StartupCommandBudgetNs);
            // Separate pre-mount policy. Generic read/write recovery is
            // unchanged, and no filesystem or cached writes exist here.
            const auto result = m_transport == Transport::Bot
                ? this->ExecuteCommandOnce(command, sizeof(command), DataDirection::None, nullptr, 0, std::addressof(transferred), command_deadline,
                    opcode == ScsiTestUnitReady ? std::addressof(csw_recovery_used) : nullptr)
                : this->ExecuteCommandUasOnce(command, sizeof(command), DataDirection::None, nullptr, 0, std::addressof(transferred), command_deadline);
            RecordCommandResult(opcode, result);
            if (R_SUCCEEDED(result)) {
                if (opcode == ScsiTestUnitReady) {
                    io_diagnostics::startup_stage.store(4);
                    return finish(ResultSuccess(), 4, opcode);
                }
                opcode = ScsiTestUnitReady;
            } else {
                if (m_transport == Transport::Bot && opcode == ScsiTestUnitReady && !recovery_used &&
                    usb::ResultTransactionError::Includes(result) && m_interface.IsInitialized() &&
                    m_bulk_in.IsInitialized() && m_bulk_out.IsInitialized() && impl::UsbNowNs() < deadline) {
                    // One pre-mount recovery for a completed transport/framing
                    // failure. Never replay START, sense, writes or UAS here.
                    recovery_used = true;
                    io_diagnostics::startup_recoveries.store(1);
                    const auto recovery = this->RecoverStartupTransport(std::min(deadline, impl::UsbNowNs() + impl::StartupRecoveryBudgetNs));
                    io_diagnostics::startup_recovery_result.store(recovery.GetValue());
                    if (R_FAILED(recovery)) return finish(recovery, stage, opcode);
                    if (impl::UsbNowNs() + 250'000'000ULL >= deadline) break;
                    os::SleepThread(TimeSpan::FromMilliSeconds(250));
                    continue;
                }
                if (m_transport != Transport::Bot || !usb::ResultScsiCommandFailed::Includes(result)) {
                    if (out_transport_failed != nullptr && m_transport == Transport::Bot && opcode == ScsiTestUnitReady)
                        *out_transport_failed = true;
                    return finish(result, stage, opcode);
                }
                // A valid failed CSW completed the command. Read and validate
                // its sense before deciding whether another TUR is safe.
                io_diagnostics::startup_stage.store(3);
                const u32 sense_request_size = reader_fixed_sense::RequestSize(this,
                    static_cast<u32>(m_interface.GetId()), m_transport == Transport::Bot, m_geometry.block_count, MaxSenseDataSize);
                const bool fixed_request = sense_request_size == reader_fixed_sense::FixedBytes;
                u8 sense_command[6]{}; sense_command[0] = ScsiRequestSense; sense_command[4] = static_cast<u8>(sense_request_size);
                std::memset(m_data_buffer, 0, MaxSenseDataSize);
                u32 sense_size = 0;
                if (fixed_request) reader_fixed_sense::Begin(static_cast<u32>(m_interface.GetId()), m_lun,
                    sense_request_size, static_cast<u32>(impl::UsbNowNs() / 1'000'000ULL));
                const auto sense_result = this->ExecuteCommandOnce(sense_command, sizeof(sense_command), DataDirection::In,
                    m_data_buffer, sense_request_size, std::addressof(sense_size), std::min(deadline, impl::UsbNowNs() + impl::SenseBudgetNs),
                    std::addressof(csw_recovery_used));
                const u32 sense = R_SUCCEEDED(sense_result) ? reader_fixed_sense::AllocatedCurrentSense(
                    m_data_buffer, sense_size, sense_request_size, impl::StartupSense(m_data_buffer, sense_size)) : 0;
                if (fixed_request) reader_fixed_sense::Finish(sense_result.GetValue(), sense_size, sense);
                RecordCommandResult(ScsiRequestSense, sense_result);
                if (R_FAILED(sense_result)) {
                    if (out_transport_failed != nullptr && opcode == ScsiTestUnitReady && !usb::ResultScsiCommandFailed::Includes(sense_result))
                        *out_transport_failed = true;
                    return finish(sense_result, 3, ScsiRequestSense);
                }
                io_diagnostics::startup_sense.store(sense);
                if (opcode == ScsiTestUnitReady && impl::StartupMediumAbsent(sense)) {
                    if (out_medium_absent != nullptr) *out_medium_absent = true;
                    return finish(usb::ResultTransactionError(), stage, opcode);
                }
                // A single insertion Unit Attention can be cleared BEFORE
                // capacity/partitions/caches exist. Never relax live I/O policy.
                const bool insertion_attention = opcode == ScsiTestUnitReady &&
                    sense == 0x01062800 && !insertion_attention_used;
                if (insertion_attention) {
                    insertion_attention_used = true;
                    io_diagnostics::reader_insertion_retries.fetch_add(1);
                }
                const bool becoming_ready = sense == 0x01020401;
                const bool reset_attention = (sense & 0xFFFFFF00u) == 0x01062900;
                const bool needs_start = sense == 0x01020402;
                const bool start_unsupported = opcode == 0x1B && (sense == 0x01052000 || sense == 0x01052400);
                if (!becoming_ready && !reset_attention && !needs_start && !start_unsupported && !insertion_attention)
                    return finish(usb::ResultTransactionError(), stage, opcode);
                // Issue START once, only after the drive explicitly requests
                // initialization. Do not replay it on a timeout or reset.
                opcode = needs_start && !start_sent ? 0x1B : ScsiTestUnitReady;
            }
            if (impl::UsbNowNs() + 250'000'000ULL >= deadline) break;
            os::SleepThread(TimeSpan::FromMilliSeconds(250));
        }
        return finish(usb::ResultResourceBusy(), io_diagnostics::startup_stage.load(), opcode);
    }

    Result Device::StartStopUnit(bool start) {
        u8 command[6]{};
        command[0] = 0x1B;
        command[4] = start ? 0x01 : 0x00;
        u32 transferred_size = 0;
        R_RETURN(this->ExecuteCommand(command, sizeof(command), DataDirection::None, nullptr, 0, std::addressof(transferred_size)));
    }

    Result Device::TestUnitReady() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());

        u8 command[6]{};
        command[0] = ScsiTestUnitReady;
        u32 transferred_size = 0;
        R_RETURN(this->ExecuteCommand(command, sizeof(command), DataDirection::None, nullptr, 0, std::addressof(transferred_size)));
    }

    Result Device::RequestSense(void *out, size_t out_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out != nullptr && out_size > 0 && out_size <= MaxSenseDataSize, usb::ResultInvalidParameter());
        R_UNLESS(usb::IsDmaAligned(reinterpret_cast<u64>(out)), usb::ResultAlignmentError());

        u8 command[6]{};
        command[0] = ScsiRequestSense;
        command[4] = static_cast<u8>(out_size);
        u32 transferred_size = 0;
        R_TRY(this->ExecuteCommand(command, sizeof(command), DataDirection::In, out, static_cast<u32>(out_size), std::addressof(transferred_size)));
        R_UNLESS(transferred_size <= out_size, usb::ResultTransactionError());
        R_SUCCEED();
    }

    Result Device::GetInquiry(InquiryData *out) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());

        u8 command[6]{};
        command[0] = ScsiInquiry;
        command[4] = InquiryDataSize;
        u32 transferred_size = 0;
        R_TRY(this->ExecuteCommand(command, sizeof(command), DataDirection::In, m_data_buffer, InquiryDataSize, std::addressof(transferred_size)));
        R_UNLESS(transferred_size >= InquiryDataSize, usb::ResultTransactionError());

        out->peripheral_device_type = m_data_buffer[0] & 0x1F;
        out->removable = (m_data_buffer[1] & 0x80) != 0;
        CopyInquiryString(out->vendor, sizeof(out->vendor), m_data_buffer + 8, 8);
        CopyInquiryString(out->product, sizeof(out->product), m_data_buffer + 16, 16);
        CopyInquiryString(out->revision, sizeof(out->revision), m_data_buffer + 32, 4);
        R_SUCCEED();
    }

    Result Device::ReadCapacity() {
        u8 command[16]{};
        command[0] = ScsiReadCapacity10;
        u32 transferred_size = 0;
        const auto capacity10_result = this->ExecuteCommand(command, 10, DataDirection::In, m_data_buffer, 8, std::addressof(transferred_size));

        bool use_capacity16 = R_FAILED(capacity10_result) || transferred_size < 8;
        u64 last_lba = 0;
        u32 block_size = 0;
        if (!use_capacity16) {
            last_lba = LoadBigEndian32(m_data_buffer + 0);
            block_size = LoadBigEndian32(m_data_buffer + 4);
            use_capacity16 = last_lba == UINT32_MAX || !IsValidBlockSize(block_size);
        }

        if (use_capacity16) {
            std::memset(command, 0, sizeof(command));
            command[0] = ScsiServiceActionIn;
            command[1] = ScsiReadCapacity16;
            StoreBigEndian32(command + 10, 32);
            R_TRY(this->ExecuteCommand(command, sizeof(command), DataDirection::In, m_data_buffer, 32, std::addressof(transferred_size)));
            R_UNLESS(transferred_size >= 32, usb::ResultTransactionError());

            last_lba = LoadBigEndian64(m_data_buffer + 0);
            block_size = LoadBigEndian32(m_data_buffer + 8);
        }

        R_UNLESS(IsValidBlockSize(block_size), usb::ResultOperationDenied());
        R_UNLESS(last_lba < UINT64_MAX, usb::ResultOperationDenied());

        m_geometry.block_count = last_lba + 1;
        m_geometry.block_size = block_size;
        R_SUCCEED();
    }

    Result Device::ReadBlocks(u64 lba, u32 block_count, void *buffer, size_t buffer_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(block_count > 0 && buffer != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(usb::IsDmaAligned(reinterpret_cast<u64>(buffer)), usb::ResultAlignmentError());
        R_UNLESS(m_geometry.block_size != 0, usb::ResultInternalStateError());
        R_UNLESS(block_count <= (SIZE_MAX / m_geometry.block_size), usb::ResultInvalidParameter());
        R_UNLESS(buffer_size == static_cast<size_t>(block_count) * m_geometry.block_size, usb::ResultInvalidParameter());
        R_UNLESS(lba < m_geometry.block_count && block_count <= m_geometry.block_count - lba, usb::ResultInvalidParameter());

        u8 *dst = static_cast<u8 *>(buffer);
        u64 current_lba = lba;
        u32 remaining = block_count;
        while (remaining > 0) {
            const u32 max_blocks = std::max<u32>(1, MaxTransferSize / m_geometry.block_size);
            const u32 transfer_blocks = std::min(remaining, max_blocks);
            const u32 transfer_size = transfer_blocks * m_geometry.block_size;

            u8 command[16]{};
            size_t command_size;
            if (current_lba <= UINT32_MAX && transfer_blocks <= UINT16_MAX &&
                static_cast<u64>(transfer_blocks - 1) <= UINT32_MAX - current_lba) {
                command[0] = ScsiRead10;
                StoreBigEndian32(command + 2, static_cast<u32>(current_lba));
                StoreBigEndian16(command + 7, static_cast<u16>(transfer_blocks));
                command_size = 10;
            } else {
                command[0] = ScsiRead16;
                StoreBigEndian64(command + 2, current_lba);
                StoreBigEndian32(command + 10, transfer_blocks);
                command_size = 16;
            }

            u32 transferred_size = 0;
            R_TRY(this->ExecuteCommand(command, command_size, DataDirection::In, dst, transfer_size, std::addressof(transferred_size)));
            R_UNLESS(transferred_size == transfer_size, usb::ResultTransactionError());

            dst += transfer_size;
            current_lba += transfer_blocks;
            remaining -= transfer_blocks;
        }

        R_SUCCEED();
    }

    Result Device::WriteBlocks(u64 lba, u32 block_count, const void *buffer, size_t buffer_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(block_count > 0 && buffer != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(usb::IsDmaAligned(reinterpret_cast<u64>(buffer)), usb::ResultAlignmentError());
        R_UNLESS(m_geometry.block_size != 0, usb::ResultInternalStateError());
        R_UNLESS(block_count <= (SIZE_MAX / m_geometry.block_size), usb::ResultInvalidParameter());
        R_UNLESS(buffer_size == static_cast<size_t>(block_count) * m_geometry.block_size, usb::ResultInvalidParameter());
        R_UNLESS(lba < m_geometry.block_count && block_count <= m_geometry.block_count - lba, usb::ResultInvalidParameter());

        const u8 *src = static_cast<const u8 *>(buffer);
        u64 current_lba = lba;
        u32 remaining = block_count;
        while (remaining > 0) {
            const u32 max_blocks = std::max<u32>(1, MaxTransferSize / m_geometry.block_size);
            const u32 transfer_blocks = std::min(remaining, max_blocks);
            const u32 transfer_size = transfer_blocks * m_geometry.block_size;

            u8 command[16]{};
            size_t command_size;
            if (current_lba <= UINT32_MAX && transfer_blocks <= UINT16_MAX &&
                static_cast<u64>(transfer_blocks - 1) <= UINT32_MAX - current_lba) {
                command[0] = ScsiWrite10;
                StoreBigEndian32(command + 2, static_cast<u32>(current_lba));
                StoreBigEndian16(command + 7, static_cast<u16>(transfer_blocks));
                command_size = 10;
            } else {
                command[0] = ScsiWrite16;
                StoreBigEndian64(command + 2, current_lba);
                StoreBigEndian32(command + 10, transfer_blocks);
                command_size = 16;
            }

            u32 transferred_size = 0;
            R_TRY(this->ExecuteCommand(command, command_size, DataDirection::Out, const_cast<u8 *>(src), transfer_size, std::addressof(transferred_size)));
            R_UNLESS(transferred_size == transfer_size, usb::ResultTransactionError());

            src += transfer_size;
            current_lba += transfer_blocks;
            remaining -= transfer_blocks;
        }

        R_SUCCEED();
    }

    Result Device::ReadWakeCapacity(Geometry *out, u64 deadline, sleep::ReadRetry &retry) {
        u8 capacity[16]{};
        capacity[0] = ScsiReadCapacity10;
        R_TRY(ExecuteSleepRead(capacity, 10, 8, deadline, retry));
        u64 last_lba = LoadBigEndian32(m_data_buffer);
        u32 block_size = LoadBigEndian32(m_data_buffer + 4);
        if (last_lba == UINT32_MAX) {
            std::memset(capacity, 0, sizeof(capacity));
            capacity[0] = ScsiServiceActionIn;
            capacity[1] = ScsiReadCapacity16;
            StoreBigEndian32(capacity + 10, 32);
            R_TRY(ExecuteSleepRead(capacity, sizeof(capacity), 32, deadline, retry));
            last_lba = LoadBigEndian64(m_data_buffer);
            block_size = LoadBigEndian32(m_data_buffer + 8);
        }
        R_UNLESS(last_lba < UINT64_MAX && IsValidBlockSize(block_size), usb::ResultMediaChanged());
        *out = { last_lba + 1, block_size };
        R_SUCCEED();
    }

    Result Device::SynchronizeCacheForEject(u64 deadline_ns) {
        R_UNLESS(m_is_initialized && !m_keep_alive_failed, usb::ResultNotInitialized());
        // SBC SYNCHRONIZE CACHE(10), IMMED=0, SYNC_NV=0, LBA=0, length=0:
        // complete the entire cache to media. Never infer success from TUR,
        // a timer, a rejected command or an unsupported cache capability.
        u8 command[10]{}; command[0]=0x35;
        u32 transferred=0;
        completion_diagnostics::CommandScope completion_timer(0, command[0], NEXUS_USB_COMPLETION_SOURCE_SYNC, command, sizeof(command));
        const u64 deadline=deadline_ns ? deadline_ns : impl::UsbNowNs()+10000000000ULL;
        const auto result=m_transport==Transport::Uas
            ? ExecuteCommandUasOnce(command,sizeof(command),DataDirection::None,nullptr,0,&transferred,deadline)
            : ExecuteCommandOnce(command,sizeof(command),DataDirection::None,nullptr,0,&transferred,deadline);
        const Result completion_result = R_SUCCEEDED(result) && transferred != 0 ? usb::ResultTransactionError() : result;
        completion_timer.Finish(completion_result.GetValue());
        RecordCommandResult(command[0],result);
        R_TRY(result);
        R_UNLESS(transferred==0,usb::ResultTransactionError());
        m_unsynchronized_writes=false;
        R_SUCCEED();
    }

    Result Device::ExecuteSleepRead(const u8 *command, size_t command_size, u32 data_size, u64 deadline, sleep::ReadRetry &retry) {
        // Private read commands only. Retry evidence must come from a complete
        // failed BOT CSW + REQUEST SENSE or a fresh UAS session's tagged sense IU.
        R_UNLESS(command && impl::CanReplayRead(command[0]) && data_size <= sizeof(m_data_buffer), usb::ResultInvalidParameter());
        for (;;) {
            R_UNLESS(impl::UsbNowNs() < deadline, usb::ResultResourceBusy());
            u32 transferred = 0;
            u32 sense_size = 0;
            const auto rc = m_transport == Transport::Uas
                ? ExecuteCommandUasOnce(command,command_size,DataDirection::In,m_data_buffer,data_size,&transferred,deadline,
                                        retry.fresh_session ? &sense_size : nullptr)
                : ExecuteCommandOnce(command,command_size,DataDirection::In,m_data_buffer,data_size,&transferred,deadline);
            io_diagnostics::last_command_ns.store(impl::UsbNowNs(),std::memory_order_relaxed);
            if (R_SUCCEEDED(rc)) {
                R_UNLESS(transferred == data_size, usb::ResultTransactionError());
                R_SUCCEED();
            }
            R_UNLESS(usb::ResultScsiCommandFailed::Includes(rc), rc);
            const u8 *sense_data = m_data_buffer;
            if (m_transport == Transport::Bot) {
                u8 sense[6]{}; sense[0] = ScsiRequestSense; sense[4] = MaxSenseDataSize;
                std::memset(m_data_buffer,0,MaxSenseDataSize);
                R_TRY(ExecuteCommandOnce(sense,sizeof(sense),DataDirection::In,m_data_buffer,MaxSenseDataSize,&sense_size,deadline));
                io_diagnostics::last_command_ns.store(impl::UsbNowNs(),std::memory_order_relaxed);
            } else {
                R_UNLESS(retry.fresh_session && sense_size >= 8, rc);
                sense_data = m_status_buffer + 16;
            }
            if (retry.RetryReset(sense_data,sense_size)) {
                sleep_diagnostics::reset_attentions.fetch_add(1);
                continue;
            }
            if (retry.RetryFreshInsertion(sense_data, sense_size)) continue;
            R_UNLESS(impl::ClassifySense(sense_data,sense_size) == impl::SenseAction::BecomingReady && retry.RetryReady(), rc);
            const auto now = impl::UsbNowNs();
            R_UNLESS(now < deadline && deadline-now > 250'000'000ULL, rc);
            sleep_diagnostics::ready_retries.fetch_add(1);
            os::SleepThread(TimeSpan::FromMilliSeconds(250));
        }
    }

    Result Device::CaptureSleepFingerprint(u8 *out_hashes, u64 partition_lba, u64 deadline, bool clean_resume) {
        R_UNLESS(out_hashes && m_is_initialized && !m_keep_alive_failed,usb::ResultNotInitialized());
        R_UNLESS(IsValidBlockSize(m_geometry.block_size) && m_geometry.block_size <= sizeof(m_data_buffer) &&
                 partition_lba < m_geometry.block_count,usb::ResultInvalidParameter());
        sleep::ReadRetry retry{clean_resume};
        // Check current geometry without modifying the mounted geometry.
        Geometry current{};
        R_TRY(ReadWakeCapacity(&current, deadline, retry));
        R_UNLESS(current.block_count == m_geometry.block_count && current.block_size == m_geometry.block_size,usb::ResultMediaChanged());
        // Fingerprints are an additional retained-session guard, not a claim
        // that a re-enumerated/replaced device has authenticated identity.
        const u64 lbas[2]{0,partition_lba};
        for (size_t i=0;i<2;++i) {
            u8 read[16]{}; read[0]=ScsiRead16; StoreBigEndian64(read+2,lbas[i]); StoreBigEndian32(read+10,1);
            size_t command_size=16;
            if (lbas[i] <= UINT32_MAX) {
                std::memset(read,0,sizeof(read)); read[0]=ScsiRead10; StoreBigEndian32(read+2,static_cast<u32>(lbas[i])); StoreBigEndian16(read+7,1); command_size=10;
            }
            R_TRY(ExecuteSleepRead(read,command_size,m_geometry.block_size,deadline,retry));
            crypto::GenerateSha256(out_hashes+i*32,32,m_data_buffer,m_geometry.block_size);
        }
        R_SUCCEED();
    }

    Result Device::KeepAlive() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(!m_keep_alive_failed, usb::ResultNotInitialized());
        R_UNLESS(IsValidBlockSize(m_geometry.block_size) && m_geometry.block_count > 0, usb::ResultInvalidParameter());
        /* Read a single sector directly, bypassing all filesystem caches.
         * No writes, MODE SELECT, ATA passthrough, eject lock or persistent
         * device power-policy changes. TUR alone need not reset idle timers. */
        u8 command[10]{};
        command[0] = ScsiRead10;
        StoreBigEndian16(command + 7, 1);
        u32 transferred = 0;
        const u64 now = impl::UsbNowNs();
        pause_diagnostics::Scope pause_timer(pause_diagnostics::Stage::KeepAlive, m_geometry.block_size, ScsiRead10);
        completion_diagnostics::CommandScope completion_timer(m_geometry.block_size, ScsiRead10, NEXUS_USB_COMPLETION_SOURCE_KEEP_ALIVE, command, sizeof(command));
        const u64 deadline = now + impl::KeepAliveBudgetNs;
        io_diagnostics::last_command_ns.store(now, std::memory_order_relaxed);
        const auto keep_alive_result = [&]() -> Result {
            u64 ready_deadline = 0;
            u32 retries = 0;
            for (;;) {
                const u64 command_deadline = ready_deadline != 0 ? ready_deadline : deadline;
                const auto rc = m_transport == Transport::Uas
                    ? this->ExecuteCommandUasOnce(command, sizeof(command), DataDirection::In, m_data_buffer, m_geometry.block_size, std::addressof(transferred), command_deadline)
                    : this->ExecuteCommandOnce(command, sizeof(command), DataDirection::In, m_data_buffer, m_geometry.block_size, std::addressof(transferred), command_deadline);
                if (R_SUCCEEDED(rc)) {
                    if (retries != 0) runtime_diagnostics::ready_recovered.fetch_add(1, std::memory_order_relaxed);
                    R_SUCCEED();
                }
                if (m_transport != Transport::Bot || !usb::ResultScsiCommandFailed::Includes(rc)) R_RETURN(rc);
                u8 sense_command[6]{};
                sense_command[0] = ScsiRequestSense;
                sense_command[4] = MaxSenseDataSize;
                std::memset(m_data_buffer, 0, MaxSenseDataSize);
                u32 sense_size = 0;
                const u64 sense_deadline = std::min(command_deadline, impl::UsbNowNs() + impl::SenseBudgetNs);
                const auto sense_rc = this->ExecuteCommandOnce(sense_command, sizeof(sense_command), DataDirection::In,
                    m_data_buffer, MaxSenseDataSize, std::addressof(sense_size), sense_deadline);
                R_TRY(sense_rc);
                const auto action = impl::ClassifySense(m_data_buffer, sense_size);
                runtime_diagnostics::last_sense_action.store(static_cast<u32>(action) + 1, std::memory_order_relaxed);
                if (action != impl::SenseAction::BecomingReady || !impl::PrepareReadyRetry(ready_deadline, retries, deadline)) R_RETURN(rc);
                runtime_diagnostics::ready_retries.fetch_add(1, std::memory_order_relaxed);
            }
        };
        auto result = keep_alive_result();
        if (R_SUCCEEDED(result) && transferred != m_geometry.block_size) {
            result = usb::ResultTransactionError();
        }
        // Only the volume-locked worker calls KeepAlive, so there is one writer
        // for these atomics. Readers sample them without delaying filesystem I/O.
        const u64 elapsed = impl::UsbNowNs() - now;
        io_diagnostics::keep_alive_last_duration_ns.store(elapsed, std::memory_order_relaxed);
        if (elapsed > io_diagnostics::keep_alive_max_duration_ns.load(std::memory_order_relaxed)) {
            io_diagnostics::keep_alive_max_duration_ns.store(elapsed, std::memory_order_relaxed);
        }
        if (R_SUCCEEDED(result) && elapsed > impl::KeepAliveSlowNs) {
            io_diagnostics::keep_alive_slow_successes.fetch_add(1, std::memory_order_relaxed);
        }
        completion_timer.Finish(result.GetValue());
        RecordCommandResult(command[0], result);
        if (R_FAILED(result)) m_keep_alive_failed = true;
        /* No reset or uncertain-command replay here. Only a completed NOT
         * READY command can be retried; all other failures quarantine media. */
        pause_timer.Finish(result);
        R_RETURN(result);
    }

    Result Device::ExecuteCommand(const u8 *command, size_t command_size, DataDirection direction, void *data, u32 data_size, u32 *out_transferred_size) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(!m_keep_alive_failed, usb::ResultNotInitialized());
        R_UNLESS(command != nullptr && command_size > 0 && command_size <= MaxCommandBlockSize, usb::ResultInvalidParameter());
        R_UNLESS(out_transferred_size != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(data_size == 0 || data != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(data_size == 0 || usb::IsDmaAligned(reinterpret_cast<u64>(data)), usb::ResultAlignmentError());

        pause_diagnostics::Scope pause_timer(pause_diagnostics::Stage::Command, data_size, command[0]);
        completion_diagnostics::CommandScope completion_timer(data_size, command[0], NEXUS_USB_COMPLETION_SOURCE_COMMAND, command, command_size);
        io_diagnostics::last_command_ns.store(impl::UsbNowNs(), std::memory_order_relaxed);
        const auto command_result = [&]() -> Result {

        if (m_transport == Transport::Uas) {
            /* UAS has no BOT reset-recovery sequence. Let the storage worker
             * release and reprobe the interface instead of retrying against
             * stale endpoint state. */
            const auto result = this->ExecuteCommandUasOnce(command, command_size, direction, data, data_size, out_transferred_size);
            if (R_FAILED(result)) this->Quarantine();
            R_RETURN(result);
        }

        const bool replayable = direction != DataDirection::Out && impl::CanReplayRead(command[0]);
        size_t attempt = 0;
        u64 ready_deadline = 0;
        u32 ready_retries = 0;
        for (;;) {
            *out_transferred_size = 0;
            const auto result = this->ExecuteCommandOnce(command, command_size, direction, data, data_size, out_transferred_size, ready_deadline);
            if (R_SUCCEEDED(result)) {
                if (ready_retries != 0) runtime_diagnostics::ready_recovered.fetch_add(1, std::memory_order_relaxed);
                R_SUCCEED();
            }
            if (usb::ResultScsiCommandFailed::Includes(result)) {
                // A valid CSW status 1 completed the BOT transaction. Do not
                // reset the link for an ordinary device command refusal.
                if (command[0] == ScsiRequestSense) {
                    this->Quarantine();
                    R_THROW(usb::ResultTransactionError());
                }
                u8 sense_command[6]{};
                sense_command[0] = ScsiRequestSense;
                sense_command[4] = MaxSenseDataSize;
                std::memset(m_data_buffer, 0, MaxSenseDataSize);
                u32 sense_size = 0;
                u64 sense_deadline = impl::UsbNowNs() + impl::SenseBudgetNs;
                if (ready_deadline != 0 && ready_deadline < sense_deadline) sense_deadline = ready_deadline;
                const auto sense_result = this->ExecuteCommandOnce(sense_command, sizeof(sense_command),
                    DataDirection::In, m_data_buffer, MaxSenseDataSize, std::addressof(sense_size),
                    sense_deadline);
                if (R_FAILED(sense_result)) {
                    m_keep_alive_failed = true;
                    R_THROW(usb::ResultTransactionError());
                }
                const auto action = impl::ClassifySense(m_data_buffer, sense_size);
                runtime_diagnostics::last_sense_action.store(static_cast<u32>(action) + 1, std::memory_order_relaxed);
                if (action == impl::SenseAction::Refused) R_THROW(usb::ResultOperationDenied());
                if (replayable && action == impl::SenseAction::BecomingReady &&
                    impl::PrepareReadyRetry(ready_deadline, ready_retries, m_system_power_deadline)) {
                    runtime_diagnostics::ready_retries.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // Unit attention after mounting may invalidate dirty caches.
                // Consume reset attention only during initial geometry probe.
                if (attempt == 0 && ready_retries == 0 && replayable &&
                    action == impl::SenseAction::ResetDuringProbe && m_geometry.block_count == 0) {
                    ++attempt;
                    os::SleepThread(TimeSpan::FromMilliSeconds(100));
                    continue;
                }
                m_keep_alive_failed = true;
                R_THROW(usb::ResultTransactionError());
            }
            // An uncertain write/START STOP must not be replayed. Poison the
            // stream immediately so finalization cannot send cached writes.
            if (!replayable || attempt != 0 || m_system_power_deadline != 0) {
                m_keep_alive_failed = true;
                R_RETURN(result);
            }
            if (attempt == 0) {
                const auto recovery_result = this->ResetTransport();
                if (R_FAILED(recovery_result)) {
                    m_keep_alive_failed = true;
                    R_RETURN(result);
                }
                // Keep tags increasing: a stale pre-reset CSW must not match
                // the next command. Recovery is still limited to one reset.
                ++attempt;
            } else {
                R_RETURN(result);
            }
        }

        R_THROW(usb::ResultInternalStateError());
        }();
        completion_timer.Finish(command_result.GetValue());
        RecordCommandResult(command[0], command_result);
        pause_timer.Finish(command_result);
        R_RETURN(command_result);
    }

    Result Device::ExecuteCommandOnce(const u8 *command, size_t command_size, DataDirection direction, void *data, u32 data_size, u32 *out_transferred_size, u64 deadline_ns, bool *startup_csw_recovery_used) {
        R_UNLESS(!m_wake_read_only || (direction != DataDirection::Out && command && command_size &&
            (impl::CanReplayRead(command[0]) || command[0] == ScsiRequestSense)), usb::ResultOperationDenied());
        if (m_system_power_deadline && (!deadline_ns || m_system_power_deadline < deadline_ns)) deadline_ns=m_system_power_deadline;
        R_UNLESS(!deadline_ns || impl::UsbNowNs() < deadline_ns,usb::ResultResourceBusy());
        if (direction==DataDirection::Out) m_unsynchronized_writes=true;

        auto *cbw = reinterpret_cast<CommandBlockWrapper *>(m_command_buffer);
        auto *csw = reinterpret_cast<CommandStatusWrapper *>(m_status_buffer);
        std::memset(csw, 0, sizeof(*csw));
        std::memset(cbw, 0, sizeof(*cbw));
        cbw->signature = CommandBlockWrapperSignature;
        cbw->tag = ++m_tag;
        cbw->data_transfer_length = data_size;
        cbw->flags = direction == DataDirection::In ? BulkOnlyDirectionIn : 0;
        cbw->lun = m_lun;
        cbw->command_length = static_cast<u8>(command_size);
        std::memcpy(cbw->command, command, command_size);
        startup_command_diagnostics::AttemptScope startup_attempt(command, command_size, cbw->tag,
            m_lun, m_geometry.block_size, 1);

        u32 transferred_size = 0;
        bool csw_retried = false;
        bool sense_csw_recovery = false;
        const auto observed = [&](Result result, u32 stage, u32 expected) -> Result {
            if (sense_csw_recovery && R_FAILED(result))
                startup_sense_csw::Finish(result.GetValue(), stage, transferred_size, csw->data_residue, csw->status);
            if (csw_retried && R_FAILED(result)) {
                io_diagnostics::csw_recovery_final_result.store(result.GetValue());
                io_diagnostics::csw_recovery_stage.store(stage == 4 ? 9 : stage == 9 ? 6 : 10);
                if (stage == 9) io_diagnostics::csw_recovery_validated.fetch_add(1);
            }
            if (R_FAILED(result)) {
                runtime_diagnostics::last_stage.store(stage, std::memory_order_relaxed);
                runtime_diagnostics::last_opcode.store(command[0], std::memory_order_relaxed);
                runtime_diagnostics::last_result.store(result.GetValue(), std::memory_order_relaxed);
            }
            if (R_FAILED(result) && m_geometry.block_count == 0) {
                io_diagnostics::startup_xfer_stage.store(stage);
                io_diagnostics::startup_xfer_opcode.store(command[0]);
                io_diagnostics::startup_xfer_result.store(result.GetValue());
                io_diagnostics::startup_xfer_expected.store(expected);
                io_diagnostics::startup_xfer_actual.store(transferred_size);
                io_diagnostics::startup_xfer_tag.store(cbw->tag);
                io_diagnostics::startup_csw_signature.store(csw->signature);
                io_diagnostics::startup_csw_tag.store(csw->tag);
                io_diagnostics::startup_csw_residue.store(csw->data_residue);
                io_diagnostics::startup_csw_status.store(csw->status);
            }
            return result;
        };
        R_TRY(observed(pause_diagnostics::Measure(pause_diagnostics::Stage::BotCbw, CommandBlockWrapperLength, command[0], [&] {
            startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_BotCommand);
            return m_bulk_out.PostBuffer(m_command_buffer, CommandBlockWrapperLength, std::addressof(transferred_size), deadline_ns);
        }), 1, CommandBlockWrapperLength));
        R_UNLESS(transferred_size == CommandBlockWrapperLength, observed(usb::ResultTransactionError(), 2, CommandBlockWrapperLength));

        if (data_size > 0) {
            transferred_size = 0;
            if (direction == DataDirection::In) {
                R_TRY(observed(pause_diagnostics::Measure(pause_diagnostics::Stage::BotData, data_size, command[0], [&] {
                    startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_BotData);
                    return m_bulk_in.PostBuffer(data, data_size, std::addressof(transferred_size), deadline_ns);
                }), 3, data_size));
            } else {
                R_TRY(observed(pause_diagnostics::Measure(pause_diagnostics::Stage::BotData, data_size, command[0], [&] {
                    startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_BotData);
                    // SD15: split only the HOST data transfers, not the SCSI
                    // write. The CBW and single following CSW remain unchanged.
                    const bool write_command = (command[0] == ScsiWrite10 && command_size == 10) ||
                                               (command[0] == ScsiWrite16 && command_size == 16);
                    if (write_command) {
                        const auto plan = host_write::MakePlan(data_size, reinterpret_cast<uintptr_t>(data),
                            m_bulk_out.GetMaxPacketSize(), hos::GetVersion() >= hos::Version_2_0_0,
                            host_write::SelectedMode(), impl::UsbNowNs(), deadline_ns);
                        if (plan.eligible) {
                            const auto result = host_write::Transfer(plan, data,
                                [&](void *part, u32 bytes, u32 *actual, u64 stage_deadline) {
                                    return m_bulk_out.PostBuffer(part, bytes, actual, stage_deadline).GetValue();
                                }, [] { return impl::UsbNowNs(); },
                                svc::ResultTimedOut().GetValue(), usb::ResultInternalStateError().GetValue());
                            host_write::Record(plan, result);
                            transferred_size = result.transferred;
                            return static_cast<Result>(result.result);
                        }
                    }
                    return m_bulk_out.PostBuffer(data, data_size, std::addressof(transferred_size), deadline_ns);
                }), 3, data_size));
            }
            *out_transferred_size = transferred_size;
        } else {
            *out_transferred_size = 0;
        }

        std::memset(csw, 0, sizeof(*csw));
        transferred_size = 0;
        auto status_result = pause_diagnostics::Measure(pause_diagnostics::Stage::BotCsw, CommandStatusWrapperLength, command[0], [&] {
            startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_BotStatus);
            return m_bulk_in.PostBuffer(m_status_buffer, CommandStatusWrapperLength, std::addressof(transferred_size), deadline_ns);
        });
        // C17 hardware: TUR failed normally, then REQUEST SENSE returned18/64
        // bytes successfully, but its CSW read reported STALL. A short data-IN
        // packet is not itself a failed command. Recover only this known status
        // STALL (native USB23.0.1 completion6 -> 0x3248C); never guess that an
        // arbitrary timeout/transaction error is a halt. The normal TUR path
        // below keeps its existing explicit HALT query for unknown Results.
        const bool sense_status_stall = status_result.GetValue() == 0x0003248Cu &&
            command_size == 6 && command[0] == ScsiRequestSense &&
            command[1] == 0 && command[2] == 0 && command[3] == 0 && command[5] == 0 &&
            command[4] == data_size && direction == DataDirection::In && data == m_data_buffer &&
            (data_size == MaxSenseDataSize || (data_size == reader_fixed_sense::FixedBytes &&
                reader_fixed_sense::RequestSize(this, static_cast<u32>(m_interface.GetId()),
                    m_transport == Transport::Bot, m_geometry.block_count, MaxSenseDataSize) == reader_fixed_sense::FixedBytes)) &&
            *out_transferred_size > 0 && *out_transferred_size <= data_size;
        // Generic BOT, not a VID/PID quirk. Only the pre-mount readiness caller
        // can opt in, once per readiness attempt. Never replay a CBW or data.
        if (R_FAILED(status_result) && transferred_size == 0 && startup_csw_recovery_used != nullptr &&
            !*startup_csw_recovery_used && m_is_initialized && !m_keep_alive_failed &&
            m_transport == Transport::Bot && m_geometry.block_count == 0 &&
            ((command_size == 6 && command[0] == ScsiTestUnitReady && direction == DataDirection::None && data_size == 0) || sense_status_stall) &&
            m_interface.IsInitialized() && m_bulk_in.IsInitialized() && m_bulk_out.IsInitialized() &&
            deadline_ns != 0 && impl::UsbNowNs() < deadline_ns) {
            *startup_csw_recovery_used = true;
            // Preserve the first transfer failure even if recovery succeeds.
            static_cast<void>(observed(status_result, 4, CommandStatusWrapperLength));
            sense_csw_recovery = sense_status_stall;
            if (sense_csw_recovery) startup_sense_csw::Begin(io_diagnostics::startup_vid.load(),
                io_diagnostics::startup_pid.load(), static_cast<u32>(m_interface.GetId()), m_lun, cbw->tag,
                m_bulk_in_endpoint_address, data_size, *out_transferred_size, status_result.GetValue(),
                static_cast<u32>(impl::UsbNowNs() / 1'000'000ULL));
            io_diagnostics::csw_recovery_attempts.fetch_add(1);
            // Retained evidence must not borrow a later device's identity.
            io_diagnostics::csw_recovery_vid.store(io_diagnostics::startup_vid.load());
            io_diagnostics::csw_recovery_pid.store(io_diagnostics::startup_pid.load());
            io_diagnostics::csw_recovery_interface_id.store(static_cast<u32>(m_interface.GetId()));
            io_diagnostics::csw_recovery_time_ns.store(impl::UsbNowNs());
            io_diagnostics::csw_recovery_lun.store(m_lun);
            io_diagnostics::csw_recovery_tag.store(cbw->tag);
            io_diagnostics::csw_recovery_endpoint.store(m_bulk_in_endpoint_address);
            io_diagnostics::csw_recovery_original.store(status_result.GetValue());
            io_diagnostics::csw_recovery_query_result.store(UINT32_MAX);
            io_diagnostics::csw_recovery_query_bytes.store(0);
            io_diagnostics::csw_recovery_halt_status.store(UINT32_MAX);
            io_diagnostics::csw_recovery_clear_result.store(UINT32_MAX);
            io_diagnostics::csw_recovery_retry_result.store(UINT32_MAX);
            io_diagnostics::csw_recovery_retry_bytes.store(0);
            io_diagnostics::csw_recovery_final_result.store(status_result.GetValue());
            const u64 recovery_deadline = std::min(deadline_ns, impl::UsbNowNs() + impl::StartupRecoveryBudgetNs);
            const auto live = [&]() {
                return impl::UsbNowNs() < recovery_deadline && m_interface.IsInitialized() &&
                    m_bulk_in.IsInitialized() && m_bulk_out.IsInitialized();
            };
            status_result = [&]() -> Result {
                io_diagnostics::csw_recovery_stage.store(1);
                u32 control_bytes = 0;
                if (!sense_csw_recovery) {
                // Use the separate status buffer, never overwrite returned
                // REQUEST SENSE bytes or the CBW tag awaiting validation.
                std::memset(m_status_buffer, 0, 2);
                const auto query = [&] {
                    startup_command_diagnostics::AttemptScope startup_recovery;
                    startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_Unknown);
                    return m_interface.ControlTransfer(RequestTypeDeviceToHostStandardEndpoint, RequestGetStatus,
                        0, m_bulk_in_endpoint_address, 2, m_status_buffer, std::addressof(control_bytes), recovery_deadline);
                }();
                io_diagnostics::csw_recovery_query_result.store(query.GetValue());
                io_diagnostics::csw_recovery_query_bytes.store(control_bytes);
                if (R_FAILED(query)) { io_diagnostics::csw_recovery_stage.store(7); return status_result; }
                const u32 halt = static_cast<u32>(m_status_buffer[0]) | (static_cast<u32>(m_status_buffer[1]) << 8);
                if (control_bytes == 2) io_diagnostics::csw_recovery_halt_status.store(halt);
                // Unknown USB Result values do not prove a STALL. Require a
                // complete standard status response with only HALT set.
                if (control_bytes != 2 || halt != 1) { io_diagnostics::csw_recovery_stage.store(2); return status_result; }
                }
                if (!live()) {
                    if (sense_csw_recovery) startup_sense_csw::Set(startup_sense_csw::Stage, 5);
                    io_diagnostics::csw_recovery_stage.store(11); return status_result;
                }
                io_diagnostics::csw_recovery_stage.store(3);
                control_bytes = 0;
                const auto clear = [&] {
                    startup_command_diagnostics::AttemptScope startup_recovery;
                    startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_Unknown);
                    return m_interface.ControlTransfer(RequestTypeHostToDeviceStandardEndpoint, RequestClearFeature,
                        FeatureEndpointHalt, m_bulk_in_endpoint_address, 0, nullptr, std::addressof(control_bytes), recovery_deadline);
                }();
                io_diagnostics::csw_recovery_clear_result.store(clear.GetValue());
                if (sense_csw_recovery) startup_sense_csw::Set(startup_sense_csw::Clear, clear.GetValue());
                if (R_FAILED(clear) || control_bytes != 0) {
                    if (sense_csw_recovery) startup_sense_csw::Set(startup_sense_csw::Stage, 2);
                    io_diagnostics::csw_recovery_stage.store(8); return status_result;
                }
                if (!live()) {
                    if (sense_csw_recovery) startup_sense_csw::Set(startup_sense_csw::Stage, 5);
                    io_diagnostics::csw_recovery_stage.store(11); return status_result;
                }
                // USB BOT 5.3.3 / figure 2: clear confirmed IN halt, then read
                // the SAME command's CSW once. All framing checks below apply.
                io_diagnostics::csw_recovery_stage.store(4);
                csw_retried = true;
                if (sense_csw_recovery) startup_sense_csw::Set(startup_sense_csw::Stage, 3);
                std::memset(csw, 0, sizeof(*csw));
                transferred_size = 0;
                const auto retry = pause_diagnostics::Measure(pause_diagnostics::Stage::BotCsw, CommandStatusWrapperLength, command[0], [&] {
                    startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_BotStatus);
                    return m_bulk_in.PostBuffer(m_status_buffer, CommandStatusWrapperLength,
                        std::addressof(transferred_size), recovery_deadline);
                });
                io_diagnostics::csw_recovery_retry_result.store(retry.GetValue());
                io_diagnostics::csw_recovery_retry_bytes.store(transferred_size);
                if (sense_csw_recovery) {
                    startup_sense_csw::Set(startup_sense_csw::Retry, retry.GetValue());
                    startup_sense_csw::Set(startup_sense_csw::Stage, 4);
                }
                return retry;
            }();
        }
        R_TRY(observed(status_result, 4, CommandStatusWrapperLength));
        R_UNLESS(transferred_size == CommandStatusWrapperLength, observed(usb::ResultTransactionError(), 5, CommandStatusWrapperLength));
        R_UNLESS(csw->signature == CommandStatusWrapperSignature, observed(usb::ResultTransactionError(), 6, CommandStatusWrapperLength));
        R_UNLESS(csw->tag == cbw->tag, observed(usb::ResultTransactionError(), 7, CommandStatusWrapperLength));
        R_UNLESS(csw->data_residue <= data_size, observed(usb::ResultTransactionError(), 8, data_size));
        if (csw->status == 1) R_RETURN(observed(usb::ResultScsiCommandFailed(), 9, data_size));
        R_UNLESS(csw->status == CswStatusPassed, observed(usb::ResultTransactionError(), 10, data_size));
        R_UNLESS(*out_transferred_size <= data_size &&
            csw->data_residue == data_size - *out_transferred_size, observed(usb::ResultTransactionError(), 11, data_size));

        if (csw_retried) {
            if (sense_csw_recovery) startup_sense_csw::Finish(0, 0, transferred_size, csw->data_residue, csw->status);
            io_diagnostics::csw_recovery_final_result.store(0);
            io_diagnostics::csw_recovery_stage.store(5);
            io_diagnostics::csw_recovery_validated.fetch_add(1);
        }

        R_SUCCEED();
    }

    Result Device::ExecuteCommandUasOnce(const u8 *command, size_t command_size, DataDirection direction, void *data, u32 data_size, u32 *out_transferred_size, u64 deadline_ns, u32 *wake_sense_size) {
        if (wake_sense_size) *wake_sense_size = 0;
        R_UNLESS(!m_wake_read_only || (direction != DataDirection::Out && command && command_size &&
            (impl::CanReplayRead(command[0]) || command[0] == ScsiRequestSense)), usb::ResultOperationDenied());
        if (m_system_power_deadline && (!deadline_ns || m_system_power_deadline < deadline_ns)) deadline_ns=m_system_power_deadline;
        R_UNLESS(!deadline_ns || impl::UsbNowNs() < deadline_ns,usb::ResultResourceBusy());
        if (direction==DataDirection::Out) m_unsynchronized_writes=true;
        auto *command_iu = reinterpret_cast<UasCommandIu *>(m_command_buffer);
        std::memset(command_iu, 0, sizeof(*command_iu));
        command_iu->iu_id = UasCommandIuId;
        u16 tag = static_cast<u16>(++m_tag);
        if (tag == 0) {
            tag = static_cast<u16>(++m_tag);
        }
        StoreBigEndian16(command_iu->tag, tag);
        std::memcpy(command_iu->cdb, command, command_size);
        startup_command_diagnostics::AttemptScope startup_attempt(command, command_size, tag,
            m_lun, m_geometry.block_size, 2);

        u32 transferred_size = 0;
        R_TRY(pause_diagnostics::Measure(pause_diagnostics::Stage::UasCommand, sizeof(*command_iu), command[0], [&] {
            startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_UasCommand);
            return m_uas_command.PostBuffer(m_command_buffer, sizeof(*command_iu), std::addressof(transferred_size), deadline_ns);
        }));
        R_UNLESS(transferred_size == sizeof(*command_iu), usb::ResultTransactionError());

        if (data_size > 0) {
            if (direction == DataDirection::In) {
                R_TRY(pause_diagnostics::Measure(pause_diagnostics::Stage::UasData, data_size, command[0], [&] {
                    startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_UasData);
                    return m_uas_data_in.PostBuffer(data, data_size, std::addressof(transferred_size), deadline_ns);
                }));
            } else {
                R_TRY(pause_diagnostics::Measure(pause_diagnostics::Stage::UasData, data_size, command[0], [&] {
                    startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_UasData);
                    return m_uas_data_out.PostBuffer(data, data_size, std::addressof(transferred_size), deadline_ns);
                }));
            }
            *out_transferred_size = transferred_size;
        } else {
            *out_transferred_size = 0;
        }

        auto *status_iu = reinterpret_cast<UasStatusIu *>(m_status_buffer);
        std::memset(status_iu, 0, sizeof(*status_iu));
        R_TRY(pause_diagnostics::Measure(pause_diagnostics::Stage::UasStatus, sizeof(*status_iu), command[0], [&] {
            startup_command_diagnostics::SegmentScope startup_segment(NexusStartupCommand_UasStatus);
            return m_uas_status.PostBuffer(m_status_buffer, sizeof(*status_iu), std::addressof(transferred_size), deadline_ns);
        }));
        R_UNLESS(transferred_size >= 16, usb::ResultTransactionError());
        R_UNLESS(status_iu->iu_id == UasStatusIuId, usb::ResultTransactionError());
        R_UNLESS(LoadBigEndian16(status_iu->tag) == tag, usb::ResultTransactionError());
        if (wake_sense_size) {
            *wake_sense_size = static_cast<u32>(sleep::WakeSenseSize(status_iu->status,
                LoadBigEndian16(status_iu->sense_length), transferred_size, sizeof(status_iu->sense)));
            if (*wake_sense_size) R_THROW(usb::ResultScsiCommandFailed());
        }
        R_UNLESS(status_iu->status == CswStatusPassed, usb::ResultTransactionError());
        R_SUCCEED();
    }

    void Device::StoreBigEndian16(u8 *dst, u16 value) {
        dst[0] = static_cast<u8>(value >> 8);
        dst[1] = static_cast<u8>(value >> 0);
    }

    void Device::StoreBigEndian32(u8 *dst, u32 value) {
        dst[0] = static_cast<u8>(value >> 24);
        dst[1] = static_cast<u8>(value >> 16);
        dst[2] = static_cast<u8>(value >> 8);
        dst[3] = static_cast<u8>(value >> 0);
    }

    void Device::StoreBigEndian64(u8 *dst, u64 value) {
        StoreBigEndian32(dst + 0, static_cast<u32>(value >> 32));
        StoreBigEndian32(dst + 4, static_cast<u32>(value >> 0));
    }

    u16 Device::LoadBigEndian16(const u8 *src) {
        return (static_cast<u16>(src[0]) << 8) | static_cast<u16>(src[1]);
    }

    u32 Device::LoadBigEndian32(const u8 *src) {
        return (static_cast<u32>(src[0]) << 24) | (static_cast<u32>(src[1]) << 16) | (static_cast<u32>(src[2]) << 8) | static_cast<u32>(src[3]);
    }

    u64 Device::LoadBigEndian64(const u8 *src) {
        return (static_cast<u64>(LoadBigEndian32(src + 0)) << 32) | static_cast<u64>(LoadBigEndian32(src + 4));
    }

    void Device::CopyInquiryString(char *dst, size_t dst_size, const u8 *src, size_t src_size) {
        AMS_ASSERT(dst_size > 0);
        const size_t copy_size = std::min(dst_size - 1, src_size);
        std::memcpy(dst, src, copy_size);
        dst[copy_size] = '\0';
        while (copy_size > 0 && dst[copy_size - 1] == ' ') {
            dst[copy_size - 1] = '\0';
        }
    }

}

#endif
