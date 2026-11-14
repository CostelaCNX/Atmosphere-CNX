/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "usb_host_write_protocol.h"
#include "usb_write_transfer_core.hpp"
#include <stddef.h>
namespace ams::usb::host_write {
using Outcome = write_transfer::Outcome;
constexpr uint64_t DataTimeoutNs=15'000'000'000ULL;
struct Plan {
    uint32_t bytes, chunk_bytes;
    uint64_t deadline_ns;
    bool eligible, segmented;
};
inline Plan MakePlan(uint32_t bytes,uintptr_t address,uint32_t max_packet,bool modern,
                     uint32_t mode,uint64_t now,uint64_t outer_deadline) {
    const bool eligible=bytes && !(address&4095) && modern && max_packet &&
        max_packet<=1024 && !(max_packet&(max_packet-1)) &&
        NEXUS_USB_HOST_WRITE_SEGMENT_BYTES%max_packet==0;
    const bool segmented=eligible && mode==NEXUS_USB_HOST_WRITE_SEGMENTED &&
        bytes>NEXUS_USB_HOST_WRITE_SEGMENT_BYTES;
    const uint64_t own_deadline=now>UINT64_MAX-DataTimeoutNs ? UINT64_MAX : now+DataTimeoutNs;
    return {bytes,segmented ? NEXUS_USB_HOST_WRITE_SEGMENT_BYTES : bytes,
        outer_deadline && outer_deadline<own_deadline ? outer_deadline : own_deadline,
        eligible,segmented};
}
struct DataResult {uint32_t result,transferred,submissions;bool shortened;};
/* The production DATA-OUT loop is portable for fault injection. post receives
 * (buffer,size,out_transferred,absolute_deadline) and returns a native Result
 * value. An error/short ends the loop permanently; callers never replay it. */
template<typename Post,typename Now>
DataResult Transfer(const Plan &plan,void *buffer,Post post,Now now,
                    uint32_t timed_out,uint32_t invalid_length) {
    DataResult out{};
    if(!plan.eligible || !buffer || !plan.bytes || !plan.chunk_bytes ||
       plan.chunk_bytes>plan.bytes || reinterpret_cast<uintptr_t>(buffer)>UINTPTR_MAX-plan.bytes){
        out.result=invalid_length;return out;
    }
    while(out.transferred<plan.bytes) {
        if(now()>=plan.deadline_ns){out.result=timed_out;return out;}
        const uint32_t left=plan.bytes-out.transferred;
        const uint32_t amount=left<plan.chunk_bytes ? left : plan.chunk_bytes;
        uint32_t actual=0;
        ++out.submissions;
        out.result=post(static_cast<uint8_t *>(buffer)+out.transferred,amount,&actual,plan.deadline_ns);
        if(out.result)return out;
        if(actual>amount){out.result=invalid_length;return out;}
        out.transferred+=actual;
        if(actual<amount){out.shortened=true;return out;}
    }
    return out;
}
class Lease {
    write_transfer::Lease lease;
    uint64_t counters[7]{};
    static void Add(uint64_t &value,uint64_t amount){value=amount>UINT64_MAX-value?UINT64_MAX:value+amount;}
public:
    static bool ValidRequest(const NexusUsbHostWriteRequest &r) {
        return !r.reserved && (r.operation==NEXUS_USB_HOST_WRITE_ACQUIRE ?
            r.generation && !r.token && r.mode<=NEXUS_USB_HOST_WRITE_SEGMENTED &&
            r.duration_ms && r.duration_ms<=NEXUS_USB_HOST_WRITE_MAX_MS :
            !r.mode && !r.duration_ms && ((r.operation==NEXUS_USB_HOST_WRITE_QUERY && !r.token) ||
                r.operation==NEXUS_USB_HOST_WRITE_RESTORE));
    }
    bool Active()const{return lease.Active();}
    void Invalidate(){lease.Invalidate();}
    uint32_t Mode(uint64_t now){return lease.Limit(now)==NEXUS_USB_WRITE_TRANSFER_SMALL ? 1u : 0u;}
    void Record(const Plan &plan,const DataResult &result){
        if(!plan.eligible)return;
        Add(counters[0],1);Add(counters[1],plan.segmented?1:0);
        Add(counters[2],result.submissions);Add(counters[3],plan.bytes);
        Add(counters[4],result.transferred);Add(counters[5],result.shortened?1:0);
        Add(counters[6],result.result?1:0);
    }
    Outcome Control(uint64_t now,uint64_t generation,uint64_t caller,
                    const NexusUsbHostWriteRequest &r,NexusUsbHostWriteStatus *out){
        if(!out||!ValidRequest(r))return Outcome::InvalidParameter;
        const NexusUsbWriteTransferRequest request{r.generation,r.token,r.operation,
            r.operation==NEXUS_USB_HOST_WRITE_ACQUIRE ?
                (r.mode ? NEXUS_USB_WRITE_TRANSFER_SMALL : NEXUS_USB_WRITE_TRANSFER_DEFAULT) : 0u,
            r.duration_ms,r.reserved};
        NexusUsbWriteTransferStatus status{};
        const auto result=lease.Control(now,generation,caller,request,&status);
        if(result!=Outcome::Success)return result;
        *out={NEXUS_USB_HOST_WRITE_MAGIC,NEXUS_USB_HOST_WRITE_VERSION,sizeof(*out),status.flags,
            status.sampled_ns,status.media_generation,status.owner_pid,status.token,status.deadline_ns,
            status.transfer_bytes==NEXUS_USB_WRITE_TRANSFER_SMALL ? 1u : 0u,0,
            NEXUS_USB_HOST_WRITE_SEGMENT_BYTES,0,
            status.acquisitions,status.expirations,status.restores,status.cancellations,
            counters[0],counters[1],counters[2],counters[3],counters[4],counters[5],counters[6]};
        return result;
    }
};
static_assert(NEXUS_USB_HOST_WRITE_MAX_MS==NEXUS_USB_WRITE_TRANSFER_MAX_MS);
}
