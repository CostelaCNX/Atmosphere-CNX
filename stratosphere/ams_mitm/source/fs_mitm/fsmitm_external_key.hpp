/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::fs {
    /* One fixed command-607 wire record. Independent parameters would let
     * SF sort packed AccessKey before aligned RightsId and swap the halves.
     * Native forwarding must preserve every byte of the incoming record. */
    struct ExternalKeyRegistration {
        ams::fs::RightsId rights_id;
        spl::AccessKey access_key;
    };
    static_assert(sizeof(ExternalKeyRegistration) == 0x20);
    static_assert(AMS_OFFSETOF(ExternalKeyRegistration, rights_id) == 0x00);
    static_assert(AMS_OFFSETOF(ExternalKeyRegistration, access_key) == 0x10);
    using ExternalKeyRegistrationMeta = sf::impl::CommandMetaInfo<const ExternalKeyRegistration &>;
    static_assert(ExternalKeyRegistrationMeta::InDataOffsets[0] == 0);
    static_assert(ExternalKeyRegistrationMeta::InDataSize == 0x20);
    static_assert(ExternalKeyRegistrationMeta::NumBuffers == 0);

    Result ForwardExternalKeyRegistration(::Service *forward, ncm::ProgramId client,
        const ExternalKeyRegistration &request);
    Result ForwardExternalKeyRemoval(::Service *forward, ncm::ProgramId client,
        const ams::fs::RightsId &rights_id);
    Result ForwardAllExternalKeyRemoval(::Service *forward, ncm::ProgramId client);

    /* Native validation is authoritative. Never feed a rejected key to USB,
     * and never change native success into failure if the optional mirror fails. */
    template<typename Forward, typename Mirror, typename Observe>
    Result ForwardAcceptedKeyChange(Forward forward, Mirror mirror, Observe observe) {
        const Result native = forward();
        const bool accepted = R_SUCCEEDED(native);
        const Result mirrored = accepted ? mirror() : native;
        observe(native, mirrored, accepted);
        R_RETURN(native);
    }
}

#define AMS_FS_EXTERNAL_KEY_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 607, Result, RegisterExternalKey, (const ams::mitm::fs::ExternalKeyRegistration &request), (request), hos::Version_2_0_0) \
    AMS_SF_METHOD_INFO(C, H, 608, Result, UnregisterAllExternalKey, (), (), hos::Version_2_0_0) \
    AMS_SF_METHOD_INFO(C, H, 617, Result, UnregisterExternalKey, (const ams::fs::RightsId &rights_id), (rights_id), hos::Version_7_0_0)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::fs, IFsExternalKeyMitmInterface, AMS_FS_EXTERNAL_KEY_INTERFACE_INFO, 0x7DF34ED2)

namespace ams::mitm::fs {
    /* ES owns an early fsp-srv session. Intercept only key lifecycle commands,
     * never its filesystem/save/ticket operations or raw IPC payloads. */
    class FsExternalKeyMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            Result RegisterExternalKey(const ExternalKeyRegistration &request) {
                R_RETURN(ForwardExternalKeyRegistration(m_forward_service.get(), m_client_info.program_id, request));
            }
            Result UnregisterAllExternalKey() {
                R_RETURN(ForwardAllExternalKeyRemoval(m_forward_service.get(), m_client_info.program_id));
            }
            Result UnregisterExternalKey(const ams::fs::RightsId &rights_id) {
                R_RETURN(ForwardExternalKeyRemoval(m_forward_service.get(), m_client_info.program_id, rights_id));
            }
    };
}
