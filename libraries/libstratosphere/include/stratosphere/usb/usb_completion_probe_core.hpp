/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "usb_completion_probe_protocol.h"
#include "usb_write_transfer_core.hpp"
namespace ams::usb::completion_probe {
using Outcome = write_transfer::Outcome;
enum class Counter { SliceTimeout, EventRace, Query, Empty, Ready, Error };
/* Reuse the tested owner/token/generation lease; the private limit values
 * encode modes only and never change the separate disk-write policy. */
class Lease {
    write_transfer::Lease lease;
    uint64_t counters[6]{};
public:
    static bool ValidRequest(const NexusUsbCompletionProbeRequest &r) {
        return !r.reserved && (r.operation==NEXUS_USB_COMPLETION_PROBE_ACQUIRE ?
            r.generation && !r.token && r.mode<=NEXUS_USB_COMPLETION_PROBE_ASSISTED &&
            r.duration_ms && r.duration_ms<=NEXUS_USB_COMPLETION_PROBE_MAX_MS :
            !r.mode && !r.duration_ms && ((r.operation==NEXUS_USB_COMPLETION_PROBE_QUERY && !r.token) ||
                r.operation==NEXUS_USB_COMPLETION_PROBE_RESTORE));
    }
    bool Active()const{return lease.Active();}
    void Invalidate(){lease.Invalidate();}
    uint32_t Mode(uint64_t now){return lease.Limit(now)==NEXUS_USB_WRITE_TRANSFER_SMALL ? 1u : 0u;}
    void Count(Counter counter){auto &n=counters[static_cast<unsigned>(counter)];if(n!=UINT64_MAX)++n;}
    Outcome Control(uint64_t now,uint64_t generation,uint64_t caller,
                    const NexusUsbCompletionProbeRequest &r,NexusUsbCompletionProbeStatus *out){
        if(!out||!ValidRequest(r))return Outcome::InvalidParameter;
        const NexusUsbWriteTransferRequest request{r.generation,r.token,r.operation,
            r.operation==NEXUS_USB_COMPLETION_PROBE_ACQUIRE ?
                (r.mode ? NEXUS_USB_WRITE_TRANSFER_SMALL : NEXUS_USB_WRITE_TRANSFER_DEFAULT) : 0u,
            r.duration_ms,r.reserved};
        NexusUsbWriteTransferStatus status{};
        const auto result=lease.Control(now,generation,caller,request,&status);
        if(result!=Outcome::Success)return result;
        *out={NEXUS_USB_COMPLETION_PROBE_MAGIC,NEXUS_USB_COMPLETION_PROBE_VERSION,sizeof(*out),status.flags,
            status.sampled_ns,status.media_generation,status.owner_pid,status.token,status.deadline_ns,
            status.transfer_bytes==NEXUS_USB_WRITE_TRANSFER_SMALL ? 1u : 0u,0,
            status.acquisitions,status.expirations,status.restores,status.cancellations,
            counters[0],counters[1],counters[2],counters[3],counters[4],counters[5]};
        return result;
    }
};
static_assert(NEXUS_USB_COMPLETION_PROBE_MAX_MS==NEXUS_USB_WRITE_TRANSFER_MAX_MS);
}
