// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
namespace nexus_eject {
    constexpr uint64_t Magic=0x314A45465355584EULL; // NXUSFEJ1, versioned binary request
    enum : uint32_t { Eject=1, Resume=2 };
    struct Request { uint64_t magic{}, boot{}, sample_ms{}, generation{}, nonce{}; uint32_t action{}, reserved{}; };
    static_assert(sizeof(Request)==48);
    struct Snapshot {
        uint64_t boot{},sample_ms{},generation{},request{},ack{};
        uint32_t ack_result{},phase{},stage{},result{},handles{},ready{};
    };
    inline bool Valid(const Request&r,uint64_t boot,uint64_t now) {
        return r.magic==Magic && r.boot==boot && boot && r.nonce && !r.reserved
            && r.sample_ms<=now && now-r.sample_ms<=6000
            && (r.action==Eject || r.action==Resume);
    }
    // A fixed complete record avoids duplicate/missing-field ambiguity. The
    // final newline is required; truncated or trailing data is not accepted.
    inline bool Parse(const char*data,size_t size,Snapshot&out) {
        if(!data || size==0 || size>=512 || data[size-1]!='\n') return false;
        if(size<7 || std::memcmp(data,"NXEJ1 ",6))return false;
        uint64_t v[11]{};size_t p=6;
        for(unsigned i=0;i<11;++i){
            const size_t begin=p;const uint64_t limit=i<5?UINT64_MAX:UINT32_MAX;
            while(p<size&&data[p]>='0'&&data[p]<='9'){
                const unsigned d=unsigned(data[p]-'0');
                if(v[i]>(limit-d)/10)return false;
                v[i]=v[i]*10+d;++p;
            }
            if(p==begin || (p>begin+1&&data[begin]=='0') || p>=size || data[p++]!=(i==10?'\n':' '))return false;
        }
        if(p!=size||!v[0]||v[6]>4||v[7]>7||v[10]>1)return false;
        out={v[0],v[1],v[2],v[3],v[4],uint32_t(v[5]),uint32_t(v[6]),uint32_t(v[7]),uint32_t(v[8]),uint32_t(v[9]),uint32_t(v[10])};return true;
    }
    inline bool Safe(const Snapshot&s,uint64_t boot,uint64_t nonce) {
        return boot && nonce && s.boot==boot && s.request==nonce && s.ack==nonce
            && !s.ack_result && s.phase==3 && s.stage==7 && !s.result && !s.ready && !s.handles;
    }
}
