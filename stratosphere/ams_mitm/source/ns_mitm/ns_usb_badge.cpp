// SPDX-License-Identifier: GPL-2.0-only
#include <stratosphere.hpp>
#include "ns_am_mitm_service.hpp"
#include "badge_format.hpp"
#include "ns_handoff_trace.hpp"
#include "../amsmitm_fs_utils.hpp"

namespace ams::mitm::ns {
namespace {
    // One bounded scratch buffer; no JPEG codec, USB IPC, NCM scan or worker
    // wait in the HOME path. These files are already-rendered physical SD data.
    constinit os::SdkMutex g_badge_mutex;
    alignas(16) u8 g_badge_jpeg[nexus_badge::MaxJpeg];

    u32 Apply(ncm::ProgramId client,ncm::ProgramId id,const sf::OutBuffer &buffer,u32 actual,u8 variant) {
        if(client!=ncm::SystemAppletId::Qlaunch || !nexus_badge::Title(id.value) || variant>1 ||
            !nexus_badge::Fits(actual,buffer.GetSize(),4))return actual;
        std::unique_lock lock(g_badge_mutex,std::try_to_lock);
        if(!lock.owns_lock())return actual;
        char path[96];
        util::SNPrintf(path,sizeof(path),"/atmosphere/usb_badges/%016llx-%u.ubg",static_cast<unsigned long long>(id.value),variant);
        FsFile file{};
        if(R_FAILED(mitm::fs::OpenSdFile(&file,path,FsOpenMode_Read)))return actual;
        ON_SCOPE_EXIT {fsFileClose(&file);};
        if(mitm::fs::HasSdFile("/atmosphere/usb_badges/disabled"))return actual;
        s64 length{};u64 got{};nexus_badge::Header h{};
        if(R_FAILED(fsFileGetSize(&file,&length)) || length<static_cast<s64>(sizeof(h)) ||
            R_FAILED(fsFileRead(&file,0,&h,sizeof(h),FsReadOption_None,&got)) || got!=sizeof(h) ||
            !nexus_badge::Valid(h,id.value,static_cast<size_t>(length)) ||
            !nexus_badge::Fits(actual,buffer.GetSize(),h.jpeg_size) || h.original_size!=actual-nexus_badge::NacpSize ||
            h.dimension!=(variant?174u:256u))return actual;
        auto* icon=static_cast<u8*>(buffer.GetPointer())+nexus_badge::NacpSize;
        u8 digest[32];crypto::GenerateSha256Hash(digest,sizeof(digest),icon,h.original_size);
        if(std::memcmp(digest,h.original_hash,32)!=0)return actual;
        if(R_FAILED(fsFileRead(&file,sizeof(h),g_badge_jpeg,h.jpeg_size,FsReadOption_None,&got)) || got!=h.jpeg_size)return actual;
        crypto::GenerateSha256Hash(digest,sizeof(digest),g_badge_jpeg,h.jpeg_size);
        if(std::memcmp(digest,h.jpeg_hash,32)!=0 || !nexus_badge::Jpeg(g_badge_jpeg,h.jpeg_size,h.dimension))return actual;
        // Mutation starts only after every read/hash/bounds check succeeded.
        std::memcpy(icon,g_badge_jpeg,h.jpeg_size);
        if(h.jpeg_size<h.original_size)std::memset(icon+h.jpeg_size,0,h.original_size-h.jpeg_size);
        return nexus_badge::NacpSize+h.jpeg_size;
    }
    struct Request {u8 source,flag,index,pad[5];u64 id;};
    static_assert(sizeof(Request)==16);
}
Result NsControlDataTraceMitmService::GetBadgeControlData(u8 source,ncm::ProgramId id,const sf::OutBuffer &buffer,sf::Out<u32> size) {
    const Request in{source,0,0,{},id.value};u32 out{};
    const auto call=handoff::Next();handoff::Record(call,id,0,0,"control-send");
    const Result rc=serviceDispatchInOut(m_forward_service.get(),0,in,out,
        .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_Out},.buffers={{buffer.GetPointer(),buffer.GetSize()}});
    handoff::Record(call,id,0,0,"control-return",rc);R_TRY(rc);
    handoff::Record(call,id,0,0,"badge-begin");
    size.SetValue(Apply(m_client_info.program_id,id,buffer,out,0));
    handoff::Record(call,id,0,0,"badge-end");R_SUCCEED();
}
Result NsControlDataTraceMitmService::GetBadgeControlData2(u8 source,u8 flag,u8 index,ncm::ProgramId id,const sf::OutBuffer &buffer,sf::Out<u64> reply) {
    const Request in{source,flag,index,{},id.value};u64 out{};
    const auto call=handoff::Next();handoff::Record(call,id,6,0,"control-send");
    const Result rc=serviceDispatchInOut(m_forward_service.get(),6,in,out,
        .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_Out},.buffers={{buffer.GetPointer(),buffer.GetSize()}});
    handoff::Record(call,id,6,0,"control-return",rc);R_TRY(rc);
    handoff::Record(call,id,6,0,"badge-begin");
    // Keep all native lower-word flags. Only the documented upper size changes.
    const u32 size=index==0?Apply(m_client_info.program_id,id,buffer,out>>32,flag):static_cast<u32>(out>>32);
    reply.SetValue((out&0xffffffffULL)|(static_cast<u64>(size)<<32));
    handoff::Record(call,id,6,0,"badge-end");R_SUCCEED();
}
Result NsControlDataTraceMitmService::GetBadgeControlData3(u8 source,u8 flag,ncm::ProgramId id,const sf::OutBuffer &buffer,sf::Out<BadgeControlReply> reply) {
    const Request in{source,flag,0,{},id.value};BadgeControlReply out{};
    const auto call=handoff::Next();handoff::Record(call,id,19,0,"control-send");
    const Result rc=serviceDispatchInOut(m_forward_service.get(),19,in,out,
        .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_Out},.buffers={{buffer.GetPointer(),buffer.GetSize()}});
    handoff::Record(call,id,19,0,"control-return",rc);R_TRY(rc);
    handoff::Record(call,id,19,0,"badge-begin");
    out.size=Apply(m_client_info.program_id,id,buffer,out.size,flag);
    reply.SetValue(out);handoff::Record(call,id,19,0,"badge-end");R_SUCCEED();
}
}
