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
#include <stratosphere.hpp>
#include "../usb_mitm/usb_hang_capture.hpp"
#include "../amsmitm_initialization.hpp"
#include "../usb_mitm/usbmitm_module.hpp"
#include "../usb_mitm/usb_launch_trace.hpp"
#include "nsmitm_module.hpp"
#include "ns_am2_trace_mitm_service.hpp"
#include "ns_am_mitm_service.hpp"
#include "ns_web_mitm_service.hpp"
#include "ncm_launch_trace_client.hpp"

namespace ams::mitm::ns {

    namespace {

        enum PortIndex {
            PortIndex_NsAm,
            PortIndex_NsAm2,
            PortIndex_NsWeb,
            PortIndex_Count,
        };

        constexpr sm::ServiceName NsAmMitmServiceName = sm::ServiceName::Encode("ns:am");
        constexpr sm::ServiceName NsAm2MitmServiceName = sm::ServiceName::Encode("ns:am2");
        constexpr sm::ServiceName NsWebMitmServiceName = sm::ServiceName::Encode("ns:web");

        constexpr size_t MaxSessions = 8;

        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = sf::hipc::DefaultServerManagerOptions::PointerBufferSize;
            /* Preserve the existing bounded NS domain pool. C23 does not
             * install an appletAE server or consume this pool for applets. */
            static constexpr size_t MaxDomains          = 2;
            static constexpr size_t MaxDomainObjects    = 16;
            static constexpr bool CanDeferInvokeRequest = sf::hipc::DefaultServerManagerOptions::CanDeferInvokeRequest;
            static constexpr bool CanManageMitmServers  = true;
        };

        class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ServerOptions, MaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ServerManager g_server_manager;

        Result ServerManager::OnNeedsToAccept(int port_index, Server *server) {
            /* Acknowledge the mitm session. */
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));

            switch (port_index) {
                case PortIndex_NsAm:
                    R_RETURN(this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<impl::IAmMitmInterface, NsAmMitmService>(decltype(fsrv)(fsrv), client_info), fsrv));
                case PortIndex_NsAm2:
                    {
                        auto service_object = sf::CreateSharedObjectEmplaced<impl::IAm2GetterMitmInterface, NsAm2MitmService>(decltype(fsrv)(fsrv), client_info);
                        R_RETURN(this->AcceptMitmImpl(server, std::move(service_object), fsrv));
                    }
                case PortIndex_NsWeb:
                    R_RETURN(this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<impl::IWebMitmInterface, NsWebMitmService>(decltype(fsrv)(fsrv), client_info), fsrv));
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

    }

    void MitmModule::ThreadFunction(void *) {
        usb_storage::hang_capture::RegisterCurrentThread(usb_storage::hang_capture::Ns);
        /* Wait until initialization is complete. */
        mitm::WaitInitialized();

        /* Keep appletAE entirely native. Only content/control-data routes
         * are installed; no raw launch observer or background IPC probe. */

        if (hos::GetVersion() < hos::Version_3_0_0) {
            R_ABORT_UNLESS((g_server_manager.RegisterMitmServer<NsAmMitmService>(PortIndex_NsAm, NsAmMitmServiceName)));
        } else {
            /* Modern firmware exposes IApplicationManagerInterface through
             * command 7996 on ns:am2. Registration is intentionally non-fatal:
             * a conflicting service override must not turn an optional USB
             * route into a boot failure. */
            const Result ns_am2_result = g_server_manager.RegisterMitmServer<NsAm2MitmService>(PortIndex_NsAm2, NsAm2MitmServiceName);
            AMS_UNUSED(ns_am2_result);
            R_ABORT_UNLESS((g_server_manager.RegisterMitmServer<NsWebMitmService>(PortIndex_NsWeb, NsWebMitmServiceName)));
        }

        /* Loop forever, servicing our services. */
        g_server_manager.LoopProcess();
    }

}
