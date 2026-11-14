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
#include <stratosphere.hpp>
#include "ns_am2_trace_mitm_service.hpp"
#include "ns_verification_trace.hpp"
#include "ns_shim.h"

#define AMS_NS_AM_MITM_INTERFACE_INFO(C, H)                                                                                                                                                                             \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetApplicationViewDeprecated,  (const sf::OutBuffer &out_views, const sf::InBuffer &application_ids),             (out_views, application_ids))                             \
    AMS_SF_METHOD_INFO(C, H, 21, Result, GetApplicationContentPath,      (const sf::OutBuffer &out_path, ncm::ProgramId application_id, u8 content_type), (out_path, application_id, content_type))                     \
    AMS_SF_METHOD_INFO(C, H, 23, Result, ResolveApplicationContentPath,  (ncm::ProgramId application_id, u8 content_type),                                (application_id, content_type))                               \
    AMS_SF_METHOD_INFO(C, H, 38, Result, CheckApplicationLaunchVersion,  (ncm::ProgramId application_id),                                                  (application_id))                                             \
    AMS_SF_METHOD_INFO(C, H, 92, Result, GetRunningApplicationProgramId, (sf::Out<ncm::ProgramId> out, ncm::ProgramId application_id),                    (out, application_id),                    hos::Version_6_0_0) \
    AMS_SF_METHOD_INFO(C, H, 1701, Result, GetApplicationView,            (const sf::OutBuffer &out_views, const sf::InBuffer &application_ids),             (out_views, application_ids),             hos::Version_3_0_0) \
    AMS_SF_METHOD_INFO(C, H, 1704, Result, GetApplicationViewWithPromotionInfo, (const sf::OutBuffer &out_views, const sf::InBuffer &application_ids),       (out_views, application_ids),             hos::Version_8_0_0) \
    AMS_NS_VERIFY_REQUEST_INFO(C, H)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::ns::impl, IAmMitmInterface, AMS_NS_AM_MITM_INTERFACE_INFO, 0x059D2C39)

namespace ams::mitm::ns { struct BadgeControlReply { u32 flags[2]; u32 size; }; }
static_assert(sizeof(ams::mitm::ns::BadgeControlReply)==12);
#define AMS_NS_CONTROL_DATA_TRACE_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetBadgeControlData, (u8 source, ncm::ProgramId id, const sf::OutBuffer &buffer, sf::Out<u32> size), (source, id, buffer, size)) \
    AMS_SF_METHOD_INFO(C, H, 6, Result, GetBadgeControlData2, (u8 source, u8 flag, u8 index, ncm::ProgramId id, const sf::OutBuffer &buffer, sf::Out<u64> reply), (source, flag, index, id, buffer, reply), hos::Version_19_0_0) \
    AMS_SF_METHOD_INFO(C, H, 19, Result, GetBadgeControlData3, (u8 source, u8 flag, ncm::ProgramId id, const sf::OutBuffer &buffer, sf::Out<ams::mitm::ns::BadgeControlReply> reply), (source, flag, id, buffer, reply), hos::Version_21_0_0)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::ns::impl, IControlDataTraceMitmInterface, AMS_NS_CONTROL_DATA_TRACE_MITM_INTERFACE_INFO, 0xA41F0C39)

#define AMS_NS_AM2_GETTER_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 7989, Result, GetReadOnlyApplicationControlDataInterface, (sf::Out<sf::SharedPointer<mitm::ns::impl::IControlDataTraceMitmInterface>> out), (out), hos::Version_5_1_0) \
    AMS_SF_METHOD_INFO(C, H, 7996, Result, GetApplicationManagerInterface, (sf::Out<sf::SharedPointer<mitm::ns::impl::IAmMitmInterface>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::ns::impl, IAm2GetterMitmInterface, AMS_NS_AM2_GETTER_MITM_INTERFACE_INFO, 0x24C37A19)

namespace ams::mitm::ns {

    /* B1 forwards native control-data commands before optionally replacing
     * only their JPEG, for qlaunch. Unknown commands still raw-forward. */
    class NsControlDataTraceMitmService : public sf::MitmServiceImplBase {
        private:
            NsAm2ForwardTraceState m_forward_trace{};
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            Result GetBadgeControlData(u8 source, ncm::ProgramId id, const sf::OutBuffer &buffer, sf::Out<u32> size);
            Result GetBadgeControlData2(u8 source, u8 flag, u8 index, ncm::ProgramId id, const sf::OutBuffer &buffer, sf::Out<u64> reply);
            Result GetBadgeControlData3(u8 source, u8 flag, ncm::ProgramId id, const sf::OutBuffer &buffer, sf::Out<BadgeControlReply> reply);
        public:
            NsAm2ForwardTraceState *GetForwardTraceState() {
                m_forward_trace.client_program_id = m_client_info.program_id;
                m_forward_trace.application_manager_child = false;
                m_forward_trace.read_only_control_data_child = true;
                return std::addressof(m_forward_trace);
            }
    };
    static_assert(impl::IsIControlDataTraceMitmInterface<NsControlDataTraceMitmService>);

    class NsAmMitmService : public sf::MitmServiceImplBase {
        private:
            NsAm2ForwardTraceState m_forward_trace{};
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                /* We will mitm:
                 * - web applets, to facilitate hbl web browser launching.
                 * - ns:am clients while the USB-content experiment is active.
                 *   The handlers below forward every non-USB result unchanged;
                 *   the broad session acceptance is needed because 22.x does
                 *   not expose one stable Home Menu caller id here.
                 */
                AMS_UNUSED(client_info);
                return true;
            }
        public:
            /* Actual command API. */
            Result GetApplicationViewDeprecated(const sf::OutBuffer &out_views, const sf::InBuffer &application_ids);
            Result GetApplicationContentPath(const sf::OutBuffer &out_path, ncm::ProgramId application_id, u8 content_type);
            Result ResolveApplicationContentPath(ncm::ProgramId application_id, u8 content_type);
            Result CheckApplicationLaunchVersion(ncm::ProgramId application_id);
            Result GetRunningApplicationProgramId(sf::Out<ncm::ProgramId> out, ncm::ProgramId application_id);
            Result GetApplicationView(const sf::OutBuffer &out_views, const sf::InBuffer &application_ids);
            Result GetApplicationViewWithPromotionInfo(const sf::OutBuffer &out_views, const sf::InBuffer &application_ids);
            Result RequestVerifyApplication(sf::OutCopyHandle event,
                sf::Out<sf::SharedPointer<impl::IVerifyResultMitm>> out,
                sf::CopyHandle &&memory, const VerifyApplicationRequest &request) {
                R_RETURN(ForwardVerifyApplication(m_forward_service, m_client_info,
                    event, out, std::move(memory), request));
            }

            NsAm2ForwardTraceState *GetForwardTraceState() {
                m_forward_trace.client_program_id = m_client_info.program_id;
                m_forward_trace.application_manager_child = true;
                m_forward_trace.read_only_control_data_child = false;
                return std::addressof(m_forward_trace);
            }
    };
    static_assert(impl::IsIAmMitmInterface<NsAmMitmService>);

    class NsAm2MitmService : public sf::MitmServiceImplBase {
        private:
            NsAm2ForwardTraceState m_forward_trace{};
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                /* C10: AM's diagnostic-only observer must not share HOME's
                 * single synchronous NS dispatch loop. Raw forwarding still
                 * adds that dependency even when it leaves replies unchanged.
                 * Let AM connect to native NS directly; keep HOME's USB icon,
                 * control-data and content-path handling unchanged. */
                return client_info.program_id == ncm::SystemAppletId::Qlaunch;
            }
        public:
            Result GetReadOnlyApplicationControlDataInterface(sf::Out<sf::SharedPointer<impl::IControlDataTraceMitmInterface>> out);
            Result GetApplicationManagerInterface(sf::Out<sf::SharedPointer<impl::IAmMitmInterface>> out);

            NsAm2ForwardTraceState *GetForwardTraceState() {
                m_forward_trace.client_program_id = m_client_info.program_id;
                m_forward_trace.application_manager_child = true;
                m_forward_trace.read_only_control_data_child = false;
                return std::addressof(m_forward_trace);
            }
    };
    static_assert(impl::IsIAm2GetterMitmInterface<NsAm2MitmService>);

}
