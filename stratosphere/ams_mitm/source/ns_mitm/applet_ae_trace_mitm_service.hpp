/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#pragma once

#include <stratosphere.hpp>

/* This interface intentionally has no typed commands. Every appletAE request
 * is forwarded as its original raw HIPC message. Memory-only transport
 * progress also records requests still waiting for a native reply. The
 * post-reply observer recovers creator/accessor roles from
 * the ApplicationId-shaped command seen on firmware 22.5.0 without
 * reserializing qlaunch's PID/process handle/domain objects. A mapped USB
 * title may then queue short-lived stock ARP client work, but the native
 * appletAE response and Result are never changed. */
#define AMS_APPLET_AE_TRACE_MITM_INTERFACE_INFO(C, H)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::ns::impl, IAppletAeTraceMitmInterface, AMS_APPLET_AE_TRACE_MITM_INTERFACE_INFO, 0x9162A70C)

namespace ams::mitm::ns {

    constexpr size_t AppletAeMaxTrackedApplicationAccessors = 8;

    struct AppletAeApplicationAccessorTrace {
        u32 object_id;
        ncm::ProgramId application_id;
        bool ncm_trace_armed;
    };

    struct AppletAeForwardTraceState {
        u32 root_object_id;
        u32 system_applet_proxy_object_id;
        u32 application_creator_object_id;
        AppletAeApplicationAccessorTrace accessors[AppletAeMaxTrackedApplicationAccessors];
        size_t accessor_count;
        ncm::ProgramId active_trace_application_id;
        u32 active_trace_records;
    };

    class AppletAeTraceMitmService : public sf::MitmServiceImplBase {
        private:
            AppletAeForwardTraceState m_forward_trace{};
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                return client_info.program_id == ncm::SystemAppletId::Qlaunch;
            }

            AppletAeForwardTraceState *GetForwardTraceState() {
                return std::addressof(m_forward_trace);
            }
    };
    static_assert(impl::IsIAppletAeTraceMitmInterface<AppletAeTraceMitmService>);

    void ObserveAppletAeForwardRequest(const sf::cmif::PointerAndSize &request_message, const sf::cmif::PointerAndSize &response_message, void *context);
    void ObserveAppletAeForwardTransport(const sf::cmif::PointerAndSize &request_message, bool completed, Result transport_result, void *context);

}
