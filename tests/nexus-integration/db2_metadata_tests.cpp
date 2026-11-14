#include <cstdio>
#include <cstdlib>
#include <random>
#include "ncm_paged_content_meta_database_impl.hpp"
#include "ncm_content_meta_database_impl.hpp"

using namespace ams;
using namespace ams::ncm;
namespace {
size_t checks=0;
void Check(bool value,const char*what){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",what);std::abort();}}
using Bytes=std::vector<u8>;
ContentId Id(u32 n){ContentId id{};std::memcpy(id.data,&n,sizeof(n));return id;}
ContentMetaKey Key(u64 id,u32 version,ContentMetaType type,ContentInstallType install=ContentInstallType::Full){return {id,version,type,install,{}};}
Bytes Fixture(ContentMetaType type,u64 owner,bool legacy=false,size_t history=0,size_t extra_infos=0){
    size_t ext=type==ContentMetaType::Application?16:type==ContentMetaType::Patch?24:type==ContentMetaType::AddOnContent?(legacy?16:24):type==ContentMetaType::Delta?16:0;
    ContentMetaHeader header{u16(ext),u16(6+extra_infos),2,5,ContentMetaPlatform::Nx};
    Bytes bytes(sizeof(header)+ext+header.content_count*sizeof(ContentInfo)+2*sizeof(ContentMetaInfo)+history);
    std::memcpy(bytes.data(),&header,sizeof(header));
    if(ext){std::memcpy(bytes.data()+8,&owner,8);u32 system=123,app=456;std::memcpy(bytes.data()+16,&system,4);std::memcpy(bytes.data()+20,&app,4);}
    if(type==ContentMetaType::AddOnContent&&!legacy){u64 patch=999;std::memcpy(bytes.data()+24,&patch,8);bytes[20]=3;}
    std::vector<ContentInfo> infos(header.content_count);
    infos[0]={Id(11),100,0,ContentType::Meta,0};infos[1]={Id(22),100,0,ContentType::Control,9};
    infos[2]={Id(33),100,0,ContentType::Program,5};infos[3]={Id(44),100,0,ContentType::Program,2};
    infos[4]={Id(55),100,0,ContentType::Program,2};infos[5]={Id(66),100,0,ContentType::Control,1};
    for(size_t i=6;i<infos.size();++i)infos[i]={Id(1000+i),100,0,ContentType::Data,u8(i)};
    std::memcpy(bytes.data()+8+ext,infos.data(),infos.size()*sizeof(ContentInfo));
    ContentMetaInfo deps[2]{{owner+1,7,ContentMetaType::Application,1,{}},{owner+2,8,ContentMetaType::Patch,2,{}}};
    std::memcpy(bytes.data()+8+ext+infos.size()*sizeof(ContentInfo),deps,sizeof(deps));
    return bytes;
}
template<class T>bool Equal(const T&a,const T&b){return std::memcmp(&a,&b,sizeof(a))==0;}

struct Pair {
    kvdb::MemoryKeyValueStore<ContentMetaKey> memory;
    ContentMetaDatabaseImpl old{&memory,"db",StorageId::SdCard};
    PagedContentMetaDatabaseImpl paged{StorageId::SdCard};
    Pair(){Check(paged.Initialize("db:/","db",fs::SaveDataSpaceId::SdSystem,1)==0,"initialize");}
    void Put(ContentMetaKey key,const Bytes&bytes){Check(old.Set(key,{bytes.data(),bytes.size()})==0,"legacy set");Check(paged.Set(key,{bytes.data(),bytes.size()})==0,"paged set");}
};

void BasicAndDifferential(){
    Pair pair;
    std::vector<ContentMetaKey> keys{Key(100,1,ContentMetaType::Application),Key(100,2,ContentMetaType::Application),
      Key(100,3,ContentMetaType::Application,ContentInstallType::FragmentOnly),Key(101,9,ContentMetaType::Patch),
      Key(102,1,ContentMetaType::AddOnContent),Key(103,1,ContentMetaType::AddOnContent),Key(104,1,ContentMetaType::Delta),Key(5,1,ContentMetaType::SystemProgram)};
    for(size_t i=0;i<keys.size();++i)pair.Put(keys[i],Fixture(keys[i].type,100,i==4,i==3?1024*1024:0,i==3?600:0));
    u32 old_count=0,new_count=0;
    Check(pair.old.GetCount(&old_count)==pair.paged.GetCount(&new_count)&&old_count==new_count&&new_count==keys.size(),"count");
    PagedContentMetaStore::ResetFaults();
    for(const auto&key:keys){
        u64 old_size=0,new_size=0;
        Check(pair.old.GetSize(&old_size,key)==pair.paged.GetSize(&new_size,key)&&old_size==new_size,"get size");
        for(size_t capacity:{size_t(0),size_t(1),size_t(8),size_t(73),size_t(500)}){
            std::vector<u8>a(capacity),b(capacity);u64 n1=0,n2=0;
            Check(pair.old.Get(&n1,key,{a.data(),a.size()})==pair.paged.Get(&n2,key,{b.data(),b.size()})&&n1==n2&&a==b,"raw short get");
        }
        for(s32 offset:{-1,0,1,5,6,7,500,10000}){
            std::array<ContentInfo,4>a{},b{};s32 n1=0,n2=0;
            auto r1=pair.old.ListContentInfo(&n1,{a.data(),a.size()},key,offset),r2=pair.paged.ListContentInfo(&n2,{b.data(),b.size()},key,offset);
            Check(r1==r2&&(r1!=0||(n1==n2&&Equal(a,b))),"content list offsets");
            std::array<ContentMetaInfo,3>c{},d{};n1=n2=0;
            r1=pair.old.ListContentMetaInfo(&n1,{c.data(),c.size()},key,offset);r2=pair.paged.ListContentMetaInfo(&n2,{d.data(),d.size()},key,offset);
            Check(r1==r2&&(r1!=0||(n1==n2&&Equal(c,d))),"dependency list offsets");
        }
        for(auto type:{ContentType::Meta,ContentType::Program,ContentType::Control,ContentType::Data,ContentType::DeltaFragment}){
            ContentInfo a{},b{};auto r1=pair.old.GetContentInfoByType(&a,key,type),r2=pair.paged.GetContentInfoByType(&b,key,type);
            Check(r1==r2&&(r1!=0||Equal(a,b)),"typed minimum offset");
            for(u8 offset:{u8(0),u8(1),u8(2),u8(5),u8(9),u8(255)}){
                a=b={};r1=pair.old.GetContentInfoByTypeAndIdOffset(&a,key,type,offset);r2=pair.paged.GetContentInfoByTypeAndIdOffset(&b,key,type,offset);
                Check(r1==r2&&(r1!=0||Equal(a,b)),"exact offset");
            }
        }
        u8 a8=0,b8=0;Check(pair.old.GetAttributes(&a8,key)==pair.paged.GetAttributes(&b8,key)&&a8==b8,"attributes");
        ContentMetaPlatform aplat{},bplat{};Check(pair.old.GetPlatform(&aplat,key)==pair.paged.GetPlatform(&bplat,key)&&aplat==bplat,"platform");
        ApplicationId aowner{},bowner{};auto r1=pair.old.GetOwnerApplicationId(&aowner,key),r2=pair.paged.GetOwnerApplicationId(&bowner,key);
        Check(r1==r2&&(r1!=0||aowner==bowner),"owner");
        u32 a32=0,b32=0;r1=pair.old.GetRequiredSystemVersion(&a32,key);r2=pair.paged.GetRequiredSystemVersion(&b32,key);Check(r1==r2&&(r1!=0||a32==b32),"required firmware");
        u64 a64=0,b64=0;r1=pair.old.GetPatchContentMetaId(&a64,key);r2=pair.paged.GetPatchContentMetaId(&b64,key);Check(r1==r2&&(r1!=0||a64==b64),"patch id and legacy DLC");
        r1=pair.old.GetContentAccessibilities(&a8,key);r2=pair.paged.GetContentAccessibilities(&b8,key);Check(r1==r2&&(r1!=0||a8==b8),"DLC accessibility");
        if(key.type==ContentMetaType::Application||key.type==ContentMetaType::AddOnContent){r1=pair.old.GetRequiredApplicationVersion(&a32,key);r2=pair.paged.GetRequiredApplicationVersion(&b32,key);Check(r1==r2&&a32==b32,"required app version");}
        for(auto id:{Id(11),Id(44),Id(99999)}){bool a=false,b=false;r1=pair.old.HasContent(&a,key,id);r2=pair.paged.HasContent(&b,key,id);Check(r1==r2&&a==b,"has content");}
    }
    for(u64 id:{u64(0),u64(5),u64(99),u64(100),u64(101),UINT64_MAX}){
        ContentMetaKey a{},b{};auto r1=pair.old.GetLatestContentMetaKey(&a,id),r2=pair.paged.GetLatestContentMetaKey(&b,id);Check(r1==r2&&(r1!=0||a==b),"latest full only");
        auto key=ContentMetaKey::MakeUnknownType(id,0);ContentInfo c{},d{};r1=pair.old.GetContentInfoByType(&c,key,ContentType::Program);r2=pair.paged.GetContentInfoByType(&d,key,ContentType::Program);Check(r1==r2&&(r1!=0||Equal(c,d)),"lower-bound nonexact key");
    }
    for(size_t cap:{size_t(0),size_t(1),size_t(3),size_t(20)})for(auto type:{ContentMetaType::Unknown,ContentMetaType::Application,ContentMetaType::Patch,ContentMetaType::SystemProgram})for(u64 app:{u64(0),u64(100),u64(999)})for(auto install:{ContentInstallType::Unknown,ContentInstallType::Full,ContentInstallType::FragmentOnly})for(auto bounds:{std::pair<u64,u64>{0,UINT64_MAX},{100,100},{101,103},{103,101}}){
        std::vector<ContentMetaKey>a(cap),b(cap);s32 t1=0,t2=0,w1=0,w2=0;
        auto r1=pair.old.List(&t1,&w1,{a.data(),cap},type,{app},bounds.first,bounds.second,install),r2=pair.paged.List(&t2,&w2,{b.data(),cap},type,{app},bounds.first,bounds.second,install);
        Check(r1==r2&&t1==t2&&w1==w2&&a==b,"filtered list/count capacity");
    }
    for(size_t cap:{size_t(0),size_t(1),size_t(20)})for(auto type:{ContentMetaType::Unknown,ContentMetaType::Application,ContentMetaType::AddOnContent,ContentMetaType::SystemProgram}){
        std::vector<ApplicationContentMetaKey>a(cap),b(cap);s32 t1=0,t2=0,w1=0,w2=0;
        Check(pair.old.ListApplication(&t1,&w1,{a.data(),cap},type)==pair.paged.ListApplication(&t2,&w2,{b.data(),cap},type)&&t1==t2&&w1==w2&&(cap==0||std::memcmp(a.data(),b.data(),cap*sizeof(a[0]))==0),"list applications");
    }
    for(u8 mask:{u8(0),u8(1),u8(4),u8(7),u8(255)}){u8 a=0,b=0;Check(pair.old.HasAttributes(&a,mask)==pair.paged.HasAttributes(&b,mask)&&a==b,"combined attributes");}
    std::array<ContentId,4>ids{Id(11),Id(44),Id(99999),Id(11)};std::array<bool,5>a{},b{};
    Check(pair.old.LookupOrphanContent({a.data(),a.size()},{ids.data(),ids.size()})==pair.paged.LookupOrphanContent({b.data(),b.size()},{ids.data(),ids.size()})&&a==b,"orphan lookup including duplicate input semantics");
    auto padded=keys[0];padded.padding[0]=0xee;bool found=false;Check(pair.paged.Has(&found,padded)==0&&found,"key padding ignored");
    Check(pair.paged.HasAll(&found,{keys.data(),keys.size()})==0&&found,"has all");
    Check(pair.paged.Remove(keys[1])==pair.old.Remove(keys[1]),"remove");Check(pair.paged.Remove(keys[1])==pair.old.Remove(keys[1]),"remove missing");
}

void ErrorsAndBounds(){
    Pair pair;const auto key=Key(100,1,ContentMetaType::Patch);pair.Put(key,Fixture(key.type,100,false,1024*1024,600));
    PagedContentMetaStore::ResetFaults();
    ContentInfo info{};Check(pair.paged.GetContentInfoByType(&info,key,ContentType::Data)==0,"large content table");Check(PagedContentMetaStore::largest_read<=sizeof(ContentInfo),"range reads never load whole value");
    const int successful_reads=PagedContentMetaStore::read_calls;
    for(int fail=0;fail<successful_reads;++fail){PagedContentMetaStore::ResetFaults();PagedContentMetaStore::fail_read_at=fail;Check(pair.paged.GetContentInfoByType(&info,key,ContentType::Data)==905,"propagate every content read failure");}
    PagedContentMetaStore::ResetFaults();PagedContentMetaStore::fail_find=true;bool found=false;Check(pair.paged.Has(&found,key)==904,"has propagates I/O instead of missing");
    PagedContentMetaStore::ResetFaults();PagedContentMetaStore::fail_at_at=0;s32 total=0,written=0;Check(pair.paged.List(&total,&written,{nullptr,0},ContentMetaType::Unknown,{},0,UINT64_MAX,ContentInstallType::Unknown)==902,"list propagates index I/O");
    PagedContentMetaStore::ResetFaults();
    const auto valid=Fixture(ContentMetaType::Patch,100);
    for(size_t size=0;size<valid.size();++size){
        Bytes truncated(valid.begin(),valid.begin()+size);Check(pair.paged.Set(key,{truncated.data(),truncated.size()})==0,"store truncated fixture");
        u8 attrs{};const Result expected_header=size>=sizeof(ContentMetaHeader)?Result():ResultInvalidContentMetaFileSize();
        Check(pair.paged.GetAttributes(&attrs,key)==expected_header,"header query depends only on header bytes");
        ContentInfo content{};ContentMetaInfo dependency{};s32 written=0;
        const size_t content_end=sizeof(ContentMetaHeader)+24+sizeof(ContentInfo);
        const size_t dependency_end=sizeof(ContentMetaHeader)+24+6*sizeof(ContentInfo)+sizeof(ContentMetaInfo);
        Check(pair.paged.ListContentInfo(&written,{&content,1},key,0)==(size>=content_end?Result():ResultInvalidContentMetaFileSize()),"first content range checked independently");
        Check(pair.paged.ListContentMetaInfo(&written,{&dependency,1},key,0)==(size>=dependency_end?Result():ResultInvalidContentMetaFileSize()),"first dependency range checked independently");
    }
    Check(pair.paged.Set(key,{valid.data(),valid.size()})==0,"restore fixture");
    pair.paged.DisableForcibly();u64 size=0;Check(pair.paged.GetSize(&size,key)==ResultInvalidContentMetaDatabase(),"disabled get");Check(pair.paged.HasContent(&found,key,Id(11))==ResultInvalidContentMetaDatabase(),"disabled has content");
}

void LegacyForwardersAndRequestedRanges(){
    Pair pair;const auto key=Key(0x0100000012340000ULL,0,ContentMetaType::Application);
    struct LegacyForwarder{ContentMetaHeader header;ApplicationMetaExtendedHeader extended;ContentInfo infos[3];} legacy{};
    static_assert(sizeof(legacy)==0x60);
    legacy.header={16,3,1,5,ContentMetaPlatform::Nx};
    legacy.extended={{key.id|0x800},123,456};
    legacy.infos[0]={Id(11),100,0,ContentType::Meta,0};
    legacy.infos[1]={Id(22),100,0,ContentType::Program,0};
    legacy.infos[2]={Id(33),100,0,ContentType::Control,0};
    Bytes bytes(sizeof(legacy));std::memcpy(bytes.data(),&legacy,sizeof(legacy));pair.Put(key,bytes);
    s32 t1=0,t2=0,w1=0,w2=0;ApplicationContentMetaKey a[2]{},b[2]{};
    Check(pair.old.ListApplication(&t1,&w1,{a,2},ContentMetaType::Unknown)==pair.paged.ListApplication(&t2,&w2,{b,2},ContentMetaType::Unknown)&&t1==1&&t1==t2&&w1==w2&&Equal(a,b),"legacy nonexistent dependency does not block application listing");
    u8 attrs1=0,attrs2=0;Check(pair.old.GetAttributes(&attrs1,key)==pair.paged.GetAttributes(&attrs2,key)&&attrs1==attrs2,"legacy attributes");
    Check(pair.old.HasAttributes(&attrs1,7)==pair.paged.HasAttributes(&attrs2,7)&&attrs1==5&&attrs1==attrs2,"legacy HOME-screen attribute scan ignores absent dependency");
    ContentMetaPlatform platform1{},platform2{};Check(pair.old.GetPlatform(&platform1,key)==pair.paged.GetPlatform(&platform2,key)&&platform1==platform2,"legacy platform header query");
    ContentInfo info1{},info2{};Check(pair.old.GetContentInfoByType(&info1,key,ContentType::Program)==pair.paged.GetContentInfoByType(&info2,key,ContentType::Program)&&Equal(info1,info2),"legacy program lookup ignores absent dependency tail");
    std::array<ContentInfo,3> infos1{},infos2{};Check(pair.old.ListContentInfo(&w1,{infos1.data(),3},key,0)==pair.paged.ListContentInfo(&w2,{infos2.data(),3},key,0)&&w1==3&&w1==w2&&Equal(infos1,infos2),"legacy all three content records remain readable");
    u32 version1=0,version2=0;Check(pair.old.GetRequiredSystemVersion(&version1,key)==pair.paged.GetRequiredSystemVersion(&version2,key)&&version1==123&&version1==version2,"legacy requirement query");
    ContentMetaInfo dep{};Check(pair.paged.ListContentMetaInfo(&w2,{&dep,1},key,0)==ResultInvalidContentMetaFileSize(),"actually requested absent legacy dependency is rejected");
    Bytes raw(bytes.size());u64 raw_size=0;Check(pair.paged.Get(&raw_size,key,{raw.data(),raw.size()})==0&&raw==bytes&&raw_size==bytes.size(),"legacy raw record and declared count remain unchanged");

    for(auto type:{ContentMetaType::Application,ContentMetaType::Patch,ContentMetaType::AddOnContent}){
        const auto scalar_key=Key(1000+static_cast<u8>(type),1,type);
        const auto full=Fixture(type,777,type==ContentMetaType::AddOnContent);
        const size_t ext=type==ContentMetaType::Patch?24:16;
        for(size_t size=8;size<=8+ext;++size){
            Bytes prefix(full.begin(),full.begin()+size);Check(pair.paged.Set(scalar_key,{prefix.data(),prefix.size()})==0,"store partial extended header");
            u32 version{};u64 patch{};ApplicationId owner{};u8 access{};
            if(type!=ContentMetaType::AddOnContent)
                Check(pair.paged.GetRequiredSystemVersion(&version,scalar_key)==(size>=20?Result():ResultInvalidContentMetaFileSize()),"system version reads only requested four bytes");
            if(type!=ContentMetaType::Patch){
                const size_t end=type==ContentMetaType::Application?24:20;
                Check(pair.paged.GetRequiredApplicationVersion(&version,scalar_key)==(size>=end?Result():ResultInvalidContentMetaFileSize()),"application version reads only requested four bytes");
            }
            if(type==ContentMetaType::Application)
                Check(pair.paged.GetPatchContentMetaId(&patch,scalar_key)==(size>=16?Result():ResultInvalidContentMetaFileSize()),"patch id does not require later version fields");
            else
                Check(pair.paged.GetOwnerApplicationId(&owner,scalar_key)==(size>=16?Result():ResultInvalidContentMetaFileSize()),"owner reads only requested eight bytes");
            if(type==ContentMetaType::AddOnContent){
                Check(pair.paged.GetContentAccessibilities(&access,scalar_key)==(size>=21?Result():ResultInvalidContentMetaFileSize()),"accessibility reads only requested byte");
                Check(pair.paged.GetPatchContentMetaId(&patch,scalar_key)==ResultInvalidAddOnContentMetaExtendedHeader(),"legacy DLC retains exact extended header requirement for patch id");
            }
        }
    }
    const auto dlc_key=Key(2000,1,ContentMetaType::AddOnContent);const auto full_dlc=Fixture(ContentMetaType::AddOnContent,777);
    for(size_t size=8;size<=32;++size){Bytes prefix(full_dlc.begin(),full_dlc.begin()+size);Check(pair.paged.Set(dlc_key,{prefix.data(),prefix.size()})==0,"store current DLC prefix");u64 patch{};Check(pair.paged.GetPatchContentMetaId(&patch,dlc_key)==(size>=32?Result():ResultInvalidContentMetaFileSize()),"current DLC requested data-patch id range checked");}

    Bytes short_declared=bytes;ContentMetaHeader short_header=legacy.header;short_header.extended_header_size=8;std::memcpy(short_declared.data(),&short_header,sizeof(short_header));
    Check(pair.paged.Set(key,{short_declared.data(),short_declared.size()})==0,"store undersized declared extension");
    Check(pair.paged.GetRequiredSystemVersion(&version2,key)==ResultInvalidContentMetaFileSize(),"stored later bytes do not bypass declared extension bounds");
    pair.Put(key,bytes);
    for(int fail:{0,1}){PagedContentMetaStore::ResetFaults();PagedContentMetaStore::fail_read_at=fail;Check(pair.paged.GetRequiredSystemVersion(&version2,key)==905,"header and scalar I/O failures preserved");}
    PagedContentMetaStore::ResetFaults();

    Pair key_only;const auto empty_key=Key(3000,1,ContentMetaType::Application);const auto system_key=Key(3001,1,ContentMetaType::SystemProgram);
    key_only.Put(empty_key,{});key_only.Put(system_key,{});
    Check(key_only.old.ListApplication(&t1,&w1,{a,2},ContentMetaType::Unknown)==key_only.paged.ListApplication(&t2,&w2,{b,2},ContentMetaType::Unknown)&&t1==t2&&w1==w2&&Equal(a,b),"application key and nonowner filtering need no record bytes");
    Check(key_only.paged.GetAttributes(&attrs2,empty_key)==ResultInvalidContentMetaFileSize(),"missing requested header still rejected");
}

void CommitMapping(){
    usb_enabled=true;usb_install=true;usb_ready=true;mappings=unmaps=0;fs::fail_commit=false;
    PagedContentMetaDatabaseImpl service(StorageId::SdCard);Check(service.Initialize("db:/","db",fs::SaveDataSpaceId::SdSystem,1)==0,"mapping initialize");
    auto bytes=Fixture(ContentMetaType::Application,100);auto key=Key(100,1,ContentMetaType::Application);
    Check(service.Set(key,{bytes.data(),bytes.size()})==0,"queue mapping");fs::fail_commit=true;Check(service.Commit()==900&&mappings==0,"mapping never precedes durable commit");
    fs::fail_commit=false;Check(service.Commit()==0&&mappings==1&&mapped_title==100&&mapped_meta==Id(11)&&mapped_control==Id(66)&&mapped_program==Id(44),"streamed mapping matches validator lowest offsets");
    Check(service.Commit()==0&&mappings==1,"mapping queue drained");
    for(u64 id=200;id<232;++id)Check(service.Set(Key(id,1,ContentMetaType::Application),{bytes.data(),bytes.size()})==0,"existing queue capacity");
    Check(service.Set(Key(999,1,ContentMetaType::Application),{bytes.data(),bytes.size()})==ResultBufferInsufficient(),"queue capacity rejected before mutation");bool found=true;Check(service.Has(&found,Key(999,1,ContentMetaType::Application))==0&&!found,"failed queue did not store record");
    Check(service.Commit()==0&&mappings==33,"batched mapping commit");
    usb_install=false;usb_ready=false;Check(service.Set(key,{bytes.data(),bytes.size()})==0,"queue SD unmap");Check(service.Commit()==0&&unmaps==1,"SD mapping removed after commit");
    usb_enabled=usb_install=usb_ready=false;
}
}
int main(){BasicAndDifferential();ErrorsAndBounds();LegacyForwardersAndRequestedRanges();CommitMapping();std::printf("PASS %zu metadata checks: production paged service differential API semantics, bounded range reads, large records, all read failures, legacy forwarders, requested-field bounds and durable USB mapping order.\n",checks);}
