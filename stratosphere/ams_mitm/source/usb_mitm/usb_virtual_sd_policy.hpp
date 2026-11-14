// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>
namespace nexus_vsd {
constexpr std::size_t RootCapacity=192, PathCapacity=512;
// The existing shared backend accepts at most 255 bytes for a physical path.
constexpr std::size_t VolumePathCapacity=256;
struct Profile { char root[RootCapacity]{}; char logical[PathCapacity]{}; };
inline bool UsbRoot(const Profile& profile) { return std::strcmp(profile.root,"/")==0; }
inline bool Canonical(std::string_view path) {
    if(path.empty() || path.size()>=PathCapacity || path.front()!='/')return false;
    if(path=="/")return true;
    std::size_t start=1;
    for(std::size_t i=1;i<=path.size();++i) {
        if(i==path.size() || path[i]=='/') {
            auto part=path.substr(start,i-start);
            if(part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ')return false;
            start=i+1;
        }else if(static_cast<unsigned char>(path[i])<32 || path[i]=='\\' || path[i]==':' || path[i]==127)return false;
    }
    return true;
}
inline bool Nro(std::string_view path) {
    if(path.size()<5)return false;
    auto e=path.substr(path.size()-4);
    return e[0]=='.' && (e[1]=='n'||e[1]=='N') && (e[2]=='r'||e[2]=='R') && (e[3]=='o'||e[3]=='O');
}
inline bool Parse(std::string_view path,Profile& out) {
    out={};
    if(path.size()>=VolumePathCapacity || !Canonical(path) || !Nro(path))return false;
    // Normal port layout: explicit USB-root view, never the real microSD.
    if(path.starts_with("/switch/")) {
        out.root[0]='/';std::memcpy(out.logical,path.data(),path.size());return true;
    }
    if(!path.starts_with("/homebrew/"))return false;
    const auto end=path.find('/',10);
    if(end==path.npos || end<=10 || path.substr(end,6)!="/sdmc/")return false;
    const auto name=path.substr(10,end-10);
    for(char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return false;
    auto root=path.substr(0,end+5), logical=path.substr(end+5);
    if(root.size()>=RootCapacity || !Canonical(logical))return false;
    std::memcpy(out.root,root.data(),root.size());std::memcpy(out.logical,logical.data(),logical.size());return true;
}
inline bool Resolve(char* out,std::size_t capacity,const Profile& profile,std::string_view path) {
    if(!Canonical(path))return false;
    if(UsbRoot(profile)) {
        if(path.size()>=capacity || path.size()>=VolumePathCapacity)return false;
        std::memcpy(out,path.data(),path.size());out[path.size()]=0;return true;
    }
    const auto n=std::strlen(profile.root);
    if(!n || n+path.size()>=capacity || n+path.size()>=VolumePathCapacity)return false;
    std::memcpy(out,profile.root,n);
    if(path=="/")out[n]=0;
    else {std::memcpy(out+n,path.data(),path.size());out[n+path.size()]=0;}
    return true;
}
enum class Phase { Idle, Armed, Active };
struct Session {
    Profile profile{};
    std::uint64_t generation{},serial{},armed_at{};
    Phase phase{Phase::Idle};
    void Clear(){profile={};generation=0;armed_at=0;phase=Phase::Idle;++serial;}
    bool Arm(std::string_view path,std::uint64_t gen,std::uint64_t now) {
        Profile candidate{};
        if(!gen || phase!=Phase::Idle || !Parse(path,candidate))return false;
        profile=candidate;generation=gen;armed_at=now;phase=Phase::Armed;++serial;return true;
    }
    // Only a matching, timely loader handoff activates the view. Every other
    // load (including hbmenu/chainload) revokes it before another NRO is opened.
    bool Begin(std::string_view path,std::uint64_t now) {
        if(path.starts_with("sdmc:"))path.remove_prefix(5);
        if(phase==Phase::Armed && now>=armed_at && now-armed_at<=30000 && path==profile.logical) {
            phase=Phase::Active;return true;
        }
        Clear();return false;
    }
    bool Valid(std::uint64_t token,std::uint64_t gen) const {
        return phase==Phase::Active && serial==token && generation==gen && gen;
    }
};
}
