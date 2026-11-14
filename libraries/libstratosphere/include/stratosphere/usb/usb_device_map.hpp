/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ams::usb::mapping {
    constexpr unsigned Limit = 16, EndpointSlots = 30, HeaderWords = 16;
    constexpr unsigned Words = HeaderWords + EndpointSlots * 4;
    constexpr size_t ReportBytes = 96 * 1024;
    struct Descriptor { uint32_t words[Words]{}; };
    struct SnapshotData {
        uint32_t revision{}, query_result{}, current_count{}, saved_count{}, limited{};
        uint32_t attempt_valid{}, attempt_lun{}, lun_count{}, tried_mask{}, empty_mask{}, result{};
        uint64_t sample_ns{}, saved_ns{}, attempt_ns{};
        Descriptor interfaces[Limit]{}, attempt{};
    };
    // One storage-worker writer. Atomics avoid data races; the seqlock reader
    // never waits on a USB/volume lock or blocks live I/O. No device serials.
    inline std::atomic<uint32_t> revision{}, query_result{}, current_count{}, saved_count{}, limited{};
    inline std::atomic<uint32_t> attempt_valid{}, attempt_lun{}, lun_count{}, tried_mask{}, empty_mask{}, result{};
    inline std::atomic<uint64_t> sample_ns{}, saved_ns{}, attempt_ns{};
    inline std::atomic<uint32_t> descriptors[Limit + 1][Words]{};
    constexpr uint32_t Pack(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return (a & 255) | ((b & 255) << 8) | ((c & 255) << 16) | ((d & 255) << 24);
    }
    template<class Interface> Descriptor Encode(const Interface &v) {
        Descriptor out{}; auto *w = out.words;
        const auto &d = v.device_desc; const auto &c = v.config_desc; const auto &i = v.inf.interface_desc;
        w[0] = (uint32_t(d.idVendor) << 16) | d.idProduct;
        w[1] = (uint32_t(d.bcdUSB) << 16) | d.bcdDevice;
        w[2] = Pack(d.bDeviceClass,d.bDeviceSubClass,d.bDeviceProtocol,d.bMaxPacketSize0);
        w[3] = Pack(d.iManufacturer,d.iProduct,d.iSerialNumber,d.bNumConfigurations);
        w[4] = Pack(d.bLength,d.bDescriptorType,0,0);
        w[5] = uint32_t(c.wTotalLength) | (uint32_t(c.bNumInterfaces) << 16) | (uint32_t(c.bConfigurationValue) << 24);
        w[6] = Pack(c.bLength,c.bDescriptorType,c.iConfiguration,c.bmAttributes);
        w[7] = c.MaxPower;
        w[8] = Pack(i.bInterfaceClass,i.bInterfaceSubClass,i.bInterfaceProtocol,i.bInterfaceNumber);
        w[9] = Pack(i.bLength,i.bDescriptorType,i.bAlternateSetting,i.bNumEndpoints);
        w[10] = i.iInterface;
        w[11] = v.busID; w[12] = v.deviceID; w[13] = v.inf.deviceID_2; w[14] = uint32_t(v.inf.ID);
        for (unsigned n=0;n<EndpointSlots;++n) {
            const auto &e = n<15 ? v.inf.input_endpoint_descs[n] : v.inf.output_endpoint_descs[n-15];
            const auto &s = n<15 ? v.inf.input_ss_endpoint_companion_descs[n] : v.inf.output_ss_endpoint_companion_descs[n-15];
            const unsigned p=HeaderWords+n*4;
            w[p] = Pack(e.bLength,e.bDescriptorType,e.bEndpointAddress,e.bmAttributes);
            w[p+1] = uint32_t(e.wMaxPacketSize) | (uint32_t(e.bInterval) << 16);
            w[p+2] = Pack(s.bLength,s.bDescriptorType,s.bMaxBurst,s.bmAttributes);
            w[p+3] = s.wBytesPerInterval;
        }
        return out;
    }
    inline void Store(unsigned index,const Descriptor &v) {
        for(unsigned n=0;n<Words;++n) descriptors[index][n].store(v.words[n]);
    }
    template<class Interface>
    void Observe(const Interface *items,size_t count,size_t capacity,uint32_t rc,uint64_t now) {
        revision.fetch_add(1);
        sample_ns.store(now); query_result.store(rc); current_count.store(rc?0:count);
        // Preserve the last nonempty snapshot after unplug/failure. QueryAll
        // descriptors are historical evidence, never acquisition authority.
        if(!rc && count) {
            const unsigned kept=count<Limit?count:Limit;
            saved_ns.store(now);saved_count.store(kept);limited.store(count>=capacity || count>Limit);
            for(unsigned n=0;n<kept;++n) Store(n,Encode(items[n]));
        }
        revision.fetch_add(1);
    }
    template<class Interface>
    void BeginAttempt(const Interface &v,uint32_t lun,uint64_t now) {
        revision.fetch_add(1);
        // A new interface scan starts at LUN 0; do not mix another device's
        // slot results into the next attempt, even if an ID has been reused.
        if(lun==0) {tried_mask.store(0);empty_mask.store(0);lun_count.store(0);}
        Store(Limit,Encode(v));attempt_valid.store(1);attempt_lun.store(lun);
        attempt_ns.store(now);result.store(0xFFFFFFFF);
        if(lun<16)tried_mask.fetch_or(1u<<lun);
        revision.fetch_add(1);
    }
    inline void EndAttempt(uint32_t count,bool empty,uint32_t rc) {
        revision.fetch_add(1);
        lun_count.store(count);result.store(rc);
        const auto lun=attempt_lun.load();if(empty && lun<16)empty_mask.fetch_or(1u<<lun);
        revision.fetch_add(1);
    }
    inline bool Snapshot(SnapshotData &v) {
        v.revision=revision.load();if(v.revision&1)return false;
        v.query_result=query_result.load();v.current_count=current_count.load();v.saved_count=saved_count.load();v.limited=limited.load();
        v.sample_ns=sample_ns.load();v.saved_ns=saved_ns.load();v.attempt_ns=attempt_ns.load();
        v.attempt_valid=attempt_valid.load();v.attempt_lun=attempt_lun.load();v.lun_count=lun_count.load();
        v.tried_mask=tried_mask.load();v.empty_mask=empty_mask.load();v.result=result.load();
        for(unsigned i=0;i<Limit;++i)for(unsigned n=0;n<Words;++n)v.interfaces[i].words[n]=descriptors[i][n].load();
        for(unsigned n=0;n<Words;++n)v.attempt.words[n]=descriptors[Limit][n].load();
        return v.revision==revision.load();
    }
    // Buffer is supplied by the monitor, not the USB worker. Large snapshot
    // storage must be static/heap, never the monitor thread's small stack.
    struct Writer {
        char *out;size_t capacity;size_t length{};bool ok{true};
        template<class... Args> void Add(const char *fmt,Args... args) {
            if(!ok)return;
            const int n=util::SNPrintf(out+length,capacity-length,fmt,args...);
            if(n<=0 || size_t(n)>=capacity-length){ok=false;return;}length+=n;
        }
    };
    inline void FormatDescriptor(Writer &s,const char *prefix,const Descriptor &d) {
        const auto *w=d.words;
        s.Add("%s.vid=0x%04X\n%s.pid=0x%04X\n",prefix,w[0]>>16,prefix,w[0]&65535);
        s.Add("%s.bus_id=0x%08X\n%s.device_id=0x%08X\n%s.device_id_2=0x%08X\n%s.interface_id=0x%08X\n",prefix,w[11],prefix,w[12],prefix,w[13],prefix,w[14]);
        s.Add("%s.usb_bcd=0x%04X\n%s.device_bcd=0x%04X\n%s.interface=%u,%u,0x%02X,0x%02X,0x%02X,%u\n",
            prefix,w[1]>>16,prefix,w[1]&65535,prefix,w[8]>>24,(w[9]>>16)&255,w[8]&255,(w[8]>>8)&255,(w[8]>>16)&255,w[9]>>24);
        s.Add("%s.config=%u,%u,0x%02X,%u\n",prefix,w[5]>>24,(w[5]>>16)&255,w[6]>>24,w[7]);
        // Raw fixed fields retain every descriptor bit for later analysis.
        // String *indices*, not identifying string contents, are included.
        s.Add("%s.raw=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",prefix,
            w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],w[8],w[9],w[10]);
        unsigned count=0;
        for(unsigned n=0;n<EndpointSlots;++n) {
            const unsigned p=HeaderWords+n*4;
            if(!(w[p]|w[p+1]|w[p+2]|w[p+3]))continue;
            ++count;
            s.Add("%s.%s.%u=0x%02X,0x%02X,%u,%u,%u,0x%02X,%u\n",prefix,n<15?"in":"out",n%15,
                (w[p]>>16)&255,w[p]>>24,w[p+1]&65535,(w[p+1]>>16)&255,(w[p+2]>>16)&255,w[p+2]>>24,w[p+3]&65535);
            s.Add("%s.%s.%u.raw=%08X,%08X,%08X,%08X\n",prefix,n<15?"in":"out",n%15,w[p],w[p+1],w[p+2],w[p+3]);
        }
        s.Add("%s.endpoint_count=%u\n",prefix,count);
    }
    inline int Format(char *out,size_t size,const SnapshotData &v) {
        Writer s{out,size};
        s.Add("usb_map_version=1\nusb_map_revision=%u\nusb_map_physical_ports=not_exposed\n"
              "usb_map_scope=usb_hs_exposed_interfaces\nusb_map_alternates=reported_only\n"
              "usb_map_interface_limit=%u\nusb_map_endpoint_slots_per_interface=30\n"
              "usb_map_strings=indices_only_no_serial_values\n"
              "usb_map_sample_ns=%llu\nusb_map_query_result=0x%08X\nusb_map_current_count=%u\n"
              "usb_map_saved_ns=%llu\nusb_map_saved_count=%u\nusb_map_limited=%u\n",
              v.revision,Limit,(unsigned long long)v.sample_ns,v.query_result,v.current_count,
              (unsigned long long)v.saved_ns,v.saved_count,v.limited);
        s.Add("usb_map_interface_fields=number,alternate,class,subclass,protocol,declared_endpoints\n"
              "usb_map_config_fields=value,interfaces,attributes,max_power_raw\n"
              "usb_map_endpoint_fields=address,attributes,max_packet_raw,interval,max_burst,ss_attributes,bytes_per_interval\n"
              "usb_map_endpoint_zero_descriptor=not_exposed\nusb_map_power_units=USB2_2mA_USB3_8mA\n"
              "usb_map_bus_ids=opaque_not_physical_port_numbers\n");
        for(unsigned i=0;i<v.saved_count && i<Limit;++i) {
            char prefix[20];util::SNPrintf(prefix,sizeof(prefix),"map.%u",i);FormatDescriptor(s,prefix,v.interfaces[i]);
        }
        s.Add("usb_map_attempt_valid=%u\nusb_map_attempt_ns=%llu\nusb_map_attempt_lun=%u\n"
              "usb_map_lun_count=%u\nusb_map_tried_mask=0x%04X\nusb_map_empty_mask=0x%04X\nusb_map_attempt_result=0x%08X\n",
              v.attempt_valid,(unsigned long long)v.attempt_ns,v.attempt_lun,v.lun_count,v.tried_mask,v.empty_mask,v.result);
        if(v.attempt_valid)FormatDescriptor(s,"attempt",v.attempt);
        s.Add("usb_map_end=%u\n",v.revision);
        return s.ok?int(s.length):-1;
    }
}
