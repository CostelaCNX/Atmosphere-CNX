/*
 * Copyright (c) Atmosphere-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::arp {

    struct ApplicationLaunchProperty {
        ncm::ProgramId application_id;
        u32 version;
        u8 storage_id;
        u8 patch_storage_id;
        u8 application_kind;
        u8 reserved;
    };
    static_assert(util::is_pod<ApplicationLaunchProperty>::value);
    static_assert(sizeof(ApplicationLaunchProperty) == 0x10);

    struct ApplicationControlProperty : public sf::LargeData, public sf::PrefersMapAliasTransferMode {
        u8 data[0x4000];
    };
    static_assert(util::is_pod<ApplicationControlProperty>::value);
    static_assert(sizeof(ApplicationControlProperty) == 0x4000);

    Result ReadUsbApplicationControlProperty(ApplicationControlProperty *out, ncm::ProgramId application_id);

}

#define AMS_ARP_READER_MITM_INTERFACE_INFO(C, H)                                                                                                      \
    AMS_SF_METHOD_INFO(C, H, 1, Result, GetApplicationControlProperty, (sf::Out<ams::mitm::arp::ApplicationControlProperty> out, u64 instance_id),     \
                       (out, instance_id), hos::Version_10_0_0)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::arp::impl, IArpReaderMitmInterface, AMS_ARP_READER_MITM_INTERFACE_INFO, 0xA19D3E52)

namespace ams::mitm::arp {

    class ArpReaderMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;

            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                /* The failing asynchronous launch lookup is issued by ns.
                 * Keep every other arp:r client entirely outside this shim. */
                return client_info.program_id == ncm::SystemProgramId::Ns;
            }

            Result GetApplicationControlProperty(sf::Out<ApplicationControlProperty> out, u64 instance_id);
    };
    static_assert(impl::IsIArpReaderMitmInterface<ArpReaderMitmService>);

}
