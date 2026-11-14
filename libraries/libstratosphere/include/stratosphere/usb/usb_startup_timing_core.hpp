/* SPDX-License-Identifier: MIT */
#pragma once
#include "usb_startup_timing_protocol.h"
#include <cstring>
#include <algorithm>
namespace nexus_startup {
/* The owner serializes this fixed-size, allocation-free recorder. No I/O is
 * performed here. Sequences prevent old filesystem handles contaminating a
 * subsequent launch in the same process. */
class Recorder {
    NexusStartupSnapshot slots[NEXUS_STARTUP_SLOT_COUNT]{};
    uint64_t inflight[NEXUS_STARTUP_SLOT_COUNT]{};
    bool saved[NEXUS_STARTUP_SLOT_COUNT]{};
    uint64_t next_sequence{1};
    uint64_t offered_sequence{};
public:
    static bool Valid(const NexusStartupLoader &v) {
        if(v.magic!=NEXUS_STARTUP_MAGIC || v.version!=NEXUS_STARTUP_VERSION ||
           (v.source!=NEXUS_STARTUP_SOURCE_SD && v.source!=NEXUS_STARTUP_SOURCE_USB) ||
           v.reserved || (v.flags&~7u) || v.path[0]!='/' ||
           !std::memchr(v.path,0,sizeof(v.path)))return false;
        if((v.flags&NEXUS_STARTUP_HASH_MATCHED) && !(v.flags&NEXUS_STARTUP_HASH_PERFORMED))return false;
        if((v.flags&NEXUS_STARTUP_HANDOFF) && v.result)return false;
        if((v.flags&NEXUS_STARTUP_HANDOFF) && v.source==NEXUS_STARTUP_SOURCE_USB &&
           (v.flags&6u)!=6u)return false;
        return true;
    }
    uint64_t Submit(uint64_t pid,uint64_t now,const NexusStartupLoader &loader) {
        if(!pid || !Valid(loader) || !next_sequence || now>UINT64_MAX-NEXUS_STARTUP_WINDOW_NS)return 0;
        NexusStartupSnapshot *target=nullptr;
        uint32_t target_index=0;
        for(uint32_t i=0;i<NEXUS_STARTUP_SLOT_COUNT;++i) {
            auto &s=slots[i];
            const bool reusable=!s.sequence || (saved[i] && !inflight[i] &&
                (s.state!=NexusStartup_Capturing || now>=s.deadline_ns));
            if(reusable && (!target || s.sequence<target->sequence)){target=&s;target_index=i;}
        }
        if(!target)return 0;
        // A full unsaved ring must not retire a capture when submission fails.
        Supersede(pid,now);
        inflight[target_index]=0;saved[target_index]=false;
        *target={};target->magic=NEXUS_STARTUP_MAGIC;target->version=NEXUS_STARTUP_VERSION;
        target->sequence=next_sequence++;target->pid=pid;target->started_ns=now;
        target->deadline_ns=now+NEXUS_STARTUP_WINDOW_NS;target->loader=loader;
        target->state=(loader.flags&NEXUS_STARTUP_HANDOFF)?NexusStartup_Capturing:NexusStartup_LoaderFailed;
        return target->sequence;
    }
    void Supersede(uint64_t pid,uint64_t now) {
        for(auto &s:slots)if(s.pid==pid && s.state==NexusStartup_Capturing && now<s.deadline_ns)
            s.state=NexusStartup_Superseded;
    }
    uint64_t Active(uint64_t pid,uint32_t source,uint64_t now) const {
        for(const auto &s:slots)if(s.pid==pid && s.loader.source==source &&
            s.state==NexusStartup_Capturing && now>=s.started_ns && now<s.deadline_ns)return s.sequence;
        return 0;
    }
    bool Observing(uint64_t sequence,uint64_t now) const {
        for(const auto &s:slots)if(s.sequence==sequence)return s.state==NexusStartup_Capturing &&
            now>=s.started_ns && now<s.deadline_ns;
        return false;
    }
    // Admission and its timestamp are taken together under the owner's lock.
    // A call admitted before the deadline must finish before its final report
    // can be selected, even when it completes after expiry or supersession.
    bool BeginCall(uint64_t sequence,uint64_t now) {
        if(!sequence)return false;
        for(uint32_t i=0;i<NEXUS_STARTUP_SLOT_COUNT;++i)if(slots[i].sequence==sequence) {
            if(saved[i] || inflight[i]==UINT64_MAX || !Observing(sequence,now))return false;
            ++inflight[i];return true;
        }
        return false;
    }
    void EndCall(uint64_t sequence,uint64_t start,uint64_t end,uint32_t operation,
                 const char *path,uint64_t offset,uint64_t requested,uint64_t completed,uint32_t result) {
        if(!sequence)return;
        for(uint32_t i=0;i<NEXUS_STARTUP_SLOT_COUNT;++i)if(slots[i].sequence==sequence) {
            if(!inflight[i])return;
            RecordImpl(i,start,end,operation,path,offset,requested,completed,result,true);
            --inflight[i];return;
        }
    }
    void Record(uint64_t sequence,uint64_t start,uint64_t end,uint32_t operation,
                const char *path,uint64_t offset,uint64_t requested,uint64_t completed,uint32_t result) {
        if(!sequence)return;
        for(uint32_t i=0;i<NEXUS_STARTUP_SLOT_COUNT;++i)if(slots[i].sequence==sequence) {
            RecordImpl(i,start,end,operation,path,offset,requested,completed,result,false);return;
        }
    }
    bool HasActiveOrInflight(uint64_t now) const {
        for(uint32_t i=0;i<NEXUS_STARTUP_SLOT_COUNT;++i) {
            const auto &s=slots[i];
            if(inflight[i] || (s.sequence && s.state==NexusStartup_Capturing && now<s.deadline_ns))return true;
        }
        return false;
    }
    // Selection does not consume a record. The publisher retries it after any
    // write/flush/rename failure, and acknowledges only a successful publish.
    // The owner also excludes new Submit calls throughout publication; the
    // recorder lock itself is released before doing any filesystem work.
    bool NextUnsaved(NexusStartupSnapshot *out,uint64_t now) {
        if(!out || HasActiveOrInflight(now))return false;
        uint32_t target=NEXUS_STARTUP_SLOT_COUNT;
        for(uint32_t i=0;i<NEXUS_STARTUP_SLOT_COUNT;++i) {
            const auto &s=slots[i];
            if(s.sequence && !saved[i] && (target==NEXUS_STARTUP_SLOT_COUNT || s.sequence<slots[target].sequence))target=i;
        }
        if(target==NEXUS_STARTUP_SLOT_COUNT)return false;
        Snapshot(out,target,now);offered_sequence=out->sequence;return true;
    }
    bool MarkSaved(uint64_t sequence) {
        if(!sequence || sequence!=offered_sequence)return false;
        for(uint32_t i=0;i<NEXUS_STARTUP_SLOT_COUNT;++i)if(slots[i].sequence==sequence) {
            if(inflight[i] || saved[i])return false;
            saved[i]=true;offered_sequence=0;return true;
        }
        return false;
    }
private:
    void RecordImpl(uint32_t index,uint64_t start,uint64_t end,uint32_t operation,
                    const char *path,uint64_t offset,uint64_t requested,uint64_t completed,uint32_t result,bool admitted) {
        if(operation>=NEXUS_STARTUP_OP_COUNT || end<start)return;
        auto &s=slots[index];
        if(saved[index] || (s.state!=NexusStartup_Capturing && !(admitted && s.state==NexusStartup_Superseded)) ||
           start<s.started_ns || start>=s.deadline_ns)return;
            const uint64_t duration=end-start, relative=start-s.started_ns;
            auto &o=s.operations[operation];++o.calls;o.requested_bytes+=requested;
            o.completed_bytes+=completed;o.total_ns+=duration;o.max_ns=std::max(o.max_ns,duration);
            if(result)++o.errors;
            auto &b=s.buckets[relative/NEXUS_STARTUP_BUCKET_NS];++b.calls;b.io_ns+=duration;
            if(operation==NexusStartup_Read) {
                b.read_bytes+=completed;
                ++s.read_size_calls[requested<=4096?0:requested<=65536?1:requested<=1048576?2:3];
            }
            if(duration>=NEXUS_STARTUP_SLOW_NS) {
                auto &e=s.events[s.event_total%NEXUS_STARTUP_EVENT_COUNT];e={};
                e.relative_start_ns=relative;e.duration_ns=duration;e.offset=offset;
                e.requested_bytes=requested;e.completed_bytes=completed;e.operation=operation;e.result=result;
                if(path) {const size_t n=std::min(std::strlen(path),sizeof(e.path)-1);std::memcpy(e.path,path,n);}
                ++s.event_total;s.event_count=static_cast<uint32_t>(std::min<uint64_t>(s.event_total,NEXUS_STARTUP_EVENT_COUNT));
            }
    }
public:
    bool Snapshot(NexusStartupSnapshot *out,uint32_t index,uint64_t now) const {
        if(!out || index>=NEXUS_STARTUP_SLOT_COUNT)return false;
        const auto &s=slots[index];*out=s;out->magic=NEXUS_STARTUP_MAGIC;
        out->version=NEXUS_STARTUP_VERSION;out->sampled_ns=now;
        if(out->state==NexusStartup_Capturing && now>=out->deadline_ns)out->state=NexusStartup_Expired;
        const uint64_t first=s.event_total-s.event_count;
        for(uint32_t i=0;i<s.event_count;++i)out->events[i]=s.events[(first+i)%NEXUS_STARTUP_EVENT_COUNT];
        return true;
    }
};
}
