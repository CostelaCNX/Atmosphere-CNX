/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ams::usb::startup_history {
    // Single initializer writer; bounded atomic snapshots. First window never
    // rolls out. No USB commands, filesystem calls, payloads or per-game logs.
    constexpr unsigned FirstCapacity=64, Capacity=16, Words=22;
    inline std::atomic<bool> active{};
    inline std::atomic<uint32_t> sequence{}, total{}, rows[Capacity][Words]{}, first[FirstCapacity][Words]{};
    inline uint32_t generation{},bus{},device{},identity{};
    inline void Begin(uint32_t g,uint32_t b,uint32_t d,uint32_t vid,uint32_t pid) {
        active=false; generation=g;bus=b;device=d;identity=(vid<<16)|pid;active=true;
    }
    inline void End(){active=false;}
    inline uint32_t Micros(uint64_t ns) { return ns/1000>UINT32_MAX ? UINT32_MAX : uint32_t(ns/1000); }
    inline void Record(uint32_t kind,uint32_t phase,uint32_t result,uint32_t expected,uint32_t actual,
        uint32_t request,uint32_t elapsed_ms,uint32_t reported=0,uint32_t xfer=0,uint32_t submitted=0,
        uint64_t user_id=0,bool report_present=false,uint32_t request_type=0,uint32_t value=0,
        uint32_t index=0,uint32_t elapsed_us=0,uint32_t reports=0) {
        if(!active.load())return;
        // Successful submit calls are omitted; completions and failures remain.
        if(!result && phase<5)return;
        const uint32_t values[Words]={generation,bus,device,identity,kind,phase,result,expected,actual,
            request,elapsed_ms,reported,xfer,submitted,uint32_t(user_id>>32),uint32_t(user_id),report_present?1u:0u,
            request_type,value,index,elapsed_us,reports};
        ++sequence;auto n=total.load();
        for(unsigned i=0;i<Words;++i) {
            rows[n%Capacity][i]=values[i];
            if(n<FirstCapacity)first[n][i]=values[i];
        }
        total=n+1;++sequence;
    }
    inline int Format(char *out,size_t size) {
        // About7KiB bounded stack snapshot. Caller text buffer is static and
        // mutex-protected, not allocated on the monitor's32KiB stack.
        uint32_t initial[FirstCapacity][Words]{},latest[Capacity][Words]{},n=0;bool coherent=false;
        for(unsigned attempt=0;attempt<3;++attempt){
            auto before=sequence.load();if(before&1)continue;
            n=total.load();const auto count=n<Capacity?n:Capacity;
            for(unsigned r=0;r<count;++r)for(unsigned c=0;c<Words;++c)latest[r][c]=rows[(n-count+r)%Capacity][c].load();
            const auto first_count=n<FirstCapacity?n:FirstCapacity;
            for(unsigned r=0;r<first_count;++r)for(unsigned c=0;c<Words;++c)initial[r][c]=first[r][c].load();
            if(before==sequence.load()){coherent=true;break;}
        }
        if(!coherent)return std::snprintf(out,size,"usb_startup_history=C17\nusb_startup_history_coherent=0\n");
        int used=std::snprintf(out,size,
            "usb_startup_history=C17\nusb_startup_history_coherent=1\nusb_startup_history_total=%u\n"
            "usb_startup_history_scope=first64_retained_plus_last16_not_raw_xHCI_not_presence\n"
            "usb_startup_history_first_count=%u\nusb_startup_history_omitted_middle=%u\n"
            "usb_startup_history_fields=gen,bus,dev,VIDPID,kind,phase,result,expected,actual,request_or_ep,ms,report_requested,xfer,submitted,id_hi,id_lo,report_present,bmRequestType,wValue,wIndex,us,report_count\n"
            "usb_startup_history_kinds=1:control_2:bulk_3:endpoint_4:lifecycle\n"
            "usb_startup_history_phases=1:submit_2:wait_3:report_IPC_4:validation_5:completion_10:open_11:populate_12:event_20:begin_21:end_22:reset_RPC_23:skip_MAX_LUN\n"
            "usb_startup_history_end_fields=result0:initialized_elseFFFFFFFF_expected:ready_actual:capacity_request:direct_trial\n",n,
            n<FirstCapacity?n:FirstCapacity,n>FirstCapacity+Capacity?n-FirstCapacity-Capacity:0);
        if(used<0 || static_cast<size_t>(used)>=size)return used;
        for(unsigned bank=0;bank<2;++bank) {
            const unsigned cap=bank==0?FirstCapacity:Capacity,count=n<cap?n:cap;
            for(unsigned r=0;r<count;++r) {
                const auto *v=bank==0?initial[r]:latest[r];
                const int wrote=std::snprintf(out+used,size-used,
                    "usb_startup_%s_%02u=%u,%u,%u,%08X,%u,%u,%08X,%u,%u,%02X,%u,%u,%u,%u,%08X,%08X,%u,%02X,%04X,%04X,%u,%u\n",
                    bank==0?"first":"event",r,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],v[18],v[19],v[20],v[21]);
                if(wrote<0)return wrote;
                used+=wrote;if(static_cast<size_t>(used)>=size)return used;
            }
        }
        return used;
    }
}
