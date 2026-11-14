/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ams::usb::interface_recovery {
    // A standard-interface fallback, NOT a reader identification heuristic.
    // Never switch protocol/alternate, reset a device/hub, or run on mounted I/O.
    template<class Info> bool Shape(const Info &i) {
        const auto &d=i.interface_desc;
        if(d.bInterfaceClass!=8 || (d.bInterfaceSubClass!=6 && d.bInterfaceSubClass!=1) ||
           d.bInterfaceProtocol!=0x50 || d.bNumEndpoints!=2) return false;
        unsigned in=0,out=0;
        for(const auto &e:i.input_endpoint_descs) {
            if(!e.bDescriptorType) continue;
            if(e.bDescriptorType!=5 || e.bmAttributes!=2 || !(e.bEndpointAddress&0x80) ||
               !(e.bEndpointAddress&15) || !e.wMaxPacketSize) return false;
            ++in;
        }
        for(const auto &e:i.output_endpoint_descs) {
            if(!e.bDescriptorType) continue;
            if(e.bDescriptorType!=5 || e.bmAttributes!=2 || (e.bEndpointAddress&0x80) ||
               !(e.bEndpointAddress&15) || !e.wMaxPacketSize) return false;
            ++out;
        }
        return in==1 && out==1;
    }
    template<class Device> bool Eligible(const Device &d) {
        return d.config_desc.bNumInterfaces==1 && Shape(d.inf);
    }
    template<class Info> bool SameShape(const Info &a,const Info &b) {
        return Shape(a) && Shape(b) &&
            !std::memcmp(&a.interface_desc,&b.interface_desc,sizeof(a.interface_desc)) &&
            !std::memcmp(a.input_endpoint_descs,b.input_endpoint_descs,sizeof(a.input_endpoint_descs)) &&
            !std::memcmp(a.output_endpoint_descs,b.output_endpoint_descs,sizeof(a.output_endpoint_descs)) &&
            !std::memcmp(a.input_ss_endpoint_companion_descs,b.input_ss_endpoint_companion_descs,sizeof(a.input_ss_endpoint_companion_descs)) &&
            !std::memcmp(a.output_ss_endpoint_companion_descs,b.output_ss_endpoint_companion_descs,sizeof(a.output_ss_endpoint_companion_descs));
    }
    class Budget {
        struct Key { uint32_t bus,device,device2,model,version; } keys[16]{};
        unsigned used{};
        public:
        template<class Device> bool Take(const Device &d) {
            const Key key{d.busID,d.deviceID,d.inf.deviceID_2,
                (uint32_t(d.device_desc.idVendor)<<16)|d.device_desc.idProduct,
                (uint32_t(d.device_desc.bcdUSB)<<16)|d.device_desc.bcdDevice};
            for(unsigned n=0;n<used;++n) if(!std::memcmp(&key,&keys[n],sizeof(key))) return false;
            // Saturation denies only the extra fallback, never normal mounts.
            // Do not evict old entries and accidentally re-arm a failing device.
            if(used==16) return false;
            keys[used++]=key; return true;
        }
    };
    inline std::atomic<uint32_t> attempts{},initialized{},denied{},stage{},original{},final{},reselect_result{UINT32_MAX};
    template<class Result,class Permit,class Refresh,class Retry,class Stop>
    Result Attempt(bool eligible,Result initial,Permit permit,Refresh refresh,Retry retry,Stop stop) {
        if(!eligible || !initial.GetValue() || stop()) return initial;
        if(!permit()) { denied.fetch_add(1); return initial; }
        attempts.fetch_add(1); stage.store(1); original.store(initial.GetValue());
        reselect_result.store(UINT32_MAX); final.store(initial.GetValue());
        auto rc=refresh();
        if(rc.GetValue()) { stage.store(4); final.store(rc.GetValue()); return rc; }
        if(stop()) { stage.store(5); return initial; }
        stage.store(2); rc=retry(); final.store(rc.GetValue());
        if(!rc.GetValue()) initialized.fetch_add(1);
        stage.store(rc.GetValue()?6:3); return rc;
    }
    inline int Format(char *out,size_t n) {
        return std::snprintf(out,n,
          "usb_interface_recovery=C13\nusb_interface_recovery_scope=pre_mount_single_interface_BOT_once_per_attachment\n"
          "usb_interface_recovery_stage=%u\nusb_interface_recovery_attempts=%u\nusb_interface_recovery_initialized=%u\n"
          "usb_interface_recovery_denied=%u\nusb_interface_recovery_original=0x%08X\n"
          "usb_interface_recovery_reselect=0x%08X\nusb_interface_recovery_final=0x%08X\n",
          stage.load(),attempts.load(),initialized.load(),denied.load(),original.load(),reselect_result.load(),final.load());
    }
}
