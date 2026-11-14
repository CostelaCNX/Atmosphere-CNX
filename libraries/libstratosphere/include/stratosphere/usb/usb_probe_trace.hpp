/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ams::usb::probe_trace {
    // One startup writer. Every shared word atomic; monitor never waits on I/O.
    // Preserve the FIRST error per channel in the latest initializer attempt.
    // No buffers, credentials, strings or live read/write tracing.
    inline std::atomic<bool> active{};
    inline std::atomic<uint32_t> sequence{},generation{};
    struct Fields {std::atomic<uint32_t> phase{},result{},expected{},actual{},request{};};
    inline Fields control,bulk;
    inline void Clear(Fields &f) {f.phase=0;f.result=0;f.expected=0;f.actual=0;f.request=0;}
    inline void Begin() { active=false;sequence.fetch_add(1);++generation;Clear(control);Clear(bulk);sequence.fetch_add(1);active=true; }
    inline void End() { active=false; }
    inline void Observe(bool is_control,uint32_t phase,uint32_t result,uint32_t expected,uint32_t actual,uint32_t request) {
        if(!result || !active.load()) return;
        auto &f=is_control?control:bulk;
        if(f.phase.load()) return;
        sequence.fetch_add(1);
        f.phase=phase;f.result=result;f.expected=expected;f.actual=actual;f.request=request;
        sequence.fetch_add(1);
    }
    inline int Format(char *out,size_t n) {
        uint32_t c[5]{},b[5]{},g=0;bool coherent=false;
        for(unsigned attempt=0;attempt<3;++attempt) {
            const auto before=sequence.load(); if(before&1) continue;
            g=generation.load();
            c[0]=control.phase.load();c[1]=control.result.load();c[2]=control.expected.load();c[3]=control.actual.load();c[4]=control.request.load();
            b[0]=bulk.phase.load();b[1]=bulk.result.load();b[2]=bulk.expected.load();b[3]=bulk.actual.load();b[4]=bulk.request.load();
            if(before==sequence.load()) {coherent=true;break;}
        }
        if(!coherent) return std::snprintf(out,n,"usb_probe_trace=C13\nusb_probe_trace_coherent=0\n");
        return std::snprintf(out,n,
            "usb_probe_trace=C13\nusb_probe_trace_coherent=1\nusb_probe_trace_generation=%u\n"
            "usb_probe_trace_scope=first_errors_latest_initialize_not_presence\n"
            "usb_probe_trace_phases=0:none_1:submit_2:wait_3:report_IPC_4:report_validation_5:completion\n"
            "usb_probe_control=%u,0x%08X,%u,%u,0x%02X\nusb_probe_bulk=%u,0x%08X,%u,%u,0x%02X\n"
            "usb_probe_trace_fields=phase,result,expected,actual,request_or_endpoint\n",g,
            c[0],c[1],c[2],c[3],c[4],b[0],b[1],b[2],b[3],b[4]);
    }
}
