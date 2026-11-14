/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ams::usb::reader_fixed_sense {
    constexpr uint32_t FixedBytes = 18;
    // Only the existing serialized USB startup worker uses this scope. Bind
    // the selection to this Device AND acquired interface, never to the last
    // diagnostic VID/PID. No Device layout, host ABI or endpoint change.
    // RAII revokes it on every readiness exit, BEFORE capacity or file I/O.
    inline const void *owner{};
    inline uint32_t owner_interface{};
    class Scope {
        bool acquired{};
        public:
            Scope(const void *device, uint32_t interface_id, bool eligible_startup) {
                if (eligible_startup && device && !owner) {
                    owner = device; owner_interface = interface_id; acquired = true;
                }
            }
            ~Scope() { if (acquired) { owner = nullptr; owner_interface = 0; } }
            Scope(const Scope &) = delete;
            Scope &operator=(const Scope &) = delete;
    };
    inline uint32_t RequestSize(const void *device, uint32_t interface_id,
                                bool bot, uint64_t blocks, uint32_t normal) {
        return device && owner == device && owner_interface == interface_id && bot && blocks == 0
            ? FixedBytes : normal;
    }
    inline uint32_t AllocatedCurrentSense(const uint8_t *p, uint32_t received,
                                         uint32_t requested, uint32_t strict_result) {
        // A full18-byte allocation can intentionally truncate optional sense
        // data. Consume only the mandatory current key/ASC/ASCQ, never extra
        // descriptors. SHORT/incomplete transfers still use the strict parser.
        // Caller first validates data/CSW/tag/residue and successful status.
        if (strict_result || !p || requested != FixedBytes || received != FixedBytes || p[7] <= FixedBytes - 8)
            return strict_result;
        if ((p[0] & 0x7f) == 0x70)
            return 0x01000000u | ((p[2] & 0xf) << 16) | (p[12] << 8) | p[13];
        if ((p[0] & 0x7f) == 0x72)
            return 0x01000000u | ((p[1] & 0xf) << 16) | (p[2] << 8) | p[3];
        return 0;
    }
    // Monitor readers only access this bounded atomic diagnostic snapshot.
    // A completed request is NOT proof of readiness, capacity or a mount.
    enum Field : unsigned { Interface, Lun, Requested, Received, Result, Sense, TimeMs, Complete, Count };
    inline std::atomic<uint32_t> sequence{}, attempts{}, first[Count]{}, latest[Count]{};
    inline void Begin(uint32_t interface_id, uint32_t lun, uint32_t requested, uint32_t time_ms) {
        const uint32_t row[Count] = {interface_id,lun,requested,0,UINT32_MAX,0,time_ms,0};
        ++sequence; const auto n=attempts.load();
        for(unsigned i=0;i<Count;++i) { latest[i]=row[i]; if(n==0)first[i]=row[i]; }
        attempts=n+1; ++sequence;
    }
    inline void Finish(uint32_t result, uint32_t received, uint32_t sense) {
        ++sequence;
        const Field keys[]={Result,Received,Sense,Complete};
        const uint32_t values[]={result,received,sense,1};
        for(unsigned i=0;i<4;++i) { latest[keys[i]]=values[i]; if(attempts.load()==1)first[keys[i]]=values[i]; }
        ++sequence;
    }
    inline int Format(char *out,size_t size) {
        uint32_t rows[2][Count]{},n=0;bool coherent=false;
        for(unsigned retry=0;retry<3;++retry) {
            const auto before=sequence.load();if(before&1)continue;n=attempts.load();
            for(unsigned i=0;i<Count;++i){rows[0][i]=first[i];rows[1][i]=latest[i];}
            if(before==sequence.load()){coherent=true;break;}
        }
        int used=std::snprintf(out,size,
            "usb_fixed_sense=C21\nusb_fixed_sense_profile=validated-bot-startup-sense18\n"
            "usb_fixed_sense_identity=see_usb_compatibility_selection_and_interface_rows\n"
            "usb_fixed_sense_coherent=%u\nusb_fixed_sense_attempts=%u\n"
            "usb_fixed_sense_fields=interface,lun,requested,received,result,parsed_current_sense,time_ms,complete\n",coherent?1u:0u,n);
        if(!coherent || used<0 || static_cast<size_t>(used)>=size)return used;
        for(unsigned bank=0;bank<2;++bank) {
            const auto *v=rows[bank];
            const int wrote=std::snprintf(out+used,size-used,
                "usb_fixed_sense_%s=%08X,%u,%u,%u,%08X,%08X,%u,%u\n",
                bank==0?"first":"latest",v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);
            if(wrote<0)return wrote;
            used+=wrote;
            if(static_cast<size_t>(used)>=size)return used;
        }
        return used;
    }
}
