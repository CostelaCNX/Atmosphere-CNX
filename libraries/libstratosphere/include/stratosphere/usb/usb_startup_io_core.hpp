/* SPDX-License-Identifier: MIT */
#pragma once
#include "usb_startup_io_protocol.h"
#include "usb_startup_command_core.hpp"
#include <algorithm>
#include <cstring>
namespace nexus_startup {
/* Sidecar slots follow the base recorder's actual slot reuse, never sequence
 * modulo. Owner holds the same lock for admission, completion and snapshots.
 * No allocation or I/O. Base recorder pins every admitted call until EndCall. */
class IoRecorder {
    NexusStartupIoDetails slots[NEXUS_STARTUP_SLOT_COUNT]{};
    NexusStartupCommandDetails commands[NEXUS_STARTUP_SLOT_COUNT]{};
    static void Header(NexusStartupIoDetails &s) {
        s.magic=NEXUS_STARTUP_IO_MAGIC;s.version=NEXUS_STARTUP_IO_VERSION;
        s.size=sizeof(s);s.stage_count=NEXUS_STARTUP_IO_STAGES;
    }
    static void Header(NexusStartupCommandDetails &s) {
        s.magic=NEXUS_STARTUP_COMMAND_MAGIC;s.version=NEXUS_STARTUP_COMMAND_VERSION;s.size=sizeof(s);
    }
public:
    void Reset(uint32_t index,const NexusStartupSnapshot &base) {
        if(index>=NEXUS_STARTUP_SLOT_COUNT)return;
        auto &s=slots[index];s={};Header(s);s.sequence=base.sequence;
        s.started_ns=base.started_ns;s.deadline_ns=base.deadline_ns;
        s.supported=base.sequence && base.loader.source==NEXUS_STARTUP_SOURCE_USB;
        auto &c=commands[index];c={};Header(c);c.sequence=s.sequence;c.supported=s.supported;
    }
    bool Supported(uint64_t sequence) const {
        for(const auto &s:slots)if(s.sequence==sequence && sequence)return s.supported!=0;
        return false;
    }
    bool Snapshot(NexusStartupIoDetails *out,uint64_t sequence,uint64_t sampled) const {
        if(!out)return false;
        for(const auto &s:slots)if(s.sequence==sequence) {
            *out=s;Header(*out);out->sampled_ns=sampled;return true;
        }
        return false;
    }
    bool CommandSnapshot(NexusStartupCommandDetails *out,uint64_t sequence,uint64_t sampled) const {
        if(!out)return false;
        for(const auto &s:commands)if(s.sequence==sequence) {
            *out=s;Header(*out);out->sampled_ns=sampled;return true;
        }
        return false;
    }
    void Record(uint64_t sequence,uint64_t start,uint64_t end,uint32_t operation,
                const char *path,uint64_t offset,uint64_t requested,uint64_t completed,
                uint32_t result,const NexusStartupIoSample &sample,const NexusStartupCommandSample *command_sample=nullptr) {
        if(!sequence || operation>=NEXUS_STARTUP_OP_COUNT || end<start)return;
        for(auto &s:slots)if(s.sequence==sequence) {
            auto &command=commands[&s-slots];
            if(start<s.started_ns || start>=s.deadline_ns)return;
            const uint64_t elapsed=end-start;
            auto &op=s.operations[operation];++op.calls;op.total_ns+=elapsed;
            if(!s.supported)return;
            NexusStartupCommandSample checked_command{};
            if(command_sample) {
                if(ValidCommandSample(*command_sample,start,end)) {
                    checked_command=*command_sample;command.command_count+=checked_command.command_count;
                } else {++command.invalid_samples;checked_command.state=NexusStartupCommand_Invalid;}
            }
            uint64_t primary=0;bool observed=false;
            for(uint32_t i=0;i<NEXUS_STARTUP_IO_STAGES;++i) {
                const auto ns=sample.duration_ns[i];
                if((!sample.calls[i] && ns) || ns>elapsed ||
                   (i<NEXUS_STARTUP_IO_PRIMARY_STAGES && ns>elapsed-primary)) {
                    ++s.invalid_samples;return;
                }
                observed=observed || sample.calls[i]!=0;
                if(i<NEXUS_STARTUP_IO_PRIMARY_STAGES)primary+=ns;
            }
            if(!observed)return;
            ++op.instrumented_calls;
            for(uint32_t i=0;i<NEXUS_STARTUP_IO_STAGES;++i) {
                auto &phase=op.phases[i];phase.calls+=sample.calls[i];
                phase.total_ns+=sample.duration_ns[i];
                phase.max_ns=std::max(phase.max_ns,sample.duration_ns[i]);
            }
            if(elapsed<NEXUS_STARTUP_SLOW_NS)return;
            ++s.slow_event_total;
            const uint64_t relative=start-s.started_ns;
            uint32_t pos=0;
            while(pos<s.event_count && (s.events[pos].duration_ns>elapsed ||
                  (s.events[pos].duration_ns==elapsed && s.events[pos].relative_start_ns<=relative)))++pos;
            if(pos==NEXUS_STARTUP_IO_EVENTS)return;
            const uint32_t last=std::min<uint32_t>(s.event_count,NEXUS_STARTUP_IO_EVENTS-1);
            for(uint32_t i=last;i>pos;--i){s.events[i]=s.events[i-1];command.events[i]=command.events[i-1];}
            auto &event=s.events[pos];event={};event.relative_start_ns=relative;
            event.duration_ns=elapsed;event.offset=offset;event.requested_bytes=requested;
            event.completed_bytes=completed;event.operation=operation;event.result=result;
            event.sample=sample;
            if(path)std::memcpy(event.path,path,std::min(std::strlen(path),sizeof(event.path)-1));
            s.event_count=std::min<uint32_t>(s.event_count+1,NEXUS_STARTUP_IO_EVENTS);
            auto &ce=command.events[pos];ce={};ce.relative_start_ns=relative;
            ce.duration_ns=elapsed;ce.operation=operation;ce.sample=checked_command;
            command.event_count=s.event_count;
            return;
        }
    }
};
}
