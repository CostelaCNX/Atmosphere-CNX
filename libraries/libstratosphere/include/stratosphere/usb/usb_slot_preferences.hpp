/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ams::usb::slot_preferences {
    // Advisory probe order only. A descriptor fingerprint is NOT proof of
    // individual drive/card identity, presence, capacity or filesystem health.
    constexpr size_t Limit=8, FileSize=16+Limit*16;
    struct Entry { uint64_t key{}; uint32_t lun{}; };
    struct Table { Entry entries[Limit]{}; };
    inline Table table;
    inline std::atomic_flag lock=ATOMIC_FLAG_INIT;
    inline uint32_t revision=0, replacement=0;
    inline std::atomic<uint32_t> hits{}, misses{}, fallbacks{}, learned{}, last_lun{}, last_result{};

    struct Guard {
        bool held=!lock.test_and_set(std::memory_order_acquire);
        ~Guard(){if(held)lock.clear(std::memory_order_release);}
    };
    // Never wait for the monitor from the USB worker. No FS/USB IPC under lock.
    inline int Lookup(uint64_t key) {
        Guard guard;if(!guard.held)return -1;
        for(const auto &e:table.entries)if(key && e.key==key){
            hits.fetch_add(1);last_lun.store(e.lun);return static_cast<int>(e.lun);
        }
        misses.fetch_add(1);return -1;
    }
    inline void Remember(uint64_t key,uint32_t lun) {
        if(!key||lun>=16)return;
        Guard guard;if(!guard.held)return;
        size_t slot=Limit;
        for(size_t i=0;i<Limit;++i)if(table.entries[i].key==key){slot=i;break;}
        if(slot<Limit && table.entries[slot].lun==lun)return;
        if(slot==Limit)for(size_t i=0;i<Limit;++i)if(!table.entries[i].key){slot=i;break;}
        if(slot==Limit)slot=replacement++%Limit;
        table.entries[slot]={key,lun};if(++revision==0)++revision;
        learned.fetch_add(1);
    }
    inline void Forget(uint64_t key) {
        Guard guard;if(!guard.held)return;
        for(auto &e:table.entries)if(key && e.key==key){e={};if(++revision==0)++revision;}
    }
    inline bool Snapshot(Table &out,uint32_t &rev) {
        Guard guard;if(!guard.held)return false;out=table;rev=revision;return true;
    }
    // Called once, BEFORE publishing the service/starting the storage worker.
    inline void Load(const Table &in) {
        Guard guard;if(!guard.held)return;table=in;revision=0;replacement=0;
    }
    inline void Put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=static_cast<uint8_t>(n>>(i*8));}
    inline uint32_t Get32(const uint8_t *p){uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(p[i])<<(i*8);return n;}
    inline uint32_t Checksum(const uint8_t *p) {
        uint32_t crc=~0u;
        for(size_t i=0;i<FileSize;++i){
            if(i>=12&&i<16)continue;
            crc^=p[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^((0u-(crc&1u))&0xEDB88320u);
        }
        return ~crc;
    }
    inline void Encode(uint8_t (&bytes)[FileSize],const Table &in) {
        for(auto &b:bytes)b=0;
        Put32(bytes,0x4C533643);Put32(bytes+4,1);Put32(bytes+8,Limit);
        for(size_t i=0;i<Limit;++i){auto *p=bytes+16+i*16;const auto &e=in.entries[i];
            Put32(p,static_cast<uint32_t>(e.key));Put32(p+4,static_cast<uint32_t>(e.key>>32));Put32(p+8,e.lun);
        }
        Put32(bytes+12,Checksum(bytes));
    }
    inline bool Decode(Table &out,const uint8_t *bytes,size_t size) {
        if(size!=FileSize||Get32(bytes)!=0x4C533643||Get32(bytes+4)!=1||Get32(bytes+8)!=Limit||Get32(bytes+12)!=Checksum(bytes))return false;
        Table next{};
        for(size_t i=0;i<Limit;++i){const auto *p=bytes+16+i*16;auto &e=next.entries[i];
            e={uint64_t(Get32(p))|(uint64_t(Get32(p+4))<<32),Get32(p+8)};
            if(e.lun>=16||Get32(p+12)||(!e.key&&e.lun))return false;
            if(e.key)for(size_t j=0;j<i;++j)if(next.entries[j].key==e.key)return false;
        }
        out=next;return true;
    }
}
