/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ams::usb::startup_sense_csw {
    // C18: single startup-worker writer. Retain the first recovery as well as
    // the latest, so a later probe cannot erase the original failure. No data
    // payloads, filesystem operations, dynamic allocation or extra USB polling.
    enum Field : unsigned {
        Stage, Vid, Pid, Interface, Lun, Tag, Endpoint, Requested, Received,
        Original, Clear, Retry, StatusBytes, Final, FinalStage, Residue, Status,
        TimeMs, Count
    };
    inline std::atomic<uint32_t> sequence{}, attempts{}, first[Count]{}, latest[Count]{};
    inline void Begin(uint32_t vid, uint32_t pid, uint32_t interface_id, uint32_t lun,
                      uint32_t tag, uint32_t ep, uint32_t requested, uint32_t received,
                      uint32_t original, uint32_t time_ms) {
        const uint32_t row[Count] = {1,vid,pid,interface_id,lun,tag,ep,requested,received,
            original,UINT32_MAX,UINT32_MAX,0,UINT32_MAX,0,UINT32_MAX,UINT32_MAX,time_ms};
        ++sequence; const auto n=attempts.load();
        for(unsigned i=0;i<Count;++i) { latest[i]=row[i]; if(n==0)first[i]=row[i]; }
        attempts=n+1; ++sequence;
    }
    inline void Set(Field field, uint32_t value) {
        ++sequence; latest[field]=value; if(attempts.load()==1)first[field]=value; ++sequence;
    }
    inline void Finish(uint32_t result,uint32_t phase,uint32_t bytes,uint32_t residue,uint32_t status) {
        ++sequence;
        const Field keys[]={Final,FinalStage,StatusBytes,Residue,Status};
        const uint32_t values[]={result,phase,bytes,residue,status};
        for(unsigned i=0;i<5;++i) { latest[keys[i]]=values[i]; if(attempts.load()==1)first[keys[i]]=values[i]; }
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
            "usb_sense_csw=C18\nusb_sense_csw_coherent=%u\nusb_sense_csw_attempts=%u\n"
            "usb_sense_csw_policy=startup_only_short_or_full_sense_completed_STALL_same_status_once_no_data_replay\n"
            "usb_sense_csw_fields=stage,vid,pid,interface,lun,tag,ep,requested,received,original,clear,retry,status_bytes,final,final_phase,residue,status,time_ms\n"
            "usb_sense_csw_stages=1:clear_pending_2:clear_failed_3:retry_pending_4:retry_returned_5:deadline_or_session\n",
            coherent?1u:0u,n);
        if(!coherent || used<0 || static_cast<size_t>(used)>=size)return used;
        for(unsigned bank=0;bank<2;++bank) {
            const auto *v=rows[bank];
            const int wrote=std::snprintf(out+used,size-used,
                "usb_sense_csw_%s=%u,%04X,%04X,%08X,%u,%u,%02X,%u,%u,%08X,%08X,%08X,%u,%08X,%u,%u,%u,%u\n",
                bank==0?"first":"latest",v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],
                v[9],v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17]);
            if(wrote<0)return wrote;
            used+=wrote;
            if(static_cast<size_t>(used)>=size)return used;
        }
        return used;
    }
}
