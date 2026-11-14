/* SPDX-License-Identifier: MIT */
#pragma once
#include "usb_startup_timing_protocol.h"
#include "usb_startup_io_protocol.h"
#include "usb_startup_command_protocol.h"
#include "usb_write_cache_protocol.h"
#include <cstddef>
#include <cstring>
namespace nexus_startup {
/* Bounded, allocation-free JSON encoder for a single completed capture.
 * Compatible with the benchmark's startup_timings schema. Invalid UTF-8 is
 * replaced in display paths; path_bytes_hex retains the exact original bytes. */
class ReportWriter {
    char *out;
    size_t capacity, used{};
    bool good{true};
    void Byte(char c) { if(used+1<capacity)out[used++]=c;else good=false; }
    void Text(const char *s) { while(*s)Byte(*s++); }
    void Number(uint64_t n) { char digits[20];size_t count=0;do{digits[count++]=char('0'+n%10);n/=10;}while(n);while(count)Byte(digits[--count]); }
    void HexByte(unsigned char c) { constexpr char hex[]="0123456789abcdef";Byte(hex[c>>4]);Byte(hex[c&15]); }
    void Hex(const void *value,size_t size) { Byte('"');const auto *p=static_cast<const unsigned char*>(value);while(size--)HexByte(*p++);Byte('"'); }
    size_t Length(const char *p,size_t bound) { const auto *end=static_cast<const char*>(std::memchr(p,0,bound));if(!end){good=false;return bound;}return size_t(end-p); }
    void String(const char *p,size_t bound) {
        const size_t length=Length(p,bound);Byte('"');
        for(size_t i=0;i<length;) {
            const auto c=static_cast<unsigned char>(p[i]);
            if(c<0x80) { ++i;if(c=='"'||c=='\\'){Byte('\\');Byte(char(c));}else if(c<0x20){Text("\\u00");HexByte(c);}else Byte(char(c));continue; }
            const size_t n=c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
            bool valid=n && n<=length-i;
            for(size_t j=1;valid&&j<n;++j)valid=(static_cast<unsigned char>(p[i+j])&0xc0)==0x80;
            if(valid) { const auto d=static_cast<unsigned char>(p[i+1]);valid=!(c==0xe0&&d<0xa0)&&!(c==0xed&&d>=0xa0)&&!(c==0xf0&&d<0x90)&&!(c==0xf4&&d>=0x90); }
            if(valid){for(size_t j=0;j<n;++j)Byte(p[i++]);}else{Text("\\ufffd");++i;}
        }
        Byte('"');
    }
    void Path(const char *p,size_t bound) { Text("\"path\":");String(p,bound);Text(",\"path_bytes_hex\":");Hex(p,Length(p,bound)); }
    void Field(const char *name,uint64_t value) { Byte('"');Text(name);Text("\":");Number(value); }
    void NullableField(const char *name,bool known,uint64_t value) {
        Byte('"');Text(name);Text("\":");if(known)Number(value);else Text("null");
    }
    bool CommandSample(const NexusStartupCommandSample &sample) {
        static constexpr const char *states[]={"none","captured","invalid"};
        static constexpr const char *segments[]={"unknown","bot_command","bot_data","bot_status","uas_command","uas_data","uas_status"};
        static constexpr const char *phases[]={"lock_wait","metadata","prepare","data_write","sync"};
        if(sample.state>NexusStartupCommand_Invalid)return false;
        Byte('{');Field("command_count",sample.command_count);Text(",\"state\":\"");Text(states[sample.state]);Text("\",\"slowest\":");
        if(sample.state!=NexusStartupCommand_Captured){Text("null}");return true;}
        const auto &x=sample.slowest;const auto &c=x.command;
        if(x.max_wait_segment>=NEXUS_STARTUP_COMMAND_SEGMENTS || x.max_wait_transport>2)return false;
        Byte('{');Field("start_ns",c.start_ns);Byte(',');Field("duration_ns",c.duration_ns);Byte(',');
        Field("opcode",c.opcode);Byte(',');Field("source",c.source);Byte(',');Field("requested_bytes",c.bytes);Byte(',');Field("result",c.result);Byte(',');
        NullableField("lba",c.flags&NEXUS_USB_COMPLETION_LBA_VALID,c.lba);Byte(',');NullableField("idle_ns",c.flags&NEXUS_USB_COMPLETION_IDLE_VALID,c.idle_ns);
        Text(",\"filesystem_stage\":\"");Text(x.filesystem_stage<NEXUS_STARTUP_IO_PRIMARY_STAGES?phases[x.filesystem_stage]:"unknown");Text("\",");
        Field("submit_ns",c.submit_ns);Byte(',');Field("wait_ns",c.wait_ns);Byte(',');Field("report_ns",c.report_ns);Byte(',');
        Field("host_calls",c.host_calls);Byte(',');Field("wait_calls",c.wait_calls);Byte(',');Field("report_calls",c.report_calls);
        Text(",\"segments\":{");
        for(size_t i=0;i<NEXUS_STARTUP_COMMAND_SEGMENTS;++i) {
            if(i){Byte(',');}
            Byte('"');Text(segments[i]);Text("\":{");Field("calls",x.segment_calls[i]);Byte(',');Field("total_ns",x.segment_ns[i]);Byte('}');
        }
        Text("},\"max_wait\":{");Field("start_ns",c.max_wait_start_ns);Byte(',');Field("duration_ns",c.max_wait_ns);Byte(',');
        Field("bytes",c.max_wait_bytes);Byte(',');Field("endpoint",c.max_wait_endpoint);Byte(',');Field("xfer_id",c.max_wait_xfer_id);Byte(',');Field("result",c.max_wait_result);
        Text(",\"segment\":\"");Text(segments[x.max_wait_segment]);Text("\",");
        const bool attempt=x.max_wait_flags&NEXUS_STARTUP_COMMAND_ATTEMPT_VALID;
        NullableField("opcode",attempt,x.max_wait_opcode);Byte(',');NullableField("tag",attempt,x.max_wait_tag);Byte(',');
        NullableField("lba",x.max_wait_flags&NEXUS_STARTUP_COMMAND_LBA_VALID,x.max_wait_lba);
        Text(",\"transport\":\"");Text(!attempt?"unknown":x.max_wait_transport==1?"BOT":x.max_wait_transport==2?"UAS":"unknown");Text("\",");
        NullableField("lun",attempt,x.max_wait_lun);Byte(',');NullableField("sector_size",attempt && x.max_wait_sector_size,x.max_wait_sector_size);
        Text("}}}");return true;
    }
    bool IoDetails(const NexusStartupIoDetails &d,const NexusStartupCommandDetails *commands) {
        static constexpr const char *stages[]={"lock_wait","metadata","prepare","data_write","sync","host_submit","host_wait","host_report"};
        static constexpr const char *ops[]={"read","write","open_file","get_size","metadata","read_directory","open_directory","flush"};
        Text("{\"version\":1,\"supported\":");Text(d.supported?"true":"false");Byte(',');
        Field("sequence",d.sequence);Byte(',');Field("sampled_ns",d.sampled_ns);Byte(',');
        Field("started_ns",d.started_ns);Byte(',');Field("deadline_ns",d.deadline_ns);Byte(',');
        Field("slow_event_total",d.slow_event_total);Byte(',');Field("invalid_samples",d.invalid_samples);
        if(commands) {
            Text(",\"usb_command_summary\":{\"version\":1,");Field("command_count",commands->command_count);Byte(',');
            Field("invalid_samples",commands->invalid_samples);Byte('}');
        }
        Text(",\"operations\":{");
        for(size_t i=0;i<NEXUS_STARTUP_OP_COUNT;++i) {
            if(i){Byte(',');}
            Byte('"');Text(ops[i]);Text("\":{");const auto &op=d.operations[i];
            Field("calls",op.calls);Byte(',');Field("instrumented_calls",op.instrumented_calls);Byte(',');Field("total_ns",op.total_ns);
            Text(",\"stages\":{");
            for(size_t j=0;j<NEXUS_STARTUP_IO_STAGES;++j) {
                if(j){Byte(',');}
                Byte('"');Text(stages[j]);Text("\":{");const auto &phase=op.phases[j];
                Field("calls",phase.calls);Byte(',');Field("total_ns",phase.total_ns);Byte(',');Field("max_ns",phase.max_ns);Byte('}');
            }
            Text("}}");
        }
        Text("},\"slowest_events\":[");
        for(size_t i=0;i<d.event_count;++i) {
            if(i){Byte(',');}
            const auto &e=d.events[i];if(e.operation>=NEXUS_STARTUP_OP_COUNT)return false;
            Text("{\"operation\":\"");Text(ops[e.operation]);Text("\",");Path(e.path,sizeof(e.path));Byte(',');
            Field("relative_start_ns",e.relative_start_ns);Byte(',');Field("duration_ns",e.duration_ns);Byte(',');
            Field("offset",e.offset);Byte(',');Field("requested_bytes",e.requested_bytes);Byte(',');
            Field("completed_bytes",e.completed_bytes);Byte(',');Field("result",e.result);Text(",\"stages\":{");
            for(size_t j=0;j<NEXUS_STARTUP_IO_STAGES;++j) {
                if(j){Byte(',');}
                Byte('"');Text(stages[j]);Text("\":{");Field("calls",e.sample.calls[j]);
                Byte(',');Field("duration_ns",e.sample.duration_ns[j]);Byte('}');
            }
            Byte('}');
            if(commands) {
                const auto &parent=commands->events[i];
                if(parent.relative_start_ns!=e.relative_start_ns || parent.duration_ns!=e.duration_ns || parent.operation!=e.operation)return false;
                Text(",\"usb_commands\":");if(!CommandSample(parent.sample))return false;
            }
            Byte('}');
        }
        Text("]}");return true;
    }
public:
    static constexpr size_t BufferSize=64*1024;
    ReportWriter(char *buffer,size_t size):out(buffer),capacity(size){}
    size_t Format(const NexusStartupSnapshot &p,const NexusStartupIoDetails *details=nullptr,const NexusStartupCommandDetails *commands=nullptr,const NexusUsbWriteCacheSnapshot *cache=nullptr) {
        static constexpr const char *stages[]={"source_open","nro_header_read","nro_code_read","hash_check","usb_activation","code_mapping","memory_permissions","loader_total"};
        static constexpr const char *operations[]={"read","write","open_file","get_size","metadata","read_directory","open_directory","flush"};
        static constexpr const char *states[]={"empty","capturing","window_elapsed","loader_failed","superseded"};
        if(!out||!capacity||p.magic!=NEXUS_STARTUP_MAGIC||p.version!=NEXUS_STARTUP_VERSION||!p.sequence||p.state>NexusStartup_Superseded||p.event_count>NEXUS_STARTUP_EVENT_COUNT)return 0;
        if(details && (details->magic!=NEXUS_STARTUP_IO_MAGIC || details->version!=NEXUS_STARTUP_IO_VERSION ||
           details->size!=sizeof(*details) || details->stage_count!=NEXUS_STARTUP_IO_STAGES ||
           details->sequence!=p.sequence || details->sampled_ns!=p.sampled_ns ||
           details->started_ns!=p.started_ns || details->deadline_ns!=p.deadline_ns ||
           details->supported!=uint32_t(p.loader.source==NEXUS_STARTUP_SOURCE_USB) ||
           details->event_count>NEXUS_STARTUP_IO_EVENTS))return 0;
        if(commands && (!details || commands->magic!=NEXUS_STARTUP_COMMAND_MAGIC || commands->version!=NEXUS_STARTUP_COMMAND_VERSION ||
           commands->size!=sizeof(*commands) || commands->sequence!=p.sequence || commands->sampled_ns!=p.sampled_ns ||
           commands->supported!=details->supported || commands->event_count!=details->event_count))return 0;
        if(cache && (cache->magic!=NEXUS_USB_WRITE_CACHE_MAGIC || cache->version!=NEXUS_USB_WRITE_CACHE_VERSION ||
           cache->size!=sizeof(*cache) || cache->capacity!=NEXUS_USB_WRITE_CACHE_CAPACITY || cache->cached_sectors>cache->capacity))return 0;
        const auto &l=p.loader;
        Text("{\"schema\":1,\"application\":\"Nexus Atmosphere\",\"version\":\"SD14\",\"kind\":\"startup_timings\",\"result\":0,\"automatic\":true,\"firmware_build_id\":\"NEXUS-S3-E1-SD14-completion-probe\",\"query_started_ns\":");Number(p.sampled_ns);
        if(cache) {
            Text(",\"write_cache\":{\"version\":1,\"scope\":\"boot_cumulative_at_autosave\",");
            Field("sampled_ns",cache->sampled_ns);Byte(',');Field("media_generation",cache->media_generation);Byte(',');
            Field("capacity",cache->capacity);Byte(',');Field("sector_size",cache->sector_size);Byte(',');Field("cached_sectors",cache->cached_sectors);Byte(',');
            Field("logical_write_calls",cache->logical_write_calls);Byte(',');Field("logical_write_sectors",cache->logical_write_sectors);Byte(',');
            Field("elided_write_calls",cache->elided_write_calls);Byte(',');Field("elided_write_bytes",cache->elided_write_bytes);Byte(',');
            Field("cache_invalidations",cache->cache_invalidations);Byte('}');
        }
        Text(",\"note\":\"Automatically saved to physical SD after every capture window and observed operation became idle. Covers filesystem views opened after timed forwarder handoff for 60 seconds; not CPU/decompression, system saves, or semantic loading completion. Summed call durations may overlap. Loader total excludes timing submission and game execution. SD hash is skipped.\",\"captures\":[{");
        Field("sequence",p.sequence);Byte(',');Field("pid",p.pid);Text(",\"source\":\"");Text(l.source==NEXUS_STARTUP_SOURCE_SD?"SD":"USB");Text("\",\"state\":\"");Text(states[p.state]);Text("\",");
        Field("started_ns",p.started_ns);Byte(',');Field("deadline_ns",p.deadline_ns);Byte(',');Field("sampled_ns",p.sampled_ns);Text(",\"loader\":{");Path(l.path,sizeof(l.path));Byte(',');Field("nro_bytes",l.nro_bytes);Byte(',');Field("flags",l.flags);Byte(',');Field("result",l.result);
        Text(",\"hash_status\":\"");Text(!(l.flags&NEXUS_STARTUP_HASH_PERFORMED)?"skipped":l.flags&NEXUS_STARTUP_HASH_MATCHED?"matched":"mismatch_or_incomplete");Text("\",\"executable_sha256\":");
        if(l.flags&NEXUS_STARTUP_HASH_PERFORMED)Hex(l.executable_sha256,sizeof(l.executable_sha256));else Text("null");
        Text(",\"stages_ns\":{");for(size_t i=0;i<NEXUS_STARTUP_STAGE_COUNT;++i){if(i)Byte(',');Byte('"');Text(stages[i]);Text("\":");if(i==NexusStartup_Hash&&!(l.flags&NEXUS_STARTUP_HASH_PERFORMED))Text("null");else Number(l.duration_ns[i]);}
        Text("}},\"read_size_calls\":{");static constexpr const char *bands[]={"up_to_4k","up_to_64k","up_to_1m","over_1m"};for(size_t i=0;i<4;++i){if(i)Byte(',');Field(bands[i],p.read_size_calls[i]);}
        Text("},\"operations\":{");for(size_t i=0;i<NEXUS_STARTUP_OP_COUNT;++i){if(i)Byte(',');Byte('"');Text(operations[i]);Text("\":{");const auto &v=p.operations[i];Field("calls",v.calls);Byte(',');Field("requested_bytes",v.requested_bytes);Byte(',');Field("completed_bytes",v.completed_bytes);Byte(',');Field("total_ns",v.total_ns);Byte(',');Field("max_ns",v.max_ns);Byte(',');Field("errors",v.errors);Byte('}');}
        Text("},\"buckets\":[");for(size_t i=0;i<NEXUS_STARTUP_BUCKET_COUNT;++i){if(i)Byte(',');Byte('{');const auto &v=p.buckets[i];Field("start_offset_ns",i*NEXUS_STARTUP_BUCKET_NS);Byte(',');Field("read_bytes",v.read_bytes);Byte(',');Field("io_ns",v.io_ns);Byte(',');Field("calls",v.calls);Byte('}');}
        Text("],");Field("slow_event_total",p.event_total);Text(",\"slow_events\":[");for(size_t i=0;i<p.event_count;++i){if(i)Byte(',');const auto &e=p.events[i];if(e.operation>=NEXUS_STARTUP_OP_COUNT)return 0;Text("{\"operation\":\"");Text(operations[e.operation]);Text("\",");Path(e.path,sizeof(e.path));Byte(',');Field("relative_start_ns",e.relative_start_ns);Byte(',');Field("duration_ns",e.duration_ns);Byte(',');Field("offset",e.offset);Byte(',');Field("requested_bytes",e.requested_bytes);Byte(',');Field("completed_bytes",e.completed_bytes);Byte(',');Field("result",e.result);Byte('}');}
        Byte(']');if(details){Text(",\"filesystem_timings\":");if(!IoDetails(*details,commands))return 0;}
        Text("}]}\n");if(!good)return 0;out[used]=0;return used;
    }
};
}
