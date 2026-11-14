/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fsmitm_external_key.hpp"
#include "../usb_mitm/usbmitm_module.hpp"
#include "../usb_mitm/usb_external_key_stats.hpp"

namespace ams::mitm::fs {
    namespace {
        constinit os::SdkMutex g_key_forward_mutex;

        void Observe(ncm::ProgramId client, u32 command, Result native, Result mirrored, bool accepted) {
            namespace stats = usb_storage::key_stats;
            stats::last_client.store(client.value);
            stats::native_result.store(native.GetValue());
            stats::mirror_result.store(mirrored.GetValue());
            if (command == 607) {
                stats::registrations.fetch_add(1);
                if (accepted) { stats::accepted.fetch_add(1); }
                if (accepted && R_SUCCEEDED(mirrored)) { stats::mirrored.fetch_add(1); }
            } else if (accepted) {
                stats::removals.fetch_add(1);
            }
            usb::content::LoaderTraceInfo trace{};
            trace.program_id = client.value;
            trace.stage = 202;
            trace.result = native.GetValue();
            util::SNPrintf(trace.path, sizeof(trace.path), "external-key c=%u native=%08X mirror=%08X accepted=%u",
                command, native.GetValue(), mirrored.GetValue(), accepted ? 1u : 0u);
            usb_storage::RecordLoaderTraceForDiagnostics(trace);
        }
    }

    Result ForwardExternalKeyRegistration(::Service *forward, ncm::ProgramId client,
            const ExternalKeyRegistration &request) {
        /* Serialize lifecycle order across sessions. No USB startup, NCM,
         * ES, filesystem IO or waits for keys may be called from this path. */
        std::scoped_lock lk(g_key_forward_mutex);
        const ExternalKeyRegistration input = request;
        static_assert(sizeof(input) == 0x20);
        R_RETURN(ForwardAcceptedKeyChange(
            [&]() -> Result { R_RETURN(serviceDispatchIn(forward, 607, input)); },
            [&]() -> Result { R_RETURN(usb_storage::CacheExternalKey(input.rights_id, input.access_key)); },
            [&](Result a, Result b, bool ok) { Observe(client, 607, a, b, ok); }));
    }

    Result ForwardExternalKeyRemoval(::Service *forward, ncm::ProgramId client, const ams::fs::RightsId &rights_id) {
        std::scoped_lock lk(g_key_forward_mutex);
        R_RETURN(ForwardAcceptedKeyChange(
            [&]() -> Result { R_RETURN(serviceDispatchIn(forward, 617, rights_id)); },
            [&]() -> Result { R_RETURN(usb_storage::RemoveExternalKey(rights_id)); },
            [&](Result a, Result b, bool ok) { Observe(client, 617, a, b, ok); }));
    }

    Result ForwardAllExternalKeyRemoval(::Service *forward, ncm::ProgramId client) {
        std::scoped_lock lk(g_key_forward_mutex);
        R_RETURN(ForwardAcceptedKeyChange(
            [&]() -> Result { R_RETURN(serviceDispatch(forward, 608)); },
            [&]() -> Result { R_RETURN(usb_storage::RemoveAllExternalKeys()); },
            [&](Result a, Result b, bool ok) { Observe(client, 608, a, b, ok); }));
    }
}
