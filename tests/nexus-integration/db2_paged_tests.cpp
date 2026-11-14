#include <stratosphere/ncm/ncm_paged_content_meta_store.hpp>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <stdexcept>

using namespace ams;
namespace {
using Store=ncm::PagedContentMetaStore;
using Key=ncm::ContentMetaKey;
using Bytes=std::vector<u8>;
using Model=std::map<Key,Bytes>;
using Files=std::map<std::string,Bytes>;
constexpr const char* Root="db:/meta";
constexpr const char* Archive="db:/meta/imkvdb.arc";
constexpr Result IoFailure=0x1234;
size_t checks=0;
void Check(bool yes,const char* message){++checks;if(!yes){std::fprintf(stderr,"FAIL %zu: %s\n",checks,message);std::exit(1);}}
Files files,durable;
std::map<std::string,size_t> calls;
std::string fail_operation;
size_t fail_nth=0,steps=0,cut_at=0;
size_t max_read=0,max_write=0,open_handles=0,peak_handles=0;
u64 quota=0,journal=0,device_limit=512*1024*1024,commit_id=0;
bool mounted=true,partial_write=false,fail_commit_after=false,short_read=false,fail_restoration=false;
std::string current_operation;
Bytes allowed_canonical;
struct Handle {std::string path;int mode;};
std::map<void*,Handle*> handles;
struct PowerCut {};
bool Fault(const char* operation){++steps;const auto nth=++calls[operation];if(cut_at&&steps==cut_at)throw PowerCut{};return fail_operation==operation&&(nth==fail_nth||(fail_restoration&&nth==fail_nth+1));}
void ClearFault(const char* operation="",size_t nth=0){calls.clear();fail_operation=operation;fail_nth=nth;steps=0;cut_at=0;max_read=max_write=0;partial_write=false;fail_commit_after=false;short_read=false;fail_restoration=false;}
u64 Used(){u64 total=0;for(const auto& [p,b]:files){static_cast<void>(p);total+=b.size();}return total;}
bool IsOpen(const char* path){for(const auto& [p,h]:handles){static_cast<void>(p);if(h->path==path)return true;}return false;}
Key K(u32 n,u32 version=0){return {0x0100000000000000ULL+static_cast<u64>(n)*0x1000,version,ncm::ContentMetaType::Application,ncm::ContentInstallType::Full,{}};}
Bytes Pattern(size_t n,u32 seed){Bytes b(n);for(size_t i=0;i<n;++i)b[i]=static_cast<u8>((i*37+seed*17)&255);return b;}
void Append(Bytes& out,const void* data,size_t size){const auto* bytes=static_cast<const u8*>(data);out.insert(out.end(),bytes,bytes+size);}
void U32(Bytes& out,u32 value){Append(out,&value,4);}
Bytes Serialize(const Model& model){Bytes out;Append(out,"IMKV",4);U32(out,0);U32(out,static_cast<u32>(model.size()));for(const auto& [key,value]:model){Append(out,"IMEN",4);U32(out,sizeof(key));U32(out,static_cast<u32>(value.size()));Append(out,&key,sizeof(key));if(!value.empty())Append(out,value.data(),value.size());}return out;}
Model Small(){return {{K(1),Pattern(52,1)},{K(4),Pattern(63,4)},{K(7),Pattern(29,7)},{K(9),Pattern(48,9)}};}
void Setup(const Model& model,u64 size=4*1024*1024){Check(open_handles==0,"reset has no handles");files.clear();durable.clear();files[Archive]=Serialize(model);durable=files;quota=std::max<u64>(size,files[Archive].size()+1024);journal=quota;mounted=true;commit_id=0;device_limit=512*1024*1024;current_operation.clear();allowed_canonical=files[Archive];ClearFault();}
void Reboot(){Check(open_handles==0,"reboot has no live handles");files=durable;mounted=true;ClearFault();current_operation.clear();allowed_canonical=durable[Archive];}
/* Abrupt termination does not run the production's explicit close statements;
 * the OS releases process handles. Normal Result failures still require the
 * production implementation to close every handle itself. */
void TerminateProcess(){for(auto& [pointer,handle]:handles){static_cast<void>(pointer);delete handle;}handles.clear();open_handles=0;}
void Init(Store& store){Check(store.Initialize(Root,"db",fs::SaveDataSpaceId::SdSystem,0x8000000000000124ULL)==0,"store initialize");Check(store.IsAvailable(),"initialized availability");}
void Verify(Store& store,const Model& model,bool check_find=true){
    Check(store.GetCount()==model.size(),"record count matches model");u32 index=0;
    for(const auto& [key,value]:model){Store::RecordRef ref{};Check(store.At(&ref,index)==0,"At succeeds");Check(ref.key==key&&ref.size==value.size(),"record identity and size");
        Bytes actual(value.size());Check(store.Read(ref,0,actual.data(),actual.size())==0,"read whole value");Check(actual==value,"value bytes match model");
        if(check_find){Store::RecordRef found{};bool has=false;Check(store.Find(&found,&has,key)==0&&has,"Find matches existing");Check(found.key==key&&found.size==value.size(),"found ref matches");u32 pos=UINT32_MAX;Check(store.LowerBound(&pos,key)==0&&pos==index,"lower_bound exact");}
        ++index;
    }
    Store::RecordRef missing{};bool found=true;Check(store.Find(&missing,&found,K(0))==0&&!found,"missing lookup");
    Check(store.At(&missing,static_cast<u32>(model.size()))!=0,"out of range At fails");
}
void CloseAndVerifyDurable(const Model& expected){Reboot();Store check;Init(check);Verify(check,expected);}

void BasicTests(){
    {Store fresh;Store::RecordRef ref{};bool found=true;u8 value{};Check(!fresh.IsAvailable()&&fresh.GetCount()==0,"default store unavailable and empty");Check(fresh.Find(&ref,&found,K(1))!=0,"uninitialized Find fails");Check(fresh.Read(ref,0,&value,1)!=0,"uninitialized Read fails");Check(fresh.Set(K(1),&value,1)!=0,"uninitialized Set fails");Check(fresh.Remove(K(1))!=0&&fresh.Commit()!=0,"uninitialized mutations and commit fail");Check(open_handles==0,"uninitialized calls open no handles");}
    const auto baseline=Small();Setup(baseline);
    {Store store;Init(store);Verify(store,baseline);Store::RecordRef stale{};Check(store.At(&stale,0)==0,"retain copied ref");
        auto updated=baseline;auto value=Pattern(8000,2);Check(store.Set(K(2),value.data(),value.size())==0,"insert between disk keys");updated[K(2)]=value;Verify(store,updated);
        u8 b{};Check(store.Read(stale,0,&b,1)!=0,"old generation ref rejected after mutation");
        value=Pattern(11,4);Check(store.Set(K(4),value.data(),value.size())==0,"replace value");updated[K(4)]=value;
        Check(store.Remove(K(7))==0,"remove existing");updated.erase(K(7));Check(store.Remove(K(7))!=0,"missing remove error");Verify(store,updated);
        Check(durable[Archive]==Serialize(baseline),"mutations do not publish canonical");current_operation="Commit";Check(store.Commit()==0,"commit succeeds");current_operation.clear();allowed_canonical=Serialize(updated);Check(durable[Archive]==allowed_canonical,"committed archive exact legacy bytes");Verify(store,updated);
    }
    Check(open_handles==0,"basic handles closed");
    {auto updated=baseline;updated[K(2)]=Pattern(8000,2);updated[K(4)]=Pattern(11,4);updated.erase(K(7));CloseAndVerifyDurable(updated);}
    Check(open_handles==0,"reopen handles closed");
    Setup({});{Store store;Init(store);Check(store.GetCount()==0,"empty canonical");Check(store.Set(K(1),nullptr,0)==0,"zero size value");Verify(store,{{K(1),{}}});current_operation="Commit";Check(store.Commit()==0,"empty-value commit");current_operation.clear();}
    Setup({});files.erase(Archive);durable=files;{Store store;Init(store);Check(store.GetCount()==0,"new save without canonical initializes empty");current_operation="Commit";Check(store.Commit()==0,"first commit creates stock empty archive");current_operation.clear();Check(durable.at(Archive)==Serialize({}),"new database canonical is valid empty IMKV");}
}

void LargeTests(){
    Model large;for(u32 i=1;i<=20000;++i)large[K(i)]=Pattern(160,i);large[K(40000)]=Pattern(3*1024*1024+37,23);
    Setup(large,8*1024*1024);Check(files[Archive].size()>2*1024*1024+512*1024,"large fixture exceeds old resident pool");
    static_assert(sizeof(Store)<32*1024,"RAM must stay bounded independent of record count");
    {Store store;Init(store);Check(store.GetCount()==20001,"more than old 8192 record cap");Verify(store,large,false);
        Store::RecordRef ref{};bool found=false;Check(store.Find(&ref,&found,K(40000))==0&&found,"find multi-MiB record");
        for(u64 offset:{u64{0},u64{4093},u64{65535},u64{2*1024*1024}}){Bytes bytes(8197);Check(store.Read(ref,offset,bytes.data(),bytes.size())==0,"large range read");Check(std::equal(bytes.begin(),bytes.end(),large[K(40000)].begin()+offset),"large range bytes");}
        u8 byte{};Check(store.Read(ref,ref.size,&byte,1)!=0,"range past end rejected");Check(store.Read(ref,UINT64_MAX,&byte,1)!=0,"overflow range rejected");
        auto update=Pattern(211,333);Check(store.Set(K(17000),update.data(),update.size())==0,"update above old index cap");large[K(17000)]=update;
        current_operation="Commit";Check(store.Commit()==0,"large commit");current_operation.clear();allowed_canonical=Serialize(large);Check(durable[Archive]==allowed_canonical,"large exact legacy-compatible archive");
        Check(max_read<=Store::PageSize&&max_write<=Store::PageSize,"all direct IO bounded to page size");
    }
    CloseAndVerifyDurable(large);
    std::printf("Large fixture: %zu records, %zu archive bytes, store object %zu bytes, cache %zu + work %zu bytes.\n",large.size(),Serialize(large).size(),sizeof(Store),Store::CacheBytes,Store::WorkingBufferBytes);
}

void GrowthTests(){
    const auto baseline=Small();Setup(baseline,96*1024);Model pending=baseline;
    {Store store;Init(store);current_operation="Set";auto first=Pattern(120*1024,50);Check(store.Set(K(50),first.data(),first.size())==0,"grow at first large mutation");pending[K(50)]=first;
        auto second=Pattern(2*1024*1024,51);Check(store.Set(K(51),second.data(),second.size())==0,"grow while pending transaction exists");pending[K(51)]=second;
        current_operation.clear();Check(calls["extend"]>0,"actual Extend requested");Check(calls["unmount"]>0,"growth remount performed");Check(durable[Archive]==Serialize(baseline),"scratch growth checkpoints never publish pending records");Verify(store,pending);
    }
    CloseAndVerifyDurable(baseline);
    Setup(baseline,96*1024);{Store store;Init(store);current_operation="Set";const auto big=Pattern(700*1024,50);Check(store.Set(K(50),big.data(),big.size())==0,"growth before commit");pending=baseline;pending[K(50)]=big;current_operation="Commit";Check(store.Commit()==0,"commit after grow");current_operation.clear();}
    CloseAndVerifyDurable(pending);
}

template<class Operation>std::map<std::string,size_t> CountOperation(const Model& baseline,Operation operation){Setup(baseline);Store store;Init(store);ClearFault();operation(store);return calls;}

void MutationFaultTests(){
    const auto baseline=Small();const auto value=Pattern(9000,2);
    const auto measured=CountOperation(baseline,[&](Store& s){Check(s.Set(K(2),value.data(),value.size())==0,"measure Set");});
    for(const char* op:{"open","create","delete","resize","read","write","flush"}){
        const auto count=measured.count(op)?measured.at(op):0;
        for(size_t nth=1;nth<=count;++nth){Setup(baseline);{Store store;Init(store);ClearFault(op,nth);partial_write=true;const auto rc=store.Set(K(2),value.data(),value.size());ClearFault();
            if(store.IsAvailable()){auto expected=baseline;if(rc==0)expected[K(2)]=value;Verify(store,expected);}Check(durable[Archive]==Serialize(baseline),"failed Set leaves durable canonical");}
            CloseAndVerifyDurable(baseline);
        }
    }
    const auto removes=CountOperation(baseline,[&](Store& s){Check(s.Remove(K(4))==0,"measure Remove");});
    for(const char* op:{"open","create","delete","read","write","flush"}){
        const auto count=removes.count(op)?removes.at(op):0;
        for(size_t nth=1;nth<=count;++nth){Setup(baseline);{Store store;Init(store);ClearFault(op,nth);partial_write=true;const auto rc=store.Remove(K(4));ClearFault();if(store.IsAvailable()){auto expected=baseline;if(rc==0)expected.erase(K(4));Verify(store,expected);}Check(durable[Archive]==Serialize(baseline),"failed Remove leaves durable canonical");}CloseAndVerifyDurable(baseline);}
    }
}

Model Stage(Store& store,const Model& baseline){auto pending=baseline;const auto value=Pattern(6000,2);Check(store.Set(K(2),value.data(),value.size())==0,"stage new key");pending[K(2)]=value;Check(store.Remove(K(7))==0,"stage remove");pending.erase(K(7));return pending;}
void CommitFaultTests(){
    const auto baseline=Small();std::map<std::string,size_t> measured;
    Setup(baseline);{Store s;Init(s);Stage(s,baseline);ClearFault();current_operation="Commit";Check(s.Commit()==0,"measure Commit");measured=calls;current_operation.clear();}
    for(const char* op:{"open","create","delete","read","write","flush","rename","commit"}){
        const auto count=measured.count(op)?measured.at(op):0;
        for(size_t nth=1;nth<=count;++nth){Setup(baseline);Model pending;{Store store;Init(store);pending=Stage(store,baseline);ClearFault(op,nth);partial_write=true;current_operation="Commit";const auto rc=store.Commit();current_operation.clear();ClearFault();
            Check(durable[Archive]==Serialize(baseline)||durable[Archive]==Serialize(pending),"failed Commit has whole old/new durable archive");
            if(store.IsAvailable()){Verify(store,pending);current_operation="Commit";Check(store.Commit()==0,"retry commit after injected failure");current_operation.clear();Check(durable[Archive]==Serialize(pending),"retry publishes full pending state");}
            static_cast<void>(rc);
        }const auto expected=durable[Archive]==Serialize(pending)?pending:baseline;CloseAndVerifyDurable(expected);}
    }
    Setup(baseline);Model pending;{Store store;Init(store);pending=Stage(store,baseline);ClearFault("commit",1);fail_commit_after=true;current_operation="Commit";Check(store.Commit()!=0,"ambiguous commit reports error");current_operation.clear();ClearFault();if(store.IsAvailable()){Verify(store,pending);current_operation="Commit";Check(store.Commit()==0,"retry ambiguous commit");current_operation.clear();}}
    CloseAndVerifyDurable(pending);
    Setup(baseline);{Store s;Init(s);Stage(s,baseline);ClearFault("rename",2);fail_restoration=true;current_operation="Commit";Check(s.Commit()!=0,"second rename and restore both fail");current_operation.clear();ClearFault();Check(!s.IsAvailable(),"failed rename restoration invalidates backend");Check(!files.count(Archive)&&files.at("db:/meta/db2-archive.prev")==Serialize(baseline),"failed restoration retains old generation for recovery");Check(durable.at(Archive)==Serialize(baseline),"failed restoration does not commit broken namespace");}CloseAndVerifyDurable(baseline);
}

void PowerLossTests(){
    const auto baseline=Small();size_t commit_steps=0;Setup(baseline);{Store s;Init(s);Stage(s,baseline);ClearFault();current_operation="Commit";Check(s.Commit()==0,"measure powercut commit steps");commit_steps=steps;current_operation.clear();}
    for(size_t cut=1;cut<=commit_steps;++cut){Setup(baseline);Model pending;try{Store s;Init(s);pending=Stage(s,baseline);ClearFault();cut_at=cut;current_operation="Commit";s.Commit();}catch(const PowerCut&){TerminateProcess();}current_operation.clear();cut_at=0;Check(open_handles==0,"powercut process handles released");Check(durable[Archive]==Serialize(baseline)||durable[Archive]==Serialize(pending),"restart sees old or complete committed database");const auto expected=durable[Archive]==Serialize(pending)?pending:baseline;CloseAndVerifyDurable(expected);}
    std::printf("Power-loss model checked %zu explicit-commit filesystem boundaries.\n",commit_steps);
}

void GrowthFaultTests(){
    const auto baseline=Small();const auto first=Pattern(2048,45);const auto growing=Pattern(120*1024,46);
    auto stage=[&](Store& store){auto model=baseline;Check(store.Set(K(45),first.data(),first.size())==0,"stage before dirty growth");model[K(45)]=first;return model;};
    std::map<std::string,size_t> measured;size_t growth_steps=0;
    Setup(baseline,96*1024);{Store s;Init(s);stage(s);ClearFault();current_operation="Set";Check(s.Set(K(46),growing.data(),growing.size())==0,"measure dirty growth");measured=calls;growth_steps=steps;current_operation.clear();}
    size_t injected=0;
    for(const char* op:{"free","extra","commit","extend","mount","open","create","delete","read","write","resize","flush"}){
        const auto count=measured.count(op)?measured.at(op):0;
        for(size_t nth=1;nth<=count;++nth){++injected;Setup(baseline,96*1024);{Store s;Init(s);auto expected=stage(s);ClearFault(op,nth);partial_write=true;current_operation="Set";const auto result=s.Set(K(46),growing.data(),growing.size());current_operation.clear();ClearFault();if(result==0)expected[K(46)]=growing;Check(s.IsMounted()==mounted,"backend mount state matches actual post-failure mount");if(std::string(op)=="mount")Check(!s.IsAvailable()&&!s.IsMounted(),"failed remount invalidates and reports absent mount");if(s.IsAvailable())Verify(s,expected);Check(durable[Archive]==Serialize(baseline),"failed grow cannot publish pending state");}CloseAndVerifyDurable(baseline);}
    }
    for(size_t cut=1;cut<=growth_steps;++cut){Setup(baseline,96*1024);try{Store s;Init(s);stage(s);ClearFault();cut_at=cut;current_operation="Set";s.Set(K(46),growing.data(),growing.size());}catch(const PowerCut&){TerminateProcess();}cut_at=0;current_operation.clear();Check(durable[Archive]==Serialize(baseline),"power cut during growth never publishes pending state");CloseAndVerifyDurable(baseline);}
    Setup(baseline,96*1024);{Store s;Init(s);const auto expected=stage(s);device_limit=quota+journal;current_operation="Set";Check(s.Set(K(46),growing.data(),growing.size())!=0,"finite device full surfaces error");current_operation.clear();Check(s.IsAvailable(),"failed extension remounts previous live database");Verify(s,expected);Check(durable[Archive]==Serialize(baseline),"device full cannot publish pending state");}CloseAndVerifyDurable(baseline);
    Setup(baseline,96*1024);{Store s;Init(s);const auto expected=stage(s);ClearFault("commit",1);fail_commit_after=true;current_operation="Set";Check(s.Set(K(46),growing.data(),growing.size())!=0,"ambiguous scratch checkpoint failure surfaces error");current_operation.clear();ClearFault();Verify(s,expected);Check(durable[Archive]==Serialize(baseline),"ambiguous scratch checkpoint leaves canonical unchanged");}CloseAndVerifyDurable(baseline);
    Model dense;for(u32 i=1;i<=10000;++i)dense[K(i)]={};Setup(dense,1);{Store s;Init(s);Check(calls["extend"]>0,"first import extends when index exceeds legacy quota");Check(s.IsMounted(),"first import growth remains mounted");Verify(s,dense,false);Check(durable[Archive]==Serialize(dense),"first import extension preserves canonical bytes");}
    std::printf("Dirty growth: %zu injected Result failures and %zu power-loss boundaries; finite-device-full and ambiguous checkpoint checked.\n",injected,growth_steps);
}

void PreparedCommitTests(){
    const auto baseline=Small();Setup(baseline);Model pending;
    {Store s;Init(s);pending=Stage(s,baseline);ClearFault("commit",1);current_operation="Commit";Check(s.Commit()!=0,"explicit commit failure fixture");current_operation.clear();ClearFault();Verify(s,pending);
        const auto too_big=Pattern(5*1024*1024,88);ClearFault();Check(s.Set(K(88),too_big.data(),too_big.size())!=0,"pending failed commit blocks growing Set");Check(s.Remove(K(1))!=0,"pending failed commit blocks Remove");Check(calls["commit"]==0&&calls["extend"]==0,"blocked mutations cannot checkpoint a prepared commit");Verify(s,pending);Check(durable[Archive]==Serialize(baseline),"failed commit remains unpublished before retry");current_operation="Commit";Check(s.Commit()==0,"explicit retry completes prepared commit");current_operation.clear();
    }CloseAndVerifyDurable(pending);
}

void MalformedAndIoTests(){
    const auto baseline=Small();const auto valid=Serialize(baseline);
    for(size_t length=0;length<valid.size();++length){Setup(baseline);files[Archive].resize(length);durable=files;Store s;Check(s.Initialize(Root,"db",fs::SaveDataSpaceId::SdSystem,0x8000000000000124ULL)!=0,"truncated archive rejected");}
    Setup(baseline);{files[Archive].push_back(0xCC);durable=files;Store s;Init(s);Verify(s,baseline);}
    Setup(baseline);{Store s;Init(s);Store::RecordRef r{};Check(s.At(&r,0)==0,"range fixture ref");ClearFault("read",1);Bytes b(9000);const auto rc=s.Read(r,0,b.data(),b.size());Check(rc!=0,"oversized range rejects");}
    Setup(baseline);{Store s;Init(s);Store::RecordRef ref{};Check(s.At(&ref,0)==0,"invalidation ref");s.Invalidate();u8 byte{};Check(!s.IsAvailable()&&s.Read(ref,0,&byte,1)!=0,"invalidated reads fail");Check(s.Set(K(99),&byte,1)!=0,"invalidated writes fail");}
    Setup(baseline);{files["db:/meta/db2-archive.prev"]=files[Archive];files.erase(Archive);durable=files;Store s;Check(s.Initialize(Root,"db",fs::SaveDataSpaceId::SdSystem,0x8000000000000124ULL)!=0,"missing canonical plus prior generation fails closed");Check(files.at("db:/meta/db2-archive.prev")==valid,"previous generation retained for recovery");Check(!files.count(Archive),"failed initialization does not fabricate empty canonical");}
    for(size_t position:{size_t{0},size_t{12},size_t{16},size_t{24+sizeof(Key)+52}}){Setup(baseline);files[Archive][position]^=0xFF;durable=files;Store s;Check(s.Initialize(Root,"db",fs::SaveDataSpaceId::SdSystem,0x8000000000000124ULL)!=0,"invalid magic or key width rejected");}
    Setup(baseline,96*1024);{const u32 impossible=0x7FFFFFFF;std::memcpy(files[Archive].data()+8,&impossible,4);durable=files;Store s;Check(s.Initialize(Root,"db",fs::SaveDataSpaceId::SdSystem,0x8000000000000124ULL)!=0,"forged enormous count rejected");Check(calls["extend"]==0,"forged count rejected before capacity extension");}
    for(bool duplicate:{false,true}){Setup(baseline);const auto first=K(1),second=duplicate?K(1):K(0);std::memcpy(files[Archive].data()+24,&first,16);std::memcpy(files[Archive].data()+12+28+52+12,&second,16);durable=files;Store s;Check(s.Initialize(Root,"db",fs::SaveDataSpaceId::SdSystem,0x8000000000000124ULL)!=0,"duplicate or unsorted keys rejected");}
    Setup(baseline);{Store s;Init(s);Store::RecordRef ref{};ClearFault("read",1);short_read=true;Check(s.At(&ref,0)!=0,"successful short index-page read fails");ClearFault();Check(s.At(&ref,0)==0,"index cache retries after short read");Bytes actual(ref.size);ClearFault("read",1);short_read=true;Check(s.Read(ref,0,actual.data(),actual.size())!=0,"successful short value-page read fails");ClearFault();Check(s.Read(ref,0,actual.data(),actual.size())==0&&actual==baseline.begin()->second,"value cache retries after short read");}
}
}

namespace ams::fs {
Result GetEntryType(DirectoryEntryType*out,const char*path){Check(mounted,"GetEntryType requires mount");if(Fault("entry"))return IoFailure;if(std::string(path)==Root){*out=DirectoryEntryType_Directory;return 0;}if(!files.count(path))return ResultPathNotFound();*out=DirectoryEntryType_File;return 0;}
Result OpenFile(FileHandle*out,const char*path,int mode){Check(mounted,"Open requires mount");if(Fault("open"))return IoFailure;if(!files.count(path))return ResultPathNotFound();auto*h=new Handle{path,mode};out->handle=h;handles[h]=h;++open_handles;peak_handles=std::max(peak_handles,open_handles);return 0;}
void CloseFile(FileHandle f){auto it=handles.find(f.handle);Check(it!=handles.end(),"close valid handle once");delete it->second;handles.erase(it);--open_handles;}
Result GetFileSize(s64*out,FileHandle f){Check(mounted&&handles.count(f.handle),"size valid mounted handle");if(Fault("size"))return IoFailure;*out=static_cast<s64>(files.at(handles[f.handle]->path).size());return 0;}
Result SetFileSize(FileHandle f,s64 size){Check(mounted&&handles.count(f.handle),"resize valid mounted handle");if(Fault("resize"))return IoFailure;if(size<0)return ResultInvalidSize();auto&v=files.at(handles[f.handle]->path);if(static_cast<u64>(size)>v.size()&&static_cast<u64>(size)-v.size()>quota-Used())return ResultUsableSpaceNotEnough();v.resize(static_cast<size_t>(size));return 0;}
Result ReadFile(size_t*out,FileHandle f,s64 offset,void*dst,size_t size){Check(mounted&&handles.count(f.handle),"read valid mounted handle");max_read=std::max(max_read,size);const bool fault=Fault("read");if(fault&&!short_read)return IoFailure;auto&v=files.at(handles[f.handle]->path);if(offset<0||static_cast<u64>(offset)>v.size())return ResultInvalidSize();size_t actual=std::min(size,v.size()-static_cast<size_t>(offset));if(fault&&actual)--actual;if(actual)std::memcpy(dst,v.data()+offset,actual);*out=actual;return 0;}
Result ReadFile(FileHandle f,s64 offset,void*dst,size_t size){size_t actual=0;const auto rc=ReadFile(&actual,f,offset,dst,size);return rc?rc:(actual==size?0:IoFailure);}
Result WriteFile(FileHandle f,s64 offset,const void*src,size_t size,const WriteOption&){Check(mounted&&handles.count(f.handle),"write valid mounted handle");max_write=std::max(max_write,size);const bool fault=Fault("write");auto&v=files.at(handles[f.handle]->path);if(offset<0||static_cast<u64>(offset)>v.size()||size>v.size()-static_cast<size_t>(offset))return ResultInvalidSize();if(!fault||partial_write){const auto actual=fault?size/2:size;if(actual)std::memcpy(v.data()+offset,src,actual);}return fault?IoFailure:0;}
Result FlushFile(FileHandle f){Check(mounted&&handles.count(f.handle),"flush valid mounted handle");return Fault("flush")?IoFailure:0;}
Result CreateFile(const char*path,s64 size){Check(mounted,"create mounted");if(Fault("create"))return IoFailure;if(files.count(path))return ResultPathAlreadyExists();if(size<0)return ResultInvalidSize();if(static_cast<u64>(size)>quota-Used())return ResultUsableSpaceNotEnough();files[path]=Bytes(static_cast<size_t>(size));return 0;}
Result DeleteFile(const char*path){Check(mounted,"delete mounted");if(Fault("delete"))return IoFailure;if(IsOpen(path))return IoFailure;return files.erase(path)?0:Result(ResultPathNotFound());}
Result RenameFile(const char*from,const char*to){Check(mounted,"rename mounted");if(Fault("rename"))return IoFailure;if(IsOpen(from)||IsOpen(to))return IoFailure;if(!files.count(from))return ResultPathNotFound();if(files.count(to))return ResultPathAlreadyExists();files[to]=std::move(files[from]);files.erase(from);return 0;}
Result GetFreeSpaceSize(s64*out,const char*){Check(mounted,"free-space mounted");if(Fault("free"))return IoFailure;Check(Used()<=quota,"used storage within quota");*out=static_cast<s64>(quota-Used());return 0;}
Result GetTotalSpaceSize(s64*out,const char*){Check(mounted,"total-space mounted");if(Fault("total"))return IoFailure;*out=static_cast<s64>(quota);return 0;}
Result CommitSaveData(const char*){Check(mounted,"commit mounted");const bool failed=Fault("commit");if(current_operation!="Commit")Check(files[Archive]==allowed_canonical,"internal scratch checkpoint cannot publish logical mutations");if(failed&&!fail_commit_after)return IoFailure;durable=files;++commit_id;return failed?IoFailure:0;}
void Unmount(const char*){++calls["unmount"];Check(mounted,"unmount mounted");Check(open_handles==0,"all file handles closed before unmount");mounted=false;files=durable;}
Result MountSystemSaveData(const char*,SaveDataSpaceId,u64){Check(!mounted,"remount unmounted");if(Fault("mount"))return IoFailure;mounted=true;files=durable;return 0;}
Result ExtendSaveData(SaveDataSpaceId,u64,s64 available,s64 new_journal){Check(!mounted&&open_handles==0,"extend unmounted save");if(Fault("extend"))return IoFailure;if(available<static_cast<s64>(quota)||new_journal<static_cast<s64>(journal))return ResultInvalidSize();if(static_cast<u64>(available)+static_cast<u64>(new_journal)>device_limit)return ResultUsableSpaceNotEnough();quota=static_cast<u64>(available);journal=static_cast<u64>(new_journal);return 0;}
namespace impl {Result ReadSaveDataFileSystemExtraData(SaveDataExtraData*out,SaveDataSpaceId,u64){if(Fault("extra"))return IoFailure;*out={};out->available_size=static_cast<s64>(quota);out->journal_size=static_cast<s64>(journal);out->commit_id=commit_id;return 0;}}
}

int main(){BasicTests();LargeTests();GrowthTests();MutationFaultTests();CommitFaultTests();PowerLossTests();GrowthFaultTests();PreparedCommitTests();MalformedAndIoTests();Check(open_handles==0,"all filesystem handles released");std::printf("PASS %zu checks: actual disk store, 20001 records, multi-MiB range reads, scratch-only growth checkpoints, restart/commit publication, injected IO failures and corruption handling. Peak simultaneous handles %zu.\n",checks,peak_handles);}
