/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "usb_write_transfer_protocol.h"
namespace ams::usb::write_transfer {
enum class Outcome { Success, InvalidParameter, Busy, Denied, MediaChanged };
/* Externally serialized; no I/O, allocation, or scheduling. */
class Lease {
    uint64_t owner{},token{},generation{},deadline{},next_token{};
    uint64_t acquisitions{},expirations{},restores{},cancellations{};
    uint32_t bytes{NEXUS_USB_WRITE_TRANSFER_DEFAULT};
    static void Count(uint64_t &n) { if(n!=UINT64_MAX)++n; }
    void Clear(){owner=token=generation=deadline=0;bytes=NEXUS_USB_WRITE_TRANSFER_DEFAULT;}
    void Observe(uint64_t now,uint64_t current_generation){
        if(!owner)return;
        if(generation!=current_generation){Count(cancellations);Clear();}
        else if(now>=deadline){Count(expirations);Clear();}
    }
public:
    static bool ValidRequest(const NexusUsbWriteTransferRequest &r){
        if(r.reserved)return false;
        if(r.operation==NEXUS_USB_WRITE_TRANSFER_ACQUIRE)
            return r.generation&&!r.token&&r.duration_ms&&r.duration_ms<=NEXUS_USB_WRITE_TRANSFER_MAX_MS&&
                (r.transfer_bytes==NEXUS_USB_WRITE_TRANSFER_SMALL||r.transfer_bytes==NEXUS_USB_WRITE_TRANSFER_DEFAULT);
        return !r.transfer_bytes&&!r.duration_ms&&
            ((r.operation==NEXUS_USB_WRITE_TRANSFER_QUERY&&!r.token)||r.operation==NEXUS_USB_WRITE_TRANSFER_RESTORE);
    }
    bool Active()const{return owner!=0;}
    void Invalidate(){if(owner){Count(cancellations);Clear();}}
    uint32_t Limit(uint64_t now){Observe(now,generation);return bytes;}
    Outcome Control(uint64_t now,uint64_t current_generation,uint64_t caller,
                    const NexusUsbWriteTransferRequest &r,NexusUsbWriteTransferStatus *out){
        if(!out||!caller||!ValidRequest(r))return Outcome::InvalidParameter;
        Observe(now,current_generation);
        if(r.operation==NEXUS_USB_WRITE_TRANSFER_ACQUIRE){
            if(r.generation!=current_generation)return Outcome::MediaChanged;
            if(owner)return Outcome::Busy;
            const uint64_t duration=uint64_t(r.duration_ms)*1000000;
            if(next_token==UINT64_MAX||duration>UINT64_MAX-now)return Outcome::InvalidParameter;
            owner=caller;token=++next_token;generation=current_generation;deadline=now+duration;
            bytes=r.transfer_bytes;Count(acquisitions);
        }else if(r.operation==NEXUS_USB_WRITE_TRANSFER_RESTORE&&owner){
            if(caller!=owner||r.token!=token)return Outcome::Denied;
            if(r.generation!=generation)return Outcome::MediaChanged;
            Count(restores);Clear();
        }
        *out={NEXUS_USB_WRITE_TRANSFER_MAGIC,NEXUS_USB_WRITE_TRANSFER_VERSION,sizeof(*out),
            owner?NEXUS_USB_WRITE_TRANSFER_ACTIVE|(caller==owner?NEXUS_USB_WRITE_TRANSFER_OWNED:0u):0u,
            now,current_generation,owner,token,deadline,bytes,NEXUS_USB_WRITE_TRANSFER_DEFAULT,
            acquisitions,expirations,restores,cancellations};
        return Outcome::Success;
    }
};
}
