// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace nexus_mod {
constexpr size_t HeaderSize=640, RecordSize=544, MaxIndex=1024*1024, MaxFiles=1536;
constexpr uint32_t Empty=0xffffffff;
inline uint32_t U32(const void *p){uint32_t v;std::memcpy(&v,p,4);return v;}
inline uint64_t U64(const void *p){uint64_t v;std::memcpy(&v,p,8);return v;}
inline bool GameId(uint64_t id){return id>=0x0100000000010000ULL && id<0x0600000000000000ULL;}
inline bool Path(const char *p,size_t n,bool absolute){
    if(!n || n>=512 || (absolute && p[0]!='/') || (!absolute && p[0]=='/'))return false;
    size_t start=absolute?1:0;
    for(size_t i=start;i<=n;++i){
        if(i<n && (static_cast<unsigned char>(p[i])<32 || p[i]=='\\' || p[i]==':' || p[i]==0))return false;
        if(i==n || p[i]=='/'){
            size_t len=i-start;if(!len || (len==1 && p[start]=='.') || (len==2 && p[start]=='.' && p[start+1]=='.'))return false;
            start=i+1;
        }
    }
    return true;
}
struct View {
    const uint8_t *bytes{},*meta{};size_t size{};uint32_t count{},meta_size{};
    uint64_t total{},data_offset{};const char *root{};
    const uint8_t *Record(size_t i) const{return bytes+HeaderSize+i*RecordSize;}
    const char *Name(size_t i) const{return reinterpret_cast<const char*>(Record(i)+24);}
};
class Validator {
    const View &v;uint64_t dirs{},dir_size{},files{},file_size{};size_t seen_dirs{},seen_files{};
    static bool Ordered(const uint8_t *previous,size_t old_size,const uint8_t *name,size_t size) {
        if(!previous)return true;
        const int cmp=std::memcmp(previous,name,std::min(old_size,size));
        return cmp<0 || (cmp==0 && old_size<size);
    }
    bool Entry(uint32_t off,bool directory) const {
        uint64_t size=directory?dir_size:file_size,base=directory?dirs:files;
        size_t fixed=directory?24:32;
        for(uint64_t at=0;at<size;){
            if(size-at<fixed)return false;
            uint32_t n=U32(v.meta+base+at+fixed-4);
            uint64_t step=fixed+((uint64_t(n)+3)&~uint64_t(3));
            if(step>size-at)return false;
            if(at==off)return true;
            at+=step;
        }
        return false;
    }
    bool Tree(uint32_t offset,uint32_t parent,char *path,size_t length,unsigned depth){
        if(depth>24 || ++seen_dirs>2048 || !Entry(offset,true))return false;
        auto d=v.meta+dirs+offset;if(U32(d)!=parent)return false;
        uint32_t f=U32(d+12);
        const uint8_t *previous=nullptr;size_t previous_size=0;
        while(f!=Empty){
            if(seen_files>=v.count || !Entry(f,false))return false;
            auto e=v.meta+files+f;uint32_t n=U32(e+28);
            if(U32(e)!=offset || !n || length+n>=511)return false;
            std::memcpy(path+length,e+32,n);path[length+n]=0;
            if(!Path(reinterpret_cast<const char*>(e+32),n,false) || std::memchr(e+32,'/',n))return false;
            if(!Ordered(previous,previous_size,e+32,n))return false;
            previous=e+32;previous_size=n;
            size_t lo=0,hi=v.count;
            while(lo<hi){size_t mid=(lo+hi)/2;if(std::strcmp(v.Name(mid),path)<0)lo=mid+1;else hi=mid;}
            if(lo==v.count)return false;
            auto r=v.Record(lo);
            if(U32(r+16)!=length+n || std::memcmp(r+24,path,length+n) ||
               U64(e+16)!=U64(r+8) || U64(e+8)!=U64(r)-v.data_offset)return false;
            ++seen_files;f=U32(e+4);
        }
        uint32_t child=U32(d+8);size_t children=0;
        previous=nullptr;previous_size=0;
        while(child!=Empty){
            if(++children>2048 || !Entry(child,true))return false;
            auto e=v.meta+dirs+child;uint32_t n=U32(e+20);
            if(!n || length+n+1>=511 || !Path(reinterpret_cast<const char*>(e+24),n,false) || std::memchr(e+24,'/',n))return false;
            if(!Ordered(previous,previous_size,e+24,n))return false;
            previous=e+24;previous_size=n;
            std::memcpy(path+length,e+24,n);path[length+n]='/';path[length+n+1]=0;
            if(!Tree(child,offset,path,length+n+1,depth+1))return false;
            child=U32(e+4);
        }
        return true;
    }
public:
    explicit Validator(const View &value):v(value){}
    bool Run(){
        if(v.meta_size<80 || U64(v.meta)!=80)return false;
        uint64_t end=80;
        for(size_t i=1;i<9;i+=2){
            uint64_t off=U64(v.meta+i*8),size=U64(v.meta+(i+1)*8);
            if(off<end || off>v.meta_size || size>v.meta_size-off)return false;
            if(i==1 || i==5){if(size<12 || size%4)return false;}
            end=off+size;
        }
        dirs=U64(v.meta+24);dir_size=U64(v.meta+32);files=U64(v.meta+56);file_size=U64(v.meta+64);
        if(dir_size<24 || end!=v.meta_size || U64(v.meta+72)!=v.data_offset || U32(v.meta+dirs+20)!=0)return false;
        char path[512]{};
        if(!Tree(0,0,path,0,0) || seen_files!=v.count)return false;
        size_t dir_count=0,file_count=0;
        for(unsigned kind=0;kind<2;++kind){
            uint64_t size=kind?file_size:dir_size,base=kind?files:dirs;size_t fixed=kind?32:24;
            for(uint64_t at=0;at<size;){
                if(size-at<fixed)return false;
                uint64_t step=fixed+((uint64_t(U32(v.meta+base+at+fixed-4))+3)&~uint64_t(3));
                if(step>size-at)return false;
                (kind?file_count:dir_count)++;at+=step;
            }
        }
        return dir_count==seen_dirs && file_count==seen_files;
    }
};
inline bool Parse(View *out,const void *data,size_t size,uint64_t title){
    if(size<HeaderSize || size>MaxIndex || !GameId(title))return false;
    auto b=static_cast<const uint8_t*>(data);
    if(std::memcmp(b,"NXUMOD1\0",8) || U32(b+8)!=1 || U32(b+12)!=HeaderSize || U64(b+16)!=title)return false;
    uint32_t count=U32(b+36),meta=U32(b+32),root_len=U32(b+40);
    if(!count || count>MaxFiles || HeaderSize+uint64_t(count)*RecordSize+meta!=size || root_len>=512 || b[128+root_len]!=0)return false;
    const char *root=reinterpret_cast<const char*>(b+128);
    if(!Path(root,root_len,true))return false;
    View v{b,b+HeaderSize+count*RecordSize,size,count,meta,U64(b+24),(uint64_t(meta)+511)&~uint64_t(511),root};
    uint64_t end=v.data_offset;
    for(size_t i=0;i<count;++i){
        auto r=v.Record(i);uint64_t off=U64(r),n=U64(r+8);uint32_t len=U32(r+16);
        if(off!=((end+15)&~uint64_t(15)) || off>INT64_MAX || n>uint64_t(INT64_MAX)-off || len>=512 || r[24+len]!=0 ||
           !Path(v.Name(i),len,false) || root_len+1+len>=768)return false;
        if(i && std::strcmp(v.Name(i-1),v.Name(i))>=0)return false;
        end=off+n;
    }
    if(end!=v.total || !Validator(v).Run())return false;
    *out=v;return true;
}
}
