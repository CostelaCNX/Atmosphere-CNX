// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nexus_sd_io_protocol.h"
#include <cstddef>
#include <cstring>

namespace nexus_sd_io {
enum class Origin:unsigned {Other,UsbRequest,UsbSupport,Diagnostic,NativeContent};
enum class Kind:unsigned {FileSystem,File,Directory};
enum class Op:unsigned {Read,Write,Metadata,Flush};
struct Key {
    uint32_t session,object;
    bool operator==(const Key& b)const{return session==b.session&&object==b.object;}
};
struct Tag {bool known{};Origin origin=Origin::Other;};
template<size_t Capacity> struct Registry {
    struct Entry {bool used{};Key key{};Kind kind{};Origin origin{};};
    Entry entries[Capacity]{};
    uint64_t drops{};
    Tag find(Key key,Kind kind)const {
        for(const auto& e:entries)if(e.used&&e.key==key&&e.kind==kind)return {true,e.origin};
        return {};
    }
    bool add(Key key,Kind kind,Origin origin) {
        for(auto& e:entries)if(e.used&&e.key==key&&e.kind==kind){e.origin=origin;return true;}
        for(auto& e:entries)if(!e.used){e={true,key,kind,origin};return true;}
        ++drops;return false;
    }
    void erase(Key key,Kind kind) {
        for(auto& e:entries)if(e.used&&e.key==key&&e.kind==kind){e.used=false;return;}
    }
};
inline Origin classify(Origin stored,Origin active,const char* path=nullptr) {
    if(stored==Origin::Diagnostic || (path && std::strncmp(path,"/atmosphere/logs/",17)==0))return Origin::Diagnostic;
    if(active!=Origin::Other)return active;
    return stored;
}
inline void begin(NexusSdIoBucket& b,Op op) {
    ++b.in_flight;
    switch(op){case Op::Read:++b.read_calls;break;case Op::Write:++b.write_calls;break;
        case Op::Metadata:++b.metadata_calls;break;case Op::Flush:++b.flush_calls;break;}
}
inline void finish(NexusSdIoBucket& b,Op op,uint64_t ns,uint32_t result,uint64_t bytes) {
    --b.in_flight;
    switch(op){
        case Op::Read:b.read_ns+=ns;if(result)++b.read_errors;else b.read_bytes+=bytes;break;
        case Op::Write:b.write_ns+=ns;if(result)++b.write_errors;else b.write_bytes+=bytes;break;
        case Op::Metadata:b.metadata_ns+=ns;if(result)++b.metadata_errors;break;
        case Op::Flush:b.flush_ns+=ns;if(result)++b.flush_errors;break;
    }
}
}
