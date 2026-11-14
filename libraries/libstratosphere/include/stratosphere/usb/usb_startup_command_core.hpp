/* SPDX-License-Identifier: MIT */
#pragma once
#include "usb_startup_command_protocol.h"
#include <initializer_list>
namespace nexus_startup {
inline bool ValidCommandSample(const NexusStartupCommandSample &s,uint64_t start,uint64_t end) {
    if(s.reserved || end<start)return false;
    if(s.state==NexusStartupCommand_None)return s.command_count==0;
    if(s.state!=NexusStartupCommand_Captured || !s.command_count)return false;
    const auto &x=s.slowest;const auto &c=x.command;
    if(c.start_ns<start || c.start_ns>end || c.duration_ns>end-c.start_ns || c.sequence || c.reserved ||
       c.opcode>255 || c.source>NEXUS_USB_COMPLETION_SOURCE_SYNC ||
       (x.filesystem_stage>=NEXUS_STARTUP_IO_PRIMARY_STAGES && x.filesystem_stage!=NEXUS_STARTUP_COMMAND_UNKNOWN) ||
       x.max_wait_segment>=NEXUS_STARTUP_COMMAND_SEGMENTS || (x.max_wait_flags&~3u) ||
       ((x.max_wait_flags&NEXUS_STARTUP_COMMAND_LBA_VALID) && !(x.max_wait_flags&NEXUS_STARTUP_COMMAND_ATTEMPT_VALID)))return false;
    uint64_t host=0,segments=0;
    for(auto ns:{c.submit_ns,c.wait_ns,c.report_ns}) {
        if(ns>c.duration_ns-host)return false;
        host+=ns;
    }
    if((!c.host_calls && c.submit_ns) || (!c.wait_calls && c.wait_ns) || (!c.report_calls && c.report_ns))return false;
    for(uint32_t i=0;i<NEXUS_STARTUP_COMMAND_SEGMENTS;++i) {
        if((!x.segment_calls[i] && x.segment_ns[i]) || x.segment_ns[i]>c.duration_ns-segments)return false;
        segments+=x.segment_ns[i];
    }
    if(c.max_wait_ns>c.wait_ns)return false;
    if(c.wait_calls) {
        if(c.max_wait_start_ns<c.start_ns || c.max_wait_start_ns>c.start_ns+c.duration_ns ||
           c.max_wait_ns>c.start_ns+c.duration_ns-c.max_wait_start_ns)return false;
        if(x.max_wait_segment && (!x.segment_calls[x.max_wait_segment] || c.max_wait_ns>x.segment_ns[x.max_wait_segment]))return false;
    } else if(c.max_wait_ns || x.max_wait_flags || x.max_wait_segment)return false;
    if(x.max_wait_flags&NEXUS_STARTUP_COMMAND_ATTEMPT_VALID) {
        if(!c.wait_calls || x.max_wait_opcode>255 || x.max_wait_transport<1 || x.max_wait_transport>2 || x.max_wait_lun>255)return false;
    }
    return true;
}
}
