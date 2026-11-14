/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "usb_write_cache_protocol.h"

namespace ams::usb {
    // Clean, exact bytes from completed I/O only. The serialized FatFs disk
    // owner must invalidate overlapping entries BEFORE issuing any write and
    // clear after any error or media transition. Never serves reads or keeps
    // dirty data. Matching a sector permits suppressing an identical write.
    class CleanSectorCache {
        static constexpr size_t Capacity = NEXUS_USB_WRITE_CACHE_CAPACITY;
        static constexpr size_t MaxSectorSize = 4096;
        uint8_t bytes[Capacity][MaxSectorSize]{};
        uint64_t lbas[Capacity]{};
        bool valid[Capacity]{};
        size_t next{};
        uint32_t sector_size{};
        size_t Find(uint64_t lba) const {
            for (size_t i=0;i<Capacity;++i) if (valid[i] && lbas[i]==lba) return i;
            return Capacity;
        }
        public:
            uint32_t Count() const {
                uint32_t count=0;for (const auto entry:valid) count+=entry;return count;
            }
            bool Clear() {
                bool changed=false;
                for (auto &entry:valid) { changed|=entry;entry=false; }
                next=0;return changed;
            }
            bool Configure(uint32_t size) {
                const bool changed=Clear();
                sector_size=size>=512 && size<=MaxSectorSize && !(size&(size-1))?size:0;
                return changed;
            }
            bool Invalidate(uint64_t first,uint32_t count) {
                bool changed=false;
                for (size_t i=0;i<Capacity;++i) if (valid[i] && lbas[i]>=first && lbas[i]-first<count) {
                    valid[i]=false;changed=true;
                }
                return changed;
            }
            bool Matches(uint64_t lba,const void *data) const {
                if (!sector_size || !data) return false;
                const auto i=Find(lba);
                return i<Capacity && std::memcmp(bytes[i],data,sector_size)==0;
            }
            void Seed(uint64_t first,uint32_t count,const void *data) {
                if (!sector_size || !data || !count) return;
                if (first>UINT64_MAX-(count-1)) { Clear();return; }
                auto *source=static_cast<const uint8_t *>(data);
                if (count>Capacity) {
                    const auto skip=count-Capacity;
                    first+=skip;source+=static_cast<size_t>(skip)*sector_size;count=Capacity;
                }
                for (uint32_t n=0;n<count;++n,++first,source+=sector_size) {
                    auto i=Find(first);
                    if (i==Capacity) { i=next;next=(next+1)%Capacity; }
                    std::memcpy(bytes[i],source,sector_size);
                    lbas[i]=first;valid[i]=true;
                }
            }
    };
}
