/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once
#include <stratosphere/usb/usb_reader_policy.hpp>
#include <stratosphere/usb/usb_reader_diagnostics.hpp>
#include <stratosphere/usb/usb_device_map.hpp>
#include <stratosphere/usb/usb_slot_preferences.hpp>

namespace ams::usb::reader {
    inline uint64_t SlotKey(const host::Interface &v) {
        // Stable descriptor/model fingerprint, excluding transient IDs and bus
        // location. Identical readers may share a hint: validation still runs.
        if(v.inf.interface_desc.bInterfaceProtocol!=0x50)return 0;
        uint64_t key=14695981039346656037ULL;
        const auto mix=[&](const auto &value){
            const auto *p=reinterpret_cast<const uint8_t *>(&value);
            for(size_t i=0;i<sizeof(value);++i){key^=p[i];key*=1099511628211ULL;}
        };
        mix(v.device_desc);mix(v.config_desc);mix(v.inf.interface_desc);
        for(const auto &ep:v.inf.input_endpoint_descs)if(ep.bDescriptorType==5)mix(ep);
        for(const auto &ep:v.inf.output_endpoint_descs)if(ep.bDescriptorType==5)mix(ep);
        return key?key:1;
    }
    inline bool SupportedInterface(const host::Interface &v) {
        const auto &d=v.inf.interface_desc;
        return Supported(d.bInterfaceClass,d.bInterfaceSubClass,d.bInterfaceProtocol);
    }
    inline Result SelectAvailable(host::Interface *out, size_t bytes, s32 *count) {
        R_UNLESS(*count>=0 && static_cast<size_t>(*count)<=bytes/sizeof(*out),usb::ResultInternalStateError());
        s32 kept=0;
        for (s32 i=0;i<*count;++i) {
            // QueryAll uses ID=-1 and must NEVER feed acquisition.
            if (out[i].inf.ID<0 || !SupportedInterface(out[i])) continue;
            bool duplicate=false;
            for (s32 j=0;j<kept;++j) if (out[j].inf.ID==out[i].inf.ID) duplicate=true;
            if (!duplicate) out[kept++]=out[i];
        }
        *count=kept;
        R_SUCCEED();
    }
    inline Result QueryAvailable(host::Client &client, const host::InterfaceFilter &primary,
                                 host::Interface *out, size_t bytes, s32 *count) {
        *count=0;
        R_TRY(client.QueryAvailableInterfaces(&primary,out,bytes,count));
        R_TRY(SelectAvailable(out,bytes,count));
        if (*count>0) R_SUCCEED();
        // Generic mass-storage discovery only when the normal path is empty.
        // Validate descriptors locally; class-only is NOT permission to send
        // BOT commands to proprietary/CBI/UFI interfaces.
        host::InterfaceFilter broad{};broad.Flags=1u<<7;broad.bInterfaceClass=0x08;
        fallback_queries.fetch_add(1);
        const auto result=client.QueryAvailableInterfaces(&broad,out,bytes,count);
        fallback_result.store(result.GetValue());
        R_TRY(result);
        const auto selected=SelectAvailable(out,bytes,count);
        fallback_result.store(selected.GetValue());R_TRY(selected);
        if (*count>0) fallback_matches.fetch_add(1);
        R_SUCCEED();
    }
    // Match an attachment, not just its model. Never reset a replacement drive
    // that happens to share a VID/PID or an interface number.
    inline bool SameAttachment(const host::Interface &a, const host::Interface &b) {
        return a.busID == b.busID && a.deviceID == b.deviceID && a.inf.deviceID_2 == b.inf.deviceID_2 &&
            std::memcmp(&a.device_desc, &b.device_desc, sizeof(a.device_desc)) == 0 &&
            std::memcmp(&a.config_desc, &b.config_desc, sizeof(a.config_desc)) == 0 &&
            std::memcmp(&a.inf.interface_desc, &b.inf.interface_desc, sizeof(a.inf.interface_desc)) == 0 &&
            std::memcmp(a.inf.input_endpoint_descs, b.inf.input_endpoint_descs, sizeof(a.inf.input_endpoint_descs)) == 0 &&
            std::memcmp(a.inf.output_endpoint_descs, b.inf.output_endpoint_descs, sizeof(a.inf.output_endpoint_descs)) == 0;
    }
    inline Result RefreshSame(host::Client &client, const host::InterfaceFilter &filter, host::Interface *expected) {
        // Worker-only, with no live/mounted device. Avoid a large stack array.
        static host::Interface scratch[mapping::Limit];
        s32 count=0;
        R_TRY(QueryAvailable(client,filter,scratch,sizeof(scratch),&count));
        s32 match=-1;
        for (s32 i=0;i<count;++i) if (SameAttachment(*expected,scratch[i])) {
            R_UNLESS(match<0,usb::ResultOperationDenied()); // Ambiguous attachment.
            match=i;
        }
        R_UNLESS(match>=0,usb::ResultNotInitialized());
        *expected=scratch[match]; // Only fresh acquire-capable IDs are used.
        R_SUCCEED();
    }
    inline void CaptureDeviceMap(host::Client &client, uint64_t now) {
        // Worker only, before any interface is acquired. Static so the full
        // descriptor inventory does not consume the worker's small stack.
        static host::Interface scratch[mapping::Limit];
        host::InterfaceFilter all{};all.Flags=1u<<2;
        s32 count=0;
        auto rc=client.QueryAllInterfaces(&all,scratch,sizeof(scratch),&count);
        if(R_SUCCEEDED(rc) && (count<0 || count>static_cast<s32>(mapping::Limit)))
            rc=usb::ResultInternalStateError();
        mapping::Observe(scratch,R_SUCCEEDED(rc)?count:0,mapping::Limit,rc.GetValue(),now);
    }
    inline void CaptureInventory(host::Client &client, host::Interface *scratch, size_t bytes, uint64_t now) {
        Inventory v{};v.sampled_ns=now;
        // bcdDevice>=0 includes every descriptor, with a nonzero filter flag.
        // These host-side descriptor queries do not open/reset devices or read
        // any card sectors. Reuse idle acquired-buffer storage, never live I/O.
        host::InterfaceFilter all{};all.Flags=1u<<2;
        s32 count=0;
        const auto all_rc=client.QueryAllInterfaces(&all,scratch,bytes,&count);
        v.all_result=all_rc.GetValue();
        const size_t capacity=bytes/sizeof(*scratch);
        if (R_SUCCEEDED(all_rc) && count>=0 && static_cast<size_t>(count)<=capacity) {
            v.all_count=count;v.limited=count>=static_cast<s32>(capacity) || count>static_cast<s32>(InventoryLimit);
            for (s32 i=0;i<count;++i) {
                const auto &e=scratch[i];const auto &d=e.inf.interface_desc;
                if (d.bInterfaceClass==8) ++v.storage_count;
                if (SupportedInterface(e)) ++v.supported_count;
                if (i<static_cast<s32>(InventoryLimit)) v.entries[i]={
                    (static_cast<uint32_t>(e.device_desc.idVendor)<<16)|e.device_desc.idProduct,
                    (static_cast<uint32_t>(d.bInterfaceClass)<<24)|(static_cast<uint32_t>(d.bInterfaceSubClass)<<16)|
                        (static_cast<uint32_t>(d.bInterfaceProtocol)<<8)|d.bInterfaceNumber,
                    (static_cast<uint32_t>(d.bAlternateSetting)<<24)|(e.busID&0xFF)<<16|(e.deviceID&0xFFFF)};
            }
        } else if (R_SUCCEEDED(all_rc)) v.all_result=usb::ResultInternalStateError().GetValue();
        host::InterfaceFilter available{};available.Flags=1u<<7;available.bInterfaceClass=8;
        count=0;
        const auto avail_rc=client.QueryAvailableInterfaces(&available,scratch,bytes,&count);
        v.available_result=avail_rc.GetValue();
        if (R_SUCCEEDED(avail_rc) && count>=0 && static_cast<size_t>(count)<=capacity) {
            v.available_count=count;v.limited|=count>=static_cast<s32>(capacity);
            for (s32 i=0;i<count;++i) if (scratch[i].inf.ID>=0 && SupportedInterface(scratch[i])) ++v.available_supported;
        } else if (R_SUCCEEDED(avail_rc)) v.available_result=usb::ResultInternalStateError().GetValue();
        Publish(v);
    }
}
