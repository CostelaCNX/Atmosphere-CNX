/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ams::usb::reader_startup_halt {
    // Single startup writer; monitor reads only atomic words. No live I/O.
    enum { Attempts, Bus, Device, Stage, ResultCode, InStatus, OutStatus, Cleared, Count };
    inline std::atomic<uint32_t> sequence{}, fields[Count]{};
    inline uint32_t seen_bus[2]{}, seen_device[2]{};
    inline unsigned seen_count = 0;
    inline void Set(unsigned field, uint32_t value) { ++sequence; fields[field]=value; ++sequence; }
    inline bool Claim(bool eligible, uint32_t bus, uint32_t device) {
        if (!eligible || seen_count == 2) return false;
        for (unsigned i=0;i<seen_count;++i) if(seen_bus[i]==bus && seen_device[i]==device) return false;
        seen_bus[seen_count]=bus; seen_device[seen_count++]=device;
        ++sequence;
        fields[Attempts]=seen_count; fields[Bus]=bus; fields[Device]=device;
        fields[Stage]=1; fields[ResultCode]=0; fields[InStatus]=UINT32_MAX; fields[OutStatus]=UINT32_MAX; fields[Cleared]=0;
        ++sequence;
        return true;
    }
    // Caller supplies an aligned, private two-byte scratch area and a shared
    // three-second deadline. Return the original IPC error; never infer HALT
    // from a Result number. No reset, SCSI command, sector access or replay.
    template<class Transfer, class Live, class Failure>
    auto Run(Transfer transfer, Live live, Failure invalid, uint8_t *buffer, uint8_t in, uint8_t out) {
        auto finish=[&](auto rc) { Set(ResultCode,rc.GetValue()); return rc; };
        auto success=invalid(); // Initialized without requiring an AMS header.
        for (unsigned i=0;i<2;++i) {
            const uint8_t endpoint=i==0?in:out;
            const unsigned status_field=i==0?InStatus:OutStatus;
            Set(Stage,2+i*3);
            if(!live()) return finish(invalid());
            buffer[0]=buffer[1]=0; uint32_t bytes=0;
            auto rc=transfer(0x82,0,0,endpoint,2,buffer,&bytes);
            if(rc.GetValue()) return finish(rc);
            const uint32_t status=buffer[0] | (uint32_t(buffer[1])<<8);
            Set(status_field,status);
            if(bytes!=2 || status>1) return finish(invalid());
            success=rc;
            if(!status) continue;
            Set(Stage,3+i*3);
            if(!live()) return finish(invalid());
            bytes=0;rc=transfer(0x02,1,0,endpoint,0,nullptr,&bytes);
            if(rc.GetValue()) return finish(rc);
            if(bytes) return finish(invalid());
            Set(Stage,4+i*3);
            if(!live()) return finish(invalid());
            buffer[0]=buffer[1]=0;bytes=0;
            rc=transfer(0x82,0,0,endpoint,2,buffer,&bytes);
            if(rc.GetValue()) return finish(rc);
            if(bytes!=2 || buffer[0]!=0 || buffer[1]!=0) return finish(invalid());
            Set(Cleared,fields[Cleared].load() | (1u<<i));
        }
        Set(Stage,8); return finish(success);
    }
    inline int Format(char *out, size_t size) {
        uint32_t f[Count]{}; bool coherent=false;
        for(unsigned attempt=0;attempt<3;++attempt) {
            auto before=sequence.load();if(before&1)continue;
            for(unsigned i=0;i<Count;++i) f[i]=fields[i].load();
            if(before==sequence.load()){coherent=true;break;}
        }
        if(!coherent) return std::snprintf(out,size,"usb_reader_halt=C16\nusb_reader_halt_coherent=0\n");
        return std::snprintf(out,size,
            "usb_reader_halt=C16\nusb_reader_halt_coherent=1\n"
            "usb_reader_halt_scope=exact_reader_completed_maxlun_error_before_TUR_max2_per_boot\n"
            "usb_reader_halt_attempts=%u\nusb_reader_halt_bus_device=%u,%u\n"
            "usb_reader_halt_stage=%u\nusb_reader_halt_result=0x%08X\n"
            "usb_reader_halt_initial_in_out=0x%08X,0x%08X\nusb_reader_halt_verified_clear_mask=%u\n"
            "usb_reader_halt_stages=1:claimed_2:IN_query_3:IN_clear_4:IN_verify_5:OUT_query_6:OUT_clear_7:OUT_verify_8:checked_not_mount\n",
            f[Attempts],f[Bus],f[Device],f[Stage],f[ResultCode],f[InStatus],f[OutStatus],f[Cleared]);
    }
}
