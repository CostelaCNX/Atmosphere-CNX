/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>
#include "../../../libraries/libstratosphere/source/ncm/ncm_lookup_trace.hpp"
#include "../../../libraries/libstratosphere/source/ncm/ncm_usb_limits.hpp"

// Retain every existing command, implementation and server/session limit.
// These two private commands are used only by the read-only recovery app.
#define AMS_NCM_RECOVERY_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 65000, Result, BeginUsbLookupTrace, (sf::Out<u64> out_session, u64 title_id), (out_session, title_id)) \
    AMS_SF_METHOD_INFO(C, H, 65001, Result, EndUsbLookupTrace, (const sf::OutBuffer &out, u64 session), (out, session)) \
    AMS_SF_METHOD_INFO(C, H, 65033, Result, GetUsbInstallCapabilities, (const sf::OutBuffer &out), (out))

AMS_SF_DEFINE_INTERFACE_WITH_BASE(ams::ncm::recovery, IRecoveryContentManager,
    ::ams::ncm::IContentManager, AMS_NCM_RECOVERY_INTERFACE_INFO, 0xFDB4FFE2);

namespace ams::ncm::recovery {
    class ContentManager : public ncm::ContentManagerImpl {
      public:
        Result GetUsbInstallCapabilities(const sf::OutBuffer &out) {
            // Read-only, no mounts, placeholder writes or registration effects.
            // Bit 1: explicit per-handle USB installs and native-safe metadata
            // ownership. Queryable even while no USB drive is connected.
            const u32 caps[]{usb_limits::Magic,usb_limits::Abi,usb_limits::Titles,usb_limits::Content,3,0};
            R_UNLESS(out.GetSize()==sizeof(caps),fs::ResultInvalidArgument());
            std::memcpy(out.GetPointer(),caps,sizeof(caps));R_SUCCEED();
        }
        Result BeginUsbLookupTrace(sf::Out<u64> out_session, u64 title_id) {
            R_UNLESS(title_id != 0, fs::ResultInvalidArgument());
            out_session.SetValue(lookup_trace::Begin(title_id));
            R_SUCCEED();
        }
        Result EndUsbLookupTrace(const sf::OutBuffer &out, u64 session) {
            R_RETURN(lookup_trace::End(out.GetPointer(), out.GetSize(), session));
        }
    };
    static_assert(IsIRecoveryContentManager<ContentManager>);
}
