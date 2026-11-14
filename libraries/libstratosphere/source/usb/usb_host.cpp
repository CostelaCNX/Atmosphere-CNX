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
#include <stratosphere/usb/usb_runtime_diagnostics.hpp>
#include <stratosphere/usb/usb_pause_diagnostics.hpp>
#include <stratosphere/usb/usb_completion_diagnostics.hpp>
#include <stratosphere/usb/usb_completion_probe.hpp>
#include <stratosphere/usb/usb_probe_trace.hpp>
#include <stratosphere/usb/usb_startup_history.hpp>
#include "usb_keep_alive.hpp"

#if defined(ATMOSPHERE_OS_HORIZON)
#include <switch/arm/cache.h>
#endif

#if defined(ATMOSPHERE_OS_HORIZON)

namespace ams::usb::host {

    namespace {

        /* HDD spin-up and some flash controllers can take several seconds
         * before completing a BOT request. Keep the wait bounded, but avoid
         * treating a slow medium as a disconnected device. */
        constexpr u64 EndpointTransferTimeoutNs = 15'000'000'000ULL;

        #if defined(ATMOSPHERE_OS_HORIZON)
            void CloseService(::Service *service) {
                if (serviceIsActive(service)) {
                    serviceAssumeDomain(service);
                    serviceClose(service);
                }
                *service = {};
            }

            Result GetRemoteEvent(::Service *service, ::Event *out_event, bool autoclear, u32 command_id) {
                Handle handle = INVALID_HANDLE;
                serviceAssumeDomain(service);
                R_TRY(serviceDispatch(service, command_id,
                    .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
                    .out_handles = { &handle },
                ));
                eventLoadRemote(out_event, handle, autoclear);
                R_SUCCEED();
            }

            template<typename In>
            Result DispatchIn(::Service *service, u32 command_id, const In &in, const SfDispatchParams &params = {}) {
                return ::serviceDispatchImpl(service, command_id, std::addressof(in), sizeof(in), nullptr, 0, params);
            }

            template<typename Out>
            Result DispatchOut(::Service *service, u32 command_id, Out &out, const SfDispatchParams &params = {}) {
                return ::serviceDispatchImpl(service, command_id, nullptr, 0, std::addressof(out), sizeof(out), params);
            }

            template<typename In, typename Out>
            Result DispatchInOut(::Service *service, u32 command_id, const In &in, Out &out, const SfDispatchParams &params = {}) {
                return ::serviceDispatchImpl(service, command_id, std::addressof(in), sizeof(in), std::addressof(out), sizeof(out), params);
            }

            void ConvertInterfaceInfoToV8(InterfaceInfo *info) {
                if (info == nullptr || hos::GetVersion() >= hos::Version_8_0_0) {
                    return;
                }

                InterfaceInfo tmp;
                std::memcpy(std::addressof(tmp), info, sizeof(tmp));
                std::memcpy(info->output_endpoint_descs, tmp.input_endpoint_descs, sizeof(tmp.input_endpoint_descs));
                std::memcpy(info->input_endpoint_descs, tmp.output_endpoint_descs, sizeof(tmp.output_endpoint_descs));
                std::memcpy(info->output_ss_endpoint_companion_descs, tmp.input_ss_endpoint_companion_descs, sizeof(tmp.input_ss_endpoint_companion_descs));
                std::memcpy(info->input_ss_endpoint_companion_descs, tmp.output_ss_endpoint_companion_descs, sizeof(tmp.output_ss_endpoint_companion_descs));
            }

            void ConvertInterfacesToV8(Interface *interfaces, s32 count) {
                for (s32 i = 0; i < count; ++i) {
                    ConvertInterfaceInfoToV8(std::addressof(interfaces[i].inf));
                }
            }

            Result QueryInterfaces(::Service *service, u32 base_command_id, const InterfaceFilter *filter, Interface *interfaces, size_t interfaces_size, s32 *out_count) {
                if (out_count != nullptr) *out_count = 0;
                s32 count = 0;
                serviceAssumeDomain(service);
                R_TRY(DispatchInOut(service, hos::GetVersion() >= hos::Version_2_0_0 ? base_command_id + 1 : base_command_id, *filter, count, (SfDispatchParams){
                    .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                    .buffers = { { interfaces, interfaces_size } },
                }));
                R_UNLESS(count >= 0 && static_cast<size_t>(count) <= interfaces_size / sizeof(*interfaces), usb::ResultInternalStateError());
                if (out_count != nullptr) {
                    *out_count = count;
                }
                ConvertInterfacesToV8(interfaces, count);
                R_SUCCEED();
            }
        #endif

    }

    Client::~Client() {
        if (m_is_initialized) {
            static_cast<void>(this->Finalize());
        }
    }

    Result Client::Initialize() {
        R_UNLESS(!m_is_initialized, usb::ResultAlreadyInitialized());

        #if defined(ATMOSPHERE_OS_HORIZON)
            os::NativeHandle handle;
            R_TRY(sm::GetServiceHandle(std::addressof(handle), sm::ServiceName::Encode("usb:hs")));
            ::serviceCreate(std::addressof(m_service), handle);
            bool success = false;
            ON_SCOPE_EXIT {
                if (!success) {
                    if (m_interface_state_change_event_initialized) {
                        eventClose(std::addressof(m_interface_state_change_event));
                        m_interface_state_change_event_initialized = false;
                    }
                    CloseService(std::addressof(m_service));
                }
            };
            R_TRY(serviceConvertToDomain(std::addressof(m_service)));

            /* usbHsInitialize() binds the caller process before any client
             * operation on HOS 2.0+. Reproduce that against this private
             * service object instead of libnx's global service state. */
            if (hos::GetVersion() >= hos::Version_2_0_0) {
                serviceAssumeDomain(std::addressof(m_service));
                R_TRY(serviceDispatch(std::addressof(m_service), 0,
                    .in_num_handles = 1,
                    .in_handles = { dd::GetCurrentProcessHandle() },
                ));
            }

            /* The service client owns an interface-state event. This is part
             * of the usbHsInitialize() ABI even when the caller polls; not
             * acquiring it leaves the service session only partially bound. */
            R_TRY(GetRemoteEvent(std::addressof(m_service), std::addressof(m_interface_state_change_event), false,
                hos::GetVersion() >= hos::Version_2_0_0 ? 6 : 5));
            m_interface_state_change_event_initialized = true;

            m_is_initialized = true;
            success = true;
        #else
            AMS_ABORT("TODO");
        #endif

        R_SUCCEED();
    }

    Result Client::Finalize() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());

        #if defined(ATMOSPHERE_OS_HORIZON)
            if (m_interface_state_change_event_initialized) {
                eventClose(std::addressof(m_interface_state_change_event));
                m_interface_state_change_event_initialized = false;
            }
            CloseService(std::addressof(m_service));
        #endif
        m_is_initialized = false;
        R_SUCCEED();
    }

    Result Client::QueryAvailableInterfaces(const InterfaceFilter *filter, Interface *interfaces, size_t interfaces_size, s32 *out_count) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(filter != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(interfaces != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(out_count != nullptr, usb::ResultInvalidParameter());

        #if defined(ATMOSPHERE_OS_HORIZON)
            R_RETURN(QueryInterfaces(std::addressof(m_service), 1, filter, interfaces, interfaces_size, out_count));
        #else
            AMS_ABORT("TODO");
        #endif
    }

    Result Client::QueryAllInterfaces(const InterfaceFilter *filter, Interface *interfaces, size_t interfaces_size, s32 *out_count) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(filter != nullptr && interfaces != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        R_RETURN(QueryInterfaces(std::addressof(m_service), 0, filter, interfaces, interfaces_size, out_count));
    }

    Result Client::QueryAcquiredInterfaces(Interface *interfaces, size_t interfaces_size, s32 *out_count) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(interfaces != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(out_count != nullptr, usb::ResultInvalidParameter());
        *out_count = 0;

        #if defined(ATMOSPHERE_OS_HORIZON)
            s32 count = 0;
            serviceAssumeDomain(std::addressof(m_service));
            R_TRY(DispatchOut(std::addressof(m_service), hos::GetVersion() >= hos::Version_2_0_0 ? 3 : 2, count, (SfDispatchParams){
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { interfaces, interfaces_size } },
            }));
            R_UNLESS(count >= 0 && static_cast<size_t>(count) <= interfaces_size / sizeof(*interfaces), usb::ResultInternalStateError());
            *out_count = count;
            ConvertInterfacesToV8(interfaces, count);
            R_SUCCEED();
        #else
            AMS_ABORT("TODO");
        #endif
    }

    void *Client::GetInterfaceStateChangeEvent() {
        #if defined(ATMOSPHERE_OS_HORIZON)
            return m_interface_state_change_event_initialized ? std::addressof(m_interface_state_change_event) : nullptr;
        #else
            return nullptr;
        #endif
    }

    InterfaceSession::~InterfaceSession() {
        if (m_is_initialized) {
            static_cast<void>(this->Finalize());
        }
    }

    Result InterfaceSession::Initialize(Client *client, Interface *interface) {
        R_UNLESS(!m_is_initialized, usb::ResultAlreadyInitialized());
        R_UNLESS(client != nullptr && client->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(interface != nullptr, usb::ResultInvalidParameter());

        #if defined(ATMOSPHERE_OS_HORIZON)
            std::memset(std::addressof(m_session), 0, sizeof(m_session));
            std::memcpy(std::addressof(m_session.inf), interface, sizeof(*interface));
            m_session.ID = interface->inf.ID;

            bool success = false;
            ON_SCOPE_EXIT {
                if (!success) {
                    if (m_event_ctrl_xfer_initialized) {
                        eventClose(std::addressof(m_session.eventCtrlXfer));
                        m_event_ctrl_xfer_initialized = false;
                    }
                    if (m_event0_initialized) {
                        eventClose(std::addressof(m_session.event0));
                        m_event0_initialized = false;
                    }
                    CloseService(std::addressof(m_session.s));
                    std::memset(std::addressof(m_session), 0, sizeof(m_session));
                }
            };

            serviceAssumeDomain(std::addressof(client->m_service));
            Result result;
            if (hos::GetVersion() >= hos::Version_2_0_0) {
                result = DispatchIn(std::addressof(client->m_service), 7, m_session.ID, (SfDispatchParams){
                    .buffer_attrs = {
                        SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                        SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                    },
                    .buffers = {
                        { &m_session.inf.pathstr, sizeof(Interface) - sizeof(InterfaceInfo) },
                        { &m_session.inf.inf, sizeof(InterfaceInfo) },
                    },
                    .out_num_objects = 1,
                    .out_objects = { &m_session.s },
                });
            } else {
                result = DispatchIn(std::addressof(client->m_service), 6, m_session.ID, (SfDispatchParams){
                    .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                    .buffers = { { &m_session.inf.inf, sizeof(InterfaceInfo) } },
                    .out_num_objects = 1,
                    .out_objects = { &m_session.s },
                });
            }
            R_TRY(result);
            ConvertInterfaceInfoToV8(std::addressof(interface->inf));

            R_TRY(GetRemoteEvent(std::addressof(m_session.s), std::addressof(m_session.event0), false, 0));
            m_event0_initialized = true;
            if (hos::GetVersion() >= hos::Version_2_0_0) {
                R_TRY(GetRemoteEvent(std::addressof(m_session.s), std::addressof(m_session.eventCtrlXfer), false, 6));
                m_event_ctrl_xfer_initialized = true;
            }
            success = true;
        #endif
        m_client         = client;
        m_is_initialized = true;
        R_SUCCEED();
    }

    Result InterfaceSession::Finalize() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());

        #if defined(ATMOSPHERE_OS_HORIZON)
            if (m_event_ctrl_xfer_initialized) {
                eventClose(std::addressof(m_session.eventCtrlXfer));
                m_event_ctrl_xfer_initialized = false;
            }
            if (m_event0_initialized) {
                eventClose(std::addressof(m_session.event0));
                m_event0_initialized = false;
            }
            if (serviceIsActive(std::addressof(m_session.s))) {
                serviceAssumeDomain(std::addressof(m_session.s));
                serviceClose(std::addressof(m_session.s));
            }
            std::memset(std::addressof(m_session), 0, sizeof(m_session));
        #endif
        m_client         = nullptr;
        m_is_initialized = false;
        R_SUCCEED();
    }

    void InterfaceSession::Abort() {
        #if defined(ATMOSPHERE_OS_HORIZON)
            if (m_event_ctrl_xfer_initialized) {
                eventClose(std::addressof(m_session.eventCtrlXfer));
                m_event_ctrl_xfer_initialized = false;
            }
            if (m_event0_initialized) {
                eventClose(std::addressof(m_session.event0));
                m_event0_initialized = false;
            }
            if (serviceIsActive(std::addressof(m_session.s))) {
                serviceAssumeDomain(std::addressof(m_session.s));
                serviceClose(std::addressof(m_session.s));
            }
            std::memset(std::addressof(m_session), 0, sizeof(m_session));
        #endif
        m_client         = nullptr;
        m_is_initialized = false;
    }

    s32 InterfaceSession::GetId() const {
        return m_is_initialized ? m_session.ID : -1;
    }

    Result InterfaceSession::GetInterfaceInfo(InterfaceInfo *out) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());

        #if defined(ATMOSPHERE_OS_HORIZON)
            serviceAssumeDomain(std::addressof(m_session.s));
            R_TRY(serviceDispatch(std::addressof(m_session.s), 2,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { out, sizeof(*out) } },
            ));
            ConvertInterfaceInfoToV8(out);
            R_SUCCEED();
        #else
            AMS_ABORT("TODO");
        #endif
    }

    Result InterfaceSession::SelectInterface(InterfaceInfo *out, u8 alternate) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        serviceAssumeDomain(std::addressof(m_session.s));
        R_TRY(DispatchIn(std::addressof(m_session.s), 1, alternate, (SfDispatchParams){
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { out, sizeof(*out) } },
        }));
        ConvertInterfaceInfoToV8(out);
        R_SUCCEED();
    }

    Result InterfaceSession::ControlTransfer(u8 bm_request_type, u8 b_request, u16 w_value, u16 w_index, u16 w_length, void *buffer, u32 *out_transferred_size, u64 deadline_ns) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(out_transferred_size != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(w_length == 0 || (buffer != nullptr && usb::IsDmaAligned(reinterpret_cast<u64>(buffer))), usb::ResultAlignmentError());
        *out_transferred_size = 0;
        const u64 wait_ns = impl::TransferWaitNs(impl::UsbNowNs(), deadline_ns, EndpointTransferTimeoutNs);
        R_UNLESS(wait_ns != 0, svc::ResultTimedOut());

        #if defined(ATMOSPHERE_OS_HORIZON)
            if (hos::GetVersion() < hos::Version_2_0_0) {
                const struct {
                    u8 b_request;
                    u8 bm_request_type;
                    u16 w_value;
                    u16 w_index;
                    u16 w_length;
                    u32 timeout_in_ms;
                } in{b_request, bm_request_type, w_value, w_index, w_length, static_cast<u32>((wait_ns + 999'999) / 1'000'000)};
                const size_t buffer_size = (w_length + 0xFFF) & ~0xFFF;
                if (buffer != nullptr && w_length != 0) {
                    armDCacheFlush(buffer, w_length);
                }
                serviceAssumeDomain(std::addressof(m_session.s));
                R_TRY(DispatchInOut(std::addressof(m_session.s), (bm_request_type & USB_ENDPOINT_IN) != 0 ? 6 : 7, in, *out_transferred_size, (SfDispatchParams){
                    .buffer_attrs = { static_cast<u32>(SfBufferAttr_HipcMapAlias | (((bm_request_type & USB_ENDPOINT_IN) != 0) ? SfBufferAttr_Out : SfBufferAttr_In)) },
                    .buffers = { { buffer, buffer_size } },
                }));
                if (buffer != nullptr && (bm_request_type & USB_ENDPOINT_IN) != 0) {
                    armDCacheFlush(buffer, w_length);
                }
                R_SUCCEED();
            }

            const struct {
                u8 bm_request_type;
                u8 b_request;
                u16 w_value;
                u16 w_index;
                u16 w_length;
                u64 buffer;
            } in{bm_request_type, b_request, w_value, w_index, w_length, reinterpret_cast<u64>(buffer)};
            const u64 started_ns = impl::UsbNowNs();
            ::UsbHsXferReport report{};
            bool report_present = false;
            const auto observe = [&](Result rc, u32 phase, u32 actual = 0) {
                probe_trace::Observe(true, phase, rc.GetValue(), w_length, actual, b_request);
                startup_history::Record(1,phase,rc.GetValue(),w_length,actual,b_request,
                    static_cast<u32>((impl::UsbNowNs()-started_ns)/1'000'000),report.requestedSize,
                    report.xferId,0,report.id,report_present,bm_request_type,w_value,w_index,
                    startup_history::Micros(impl::UsbNowNs()-started_ns),report_present?1:0);
                return rc;
            };
            serviceAssumeDomain(std::addressof(m_session.s));
            R_TRY(observe(DispatchIn(std::addressof(m_session.s), 5, in), 1));
            const u64 remaining_ns = impl::TransferWaitNs(impl::UsbNowNs(), deadline_ns, EndpointTransferTimeoutNs);
            const Result wait_result = remaining_ns != 0 ? static_cast<Result>(eventWait(std::addressof(m_session.eventCtrlXfer), remaining_ns)) : static_cast<Result>(svc::ResultTimedOut());
            if (R_FAILED(wait_result)) {
                observe(wait_result, 2);
                this->Abort();
                R_RETURN(wait_result);
            }
            eventClear(std::addressof(m_session.eventCtrlXfer));

            serviceAssumeDomain(std::addressof(m_session.s));
            const Result report_result = serviceDispatch(std::addressof(m_session.s), 7,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { std::addressof(report), sizeof(report) } },
            );
            if (R_FAILED(report_result)) {
                observe(report_result,3);
                this->Abort(); // Completion ownership unknown; never reuse it.
                R_RETURN(report_result);
            }
            report_present = true;
            // Failed native reports may contain invalid lengths (C15 showed
            // FFFFFFF9). Retain those as evidence, never expose them as data.
            if (R_FAILED(static_cast<Result>(report.res))) R_RETURN(observe(report.res,5,report.transferredSize));
            if (report.transferredSize > w_length) {
                observe(usb::ResultInternalStateError(),4,report.transferredSize);
                this->Abort();
                R_RETURN(usb::ResultInternalStateError());
            }
            *out_transferred_size = report.transferredSize;
            R_RETURN(observe(report.res, 5, report.transferredSize));
        #else
            AMS_ABORT("TODO");
        #endif
    }

    Result InterfaceSession::ResetDevice() {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        #if defined(ATMOSPHERE_OS_HORIZON)
            serviceAssumeDomain(std::addressof(m_session.s));
            R_RETURN(serviceDispatch(std::addressof(m_session.s), 8));
        #else
            AMS_ABORT("TODO");
        #endif
    }

    EndpointSession::~EndpointSession() {
        if (m_is_initialized) {
            static_cast<void>(this->Finalize());
        }
    }

    Result EndpointSession::Initialize(InterfaceSession *interface, u16 max_urb_count, u32 max_transfer_size, EndpointDescriptor *descriptor) {
        R_UNLESS(!m_is_initialized, usb::ResultAlreadyInitialized());
        R_UNLESS(interface != nullptr && interface->IsInitialized(), usb::ResultNotInitialized());
        R_UNLESS(descriptor != nullptr, usb::ResultInvalidParameter());

        #if defined(ATMOSPHERE_OS_HORIZON)
            const struct {
                u16 max_urb_count;
                u16 pad;
                u32 ep_type;
                u32 ep_number;
                u32 ep_direction;
                u32 max_transfer_size;
            } in{
                max_urb_count,
                0,
                static_cast<u32>((descriptor->bmAttributes & USB_TRANSFER_TYPE_MASK) + 1),
                static_cast<u32>(descriptor->bEndpointAddress & USB_ENDPOINT_ADDRESS_MASK),
                static_cast<u32>((descriptor->bEndpointAddress & USB_ENDPOINT_IN) == 0 ? 0x1 : 0x2),
                max_transfer_size,
            };
            bool success = false;
            const u64 started_ns = impl::UsbNowNs();
            const auto observed_open = [&](Result rc,u32 phase) {
                startup_history::Record(3,phase,rc.GetValue(),max_transfer_size,m_session.desc.wMaxPacketSize,
                    descriptor->bEndpointAddress,static_cast<u32>((impl::UsbNowNs()-started_ns)/1'000'000),
                    max_urb_count,m_session.desc.bEndpointAddress);
                return rc;
            };
            ON_SCOPE_EXIT {
                if (!success) {
                    if (m_event_xfer_initialized) {
                        eventClose(std::addressof(m_session.eventXfer));
                        m_event_xfer_initialized = false;
                    }
                    CloseService(std::addressof(m_session.s));
                    std::memset(std::addressof(m_session), 0, sizeof(m_session));
                }
            };
            serviceAssumeDomain(std::addressof(interface->m_session.s));
            R_TRY(observed_open(DispatchInOut(std::addressof(interface->m_session.s), hos::GetVersion() >= hos::Version_2_0_0 ? 9 : 4, in, m_session.desc, (SfDispatchParams){
                .out_num_objects = 1,
                .out_objects = { &m_session.s },
            }),10));
            if (hos::GetVersion() >= hos::Version_2_0_0) {
                serviceAssumeDomain(std::addressof(m_session.s));
                R_TRY(observed_open(serviceDispatch(std::addressof(m_session.s), 3),11));
                R_TRY(observed_open(GetRemoteEvent(std::addressof(m_session.s), std::addressof(m_session.eventXfer), false, 2),12));
                m_event_xfer_initialized = true;
            }
            m_session.maxUrbCount = max_urb_count;
            success = true;
        #endif
        m_interface      = interface;
        m_is_initialized = true;
        R_SUCCEED();
    }

    Result EndpointSession::Finalize() {
        R_SUCCEED_IF(!m_is_initialized);

        #if defined(ATMOSPHERE_OS_HORIZON)
            if (m_event_xfer_initialized) {
                eventClose(std::addressof(m_session.eventXfer));
                m_event_xfer_initialized = false;
            }
            if (serviceIsActive(std::addressof(m_session.s))) {
                serviceAssumeDomain(std::addressof(m_session.s));
                serviceDispatch(std::addressof(m_session.s), hos::GetVersion() >= hos::Version_2_0_0 ? 1 : 3);
                serviceAssumeDomain(std::addressof(m_session.s));
                serviceClose(std::addressof(m_session.s));
            }
            std::memset(std::addressof(m_session), 0, sizeof(m_session));
        #endif
        m_interface      = nullptr;
        m_is_initialized = false;
        R_SUCCEED();
    }

    void EndpointSession::Abort() {
        #if defined(ATMOSPHERE_OS_HORIZON)
            if (serviceIsActive(std::addressof(m_session.s))) {
                serviceAssumeDomain(std::addressof(m_session.s));
                static_cast<void>(serviceDispatch(std::addressof(m_session.s), hos::GetVersion() >= hos::Version_2_0_0 ? 1 : 3));
                serviceAssumeDomain(std::addressof(m_session.s));
                serviceClose(std::addressof(m_session.s));
            }
            if (m_event_xfer_initialized) {
                eventClose(std::addressof(m_session.eventXfer));
                m_event_xfer_initialized = false;
            }
            std::memset(std::addressof(m_session), 0, sizeof(m_session));
        #endif
        m_interface      = nullptr;
        m_is_initialized = false;
    }

    Result EndpointSession::PostBuffer(void *buffer, u32 size, u32 *out_transferred_size, u64 deadline_ns) {
        R_UNLESS(m_is_initialized, usb::ResultNotInitialized());
        R_UNLESS(buffer != nullptr && out_transferred_size != nullptr, usb::ResultInvalidParameter());
        R_UNLESS(usb::IsDmaAligned(reinterpret_cast<u64>(buffer)), usb::ResultAlignmentError());
        *out_transferred_size = 0;
        const u64 transfer_started_ns = impl::UsbNowNs();
        const u64 wait_ns = impl::TransferWaitNs(transfer_started_ns, deadline_ns, EndpointTransferTimeoutNs);
        R_UNLESS(wait_ns != 0, svc::ResultTimedOut());

        #if defined(ATMOSPHERE_OS_HORIZON)
            if (hos::GetVersion() < hos::Version_2_0_0) {
                const bool direction_in = (m_session.desc.bEndpointAddress & USB_ENDPOINT_IN) != 0;
                const struct {
                    u32 size;
                    u32 timeout_in_ms;
                } in{size, static_cast<u32>((wait_ns + 999'999) / 1'000'000)};
                const size_t buffer_size = (size + 0xFFF) & ~0xFFF;
                armDCacheFlush(buffer, size);
                serviceAssumeDomain(std::addressof(m_session.s));
                /* HOS 1.x exposes one synchronous transfer IPC; submission
                 * includes completion because no separate wait/report exists. */
                R_TRY(completion_diagnostics::MeasureHost(completion_diagnostics::HostStage::Submit, size, m_session.desc.bEndpointAddress, 0, [&] {
                    return DispatchInOut(std::addressof(m_session.s), direction_in ? 1 : 0, in, *out_transferred_size, (SfDispatchParams){
                        .buffer_attrs = { static_cast<u32>(SfBufferAttr_HipcMapAlias | (direction_in ? SfBufferAttr_Out : SfBufferAttr_In)) },
                        .buffers = { { buffer, buffer_size } },
                    });
                }));
                if (direction_in) {
                    armDCacheFlush(buffer, size);
                }
                R_SUCCEED();
            }

            // Snapshot once: expiration affects the next transfer, not ownership
            // of this submitted buffer. Normal mode keeps event-only waiting.
            const bool assisted = completion_probe::SelectedMode() == NEXUS_USB_COMPLETION_PROBE_ASSISTED;
            const struct {
                u32 size;
                u32 pad;
                u64 buffer;
                u64 id;
            } in{size, 0, reinterpret_cast<u64>(buffer), 0};
            u32 xfer_id = 0;
            // One deadline for the submitted transfer, including empty/stale
            // event notifications. Never submit it again (it may be a write).
            const u64 completion_deadline = wait_ns <= UINT64_MAX - transfer_started_ns ? transfer_started_ns + wait_ns : UINT64_MAX;
            const u64 started_ns = impl::UsbNowNs();
            ::UsbHsXferReport report{};
            bool report_present = false;
            u32 observed_report_count = 0;
            const auto observe = [&](Result rc, u32 phase, u32 actual = 0) {
                probe_trace::Observe(false, phase, rc.GetValue(), size, actual, m_session.desc.bEndpointAddress);
                startup_history::Record(2,phase,rc.GetValue(),size,actual,m_session.desc.bEndpointAddress,
                    static_cast<u32>((impl::UsbNowNs()-started_ns)/1'000'000),report.requestedSize,
                    report.xferId,xfer_id,report.id,report_present,0,0,0,
                    startup_history::Micros(impl::UsbNowNs()-started_ns),observed_report_count);
                return rc;
            };
            R_UNLESS(impl::UsbNowNs() < completion_deadline, svc::ResultTimedOut());
            serviceAssumeDomain(std::addressof(m_session.s));
            /* This layer has no SCSI opcode. Correlate these spans with the
             * enclosing BOT/UAS stage by timestamp; opcode zero is unspecified. */
            R_TRY(observe(completion_diagnostics::MeasureHost(completion_diagnostics::HostStage::Submit, size, m_session.desc.bEndpointAddress, xfer_id, [&] {
                return DispatchInOut(std::addressof(m_session.s), 4, in, xfer_id);
            }), 1));
            for (;;) {
            const u64 remaining_ns = impl::TransferWaitNs(impl::UsbNowNs(), completion_deadline, EndpointTransferTimeoutNs);
            const u64 slice_ns = assisted ? std::min<u64>(remaining_ns, NEXUS_USB_COMPLETION_PROBE_INTERVAL_MS * 1'000'000ULL) : remaining_ns;
            Result wait_result = completion_diagnostics::MeasureHost(completion_diagnostics::HostStage::Wait, size, m_session.desc.bEndpointAddress, xfer_id, [&] {
                return slice_ns != 0 ? static_cast<Result>(eventWait(std::addressof(m_session.eventXfer), slice_ns)) : static_cast<Result>(svc::ResultTimedOut());
            });
            bool no_event_probe = false;
            if (assisted && svc::ResultTimedOut::Includes(wait_result) && impl::UsbNowNs() < completion_deadline) {
                completion_probe::Count(completion_probe::Counter::SliceTimeout);
                // Completion may race with the slice timeout. Recheck the
                // manual-reset event before probing its report queue.
                wait_result = completion_diagnostics::MeasureHost(completion_diagnostics::HostStage::Wait, size, m_session.desc.bEndpointAddress, xfer_id, [&] {
                    return static_cast<Result>(eventWait(std::addressof(m_session.eventXfer), 0));
                });
                if (R_SUCCEEDED(wait_result)) completion_probe::Count(completion_probe::Counter::EventRace);
                else if (svc::ResultTimedOut::Includes(wait_result) && impl::UsbNowNs() < completion_deadline) no_event_probe = true;
            }
            if (R_FAILED(wait_result) && !no_event_probe) {
                observe(wait_result, 2);
                this->Abort();
                R_RETURN(wait_result);
            }
            if (!no_event_probe) eventClear(std::addressof(m_session.eventXfer));

            report = {};
            report_present = false;
            u32 count = 0;
            serviceAssumeDomain(std::addressof(m_session.s));
            u32 max_reports = 1;
            if (no_event_probe) completion_probe::Count(completion_probe::Counter::Query);
            const auto report_result = completion_diagnostics::MeasureHost(completion_diagnostics::HostStage::Report, size, m_session.desc.bEndpointAddress, xfer_id, [&] {
                return DispatchInOut(std::addressof(m_session.s), 5, max_reports, count, (SfDispatchParams){
                    .buffer_attrs = { static_cast<u32>((hos::GetVersion() < hos::Version_3_0_0 ? SfBufferAttr_HipcMapAlias : SfBufferAttr_HipcAutoSelect) | SfBufferAttr_Out) },
                    .buffers = { { std::addressof(report), sizeof(report) } },
                });
            });
            observed_report_count = count;
            if (R_FAILED(report_result)) {
                if (no_event_probe) completion_probe::Count(completion_probe::Counter::Error);
                observe(report_result, 3);
                this->Abort(); // Completion ownership is unknown; do not reuse.
                R_RETURN(report_result);
            }
            if (count == 0) {
                if (no_event_probe) {
                    completion_probe::Count(completion_probe::Counter::Empty);
                    continue; // Pending, not a stale event. Never re-submit.
                }
                runtime_diagnostics::empty_reports.fetch_add(1, std::memory_order_relaxed);
                // Notification alone is not completion. Keep waiting for the
                // same transfer; bound CPU use if the host repeatedly signals.
                os::SleepThread(TimeSpan::FromMilliSeconds(1));
                continue;
            }
            report_present = true;
            if (count != 1 || report.xferId != xfer_id || (R_SUCCEEDED(static_cast<Result>(report.res)) && report.transferredSize > size)) {
                if (no_event_probe) completion_probe::Count(completion_probe::Counter::Error);
                observe(usb::ResultInternalStateError(), 4, report.transferredSize);
                runtime_diagnostics::invalid_reports.fetch_add(1, std::memory_order_relaxed);
                this->Abort();
                R_THROW(usb::ResultInternalStateError());
            }
            if (no_event_probe) {
                completion_probe::Count(completion_probe::Counter::Ready);
                // Only this transfer owns the endpoint until return. A late
                // signal after this clear is harmless: next call drains zero
                // reports and continues waiting for its own transfer id.
                eventClear(std::addressof(m_session.eventXfer));
            }
            // A failed native completion may contain an invalid unsigned
            // length. Preserve its Result; never expose failed-report bytes.
            if (R_FAILED(static_cast<Result>(report.res))) R_RETURN(observe(report.res, 5, report.transferredSize));
            *out_transferred_size = report.transferredSize;
            R_RETURN(observe(report.res, 5, report.transferredSize));
            }
        #else
            AMS_ABORT("TODO");
        #endif
    }

}

#endif
