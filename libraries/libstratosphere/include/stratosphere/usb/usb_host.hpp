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
#pragma once

#include <vapours.hpp>
#include <stratosphere/usb/usb_limits.hpp>

/*
 * This is deliberately a thin wrapper around libnx's usb:hs client API.
 * Keeping the libnx types out of non-Horizon builds lets libstratosphere's
 * generic targets continue to include <stratosphere/usb.hpp>.
 */
#if defined(ATMOSPHERE_OS_HORIZON)
#include <switch/services/usbhs.h>
#endif

namespace ams::usb::host {

    #if defined(ATMOSPHERE_OS_HORIZON)
        using InterfaceFilter    = ::UsbHsInterfaceFilter;
        using Interface           = ::UsbHsInterface;
        using InterfaceInfo       = ::UsbHsInterfaceInfo;
        using EndpointDescriptor  = ::usb_endpoint_descriptor;
    #else
        /* Generic builds do not have a usb:hs ABI. These are compile-only
         * placeholders; the implementation is Horizon-specific. */
        struct InterfaceFilter {};
        struct Interface {};
        struct InterfaceInfo {};
        struct EndpointDescriptor {};
    #endif

    class Client;
    class InterfaceSession;

    class Client {
        NON_COPYABLE(Client);
        NON_MOVEABLE(Client);
        private:
            #if defined(ATMOSPHERE_OS_HORIZON)
                /* Keep a private usb:hs session. libnx's usbHsInitialize()
                 * owns a process-global service guard, which is not safe to
                 * use from an Atmosphere sysmodule. */
                ::Service m_service{};
                ::Event m_interface_state_change_event{};
            #endif
            bool m_interface_state_change_event_initialized{false};
            bool m_is_initialized{false};
        public:
            Client() = default;
            ~Client();
        public:
            Result Initialize();
            Result Finalize();

            bool IsInitialized() const { return m_is_initialized; }

            Result QueryAvailableInterfaces(const InterfaceFilter *filter, Interface *interfaces, size_t interfaces_size, s32 *out_count);
            // Descriptor inventory only: returned IDs cannot be acquired.
            Result QueryAllInterfaces(const InterfaceFilter *filter, Interface *interfaces, size_t interfaces_size, s32 *out_count);
            Result QueryAcquiredInterfaces(Interface *interfaces, size_t interfaces_size, s32 *out_count);
            void *GetInterfaceStateChangeEvent();
        private:
            friend class InterfaceSession;
    };

    class InterfaceSession {
        NON_COPYABLE(InterfaceSession);
        NON_MOVEABLE(InterfaceSession);
        private:
            Client *m_client{nullptr};
            bool m_is_initialized{false};
            #if defined(ATMOSPHERE_OS_HORIZON)
                ::UsbHsClientIfSession m_session{};
            #else
                u8 m_session{};
            #endif
            bool m_event0_initialized{false};
            bool m_event_ctrl_xfer_initialized{false};
        public:
            InterfaceSession() = default;
            ~InterfaceSession();
        public:
            Result Initialize(Client *client, Interface *interface);
            Result Finalize();

            bool IsInitialized() const { return m_is_initialized; }
            s32 GetId() const;

            Result GetInterfaceInfo(InterfaceInfo *out);
            Result SelectInterface(InterfaceInfo *out, u8 alternate);
            Result ControlTransfer(u8 bm_request_type, u8 b_request, u16 w_value, u16 w_index, u16 w_length, void *buffer, u32 *out_transferred_size, u64 deadline_ns = 0);
            Result ResetDevice();

            /* Abort a control transfer that did not complete within the
             * bounded wait used by the mass-storage client. */
            void Abort();
        private:
            friend class EndpointSession;
    };

    class EndpointSession {
        NON_COPYABLE(EndpointSession);
        NON_MOVEABLE(EndpointSession);
        private:
            InterfaceSession *m_interface{nullptr};
            bool m_is_initialized{false};
            #if defined(ATMOSPHERE_OS_HORIZON)
                ::UsbHsClientEpSession m_session{};
            #else
                u8 m_session{};
            #endif
            bool m_event_xfer_initialized{false};
        public:
            EndpointSession() = default;
            ~EndpointSession();
        public:
            Result Initialize(InterfaceSession *interface, u16 max_urb_count, u32 max_transfer_size, EndpointDescriptor *descriptor);
            Result Finalize();

            /* Abort a transfer that did not complete within the bounded wait
             * used by the mass-storage client. The endpoint can be finalized
             * and reacquired by the storage worker on the next pass. */
            void Abort();

            bool IsInitialized() const { return m_is_initialized; }

            Result PostBuffer(void *buffer, u32 size, u32 *out_transferred_size, u64 deadline_ns = 0);
            u16 GetMaxPacketSize() const {
                #if defined(ATMOSPHERE_OS_HORIZON)
                    return m_is_initialized ? (m_session.desc.wMaxPacketSize & 0x7FF) : 0;
                #else
                    return 0;
                #endif
            }
        };

}
