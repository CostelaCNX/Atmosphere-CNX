/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#pragma once

#include <stratosphere.hpp>

namespace ams::mitm::ns {

    /* Firmware 19.0.0 expanded nn::ns::ApplicationLaunchInfo to 0x80 bytes.
     * Keep the wire layout opaque apart from the stable prefix used to scope
     * the USB fallback. Command 97 consumes the complete structure inline. */
    struct ApplicationLaunchInfoV19 {
        u64 application_id;
        u32 version;
        u32 launch_flags;
        u8 application_storage_id;
        u8 update_storage_id;
        u8 reserved[0x6E];
    };
    static_assert(util::is_pod<ApplicationLaunchInfoV19>::value);
    static_assert(sizeof(ApplicationLaunchInfoV19) == 0x80);

}

/* AM must see the native IApplicationManagerInterface wire protocol
 * unchanged. Command 97 is decoded for diagnostics but remains a strict
 * stock pass-through. Every other child command remains on the raw forward
 * path. */
#define AMS_NS_AM_CHILD_TRACE_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 97, Result, GetMainApplicationProgramIndexByApplicationLaunchInfo, (sf::Out<u8> out, const mitm::ns::ApplicationLaunchInfoV19 &launch_info), (out, launch_info), hos::Version_19_0_0)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::ns::impl, INsAmChildTraceMitmInterface, AMS_NS_AM_CHILD_TRACE_MITM_INTERFACE_INFO, 0x9117736D)

/* Only command 7996 is decoded on AM's ns:am2 root session. Decoding the
 * getter is necessary to attach the observer to the returned non-domain
 * child session. Every other root command remains raw-forwarded. */
#define AMS_NS_AM2_TRACE_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 7996, Result, GetApplicationManagerInterface, (sf::Out<sf::SharedPointer<mitm::ns::impl::INsAmChildTraceMitmInterface>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::ns::impl, INsAm2TraceMitmInterface, AMS_NS_AM2_TRACE_MITM_INTERFACE_INFO, 0xFE8A4231)

namespace ams::mitm::ns {

    struct NsAm2ForwardTraceState {
        ncm::ProgramId client_program_id;
        bool application_manager_child;
        bool read_only_control_data_child;
        u64 pending_call{};
        u32 pending_command{};
        u32 pending_object{};
    };

    class NsAmChildTraceMitmService : public sf::MitmServiceImplBase {
        private:
            NsAm2ForwardTraceState m_forward_trace{};
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            NsAm2ForwardTraceState *GetForwardTraceState() {
                m_forward_trace.client_program_id = m_client_info.program_id;
                m_forward_trace.application_manager_child = true;
                m_forward_trace.read_only_control_data_child = false;
                return std::addressof(m_forward_trace);
            }

            Result GetMainApplicationProgramIndexByApplicationLaunchInfo(sf::Out<u8> out, const ApplicationLaunchInfoV19 &launch_info);
    };
    static_assert(impl::IsINsAmChildTraceMitmInterface<NsAmChildTraceMitmService>);

    class NsAm2TraceMitmService : public sf::MitmServiceImplBase {
        private:
            NsAm2ForwardTraceState m_forward_trace{};
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                return client_info.program_id == ncm::SystemProgramId::Am;
            }

            NsAm2ForwardTraceState *GetForwardTraceState() {
                m_forward_trace.client_program_id = m_client_info.program_id;
                m_forward_trace.application_manager_child = false;
                m_forward_trace.read_only_control_data_child = false;
                return std::addressof(m_forward_trace);
            }

            Result GetApplicationManagerInterface(sf::Out<sf::SharedPointer<impl::INsAmChildTraceMitmInterface>> out);
    };
    static_assert(impl::IsINsAm2TraceMitmInterface<NsAm2TraceMitmService>);

    void ObserveNsAm2ForwardRequest(const sf::cmif::PointerAndSize &request_message, const sf::cmif::PointerAndSize &response_message, void *context);
    void ObserveNsAm2ForwardTransport(const sf::cmif::PointerAndSize &request_message, bool completed, Result result, void *context);

}
