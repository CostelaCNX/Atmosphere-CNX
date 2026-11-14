#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
using u64=uint64_t; using u32=uint32_t; using s64=int64_t;
struct Result { int code{}; Result(int value=0):code(value){} int GetValue()const{return code;} };
template<int N> struct Error : Result { Error():Result(N){} static bool Includes(Result r){return r.code==N;} };
#define R_SUCCEEDED(R) ((R).code==0)
#define R_TRY(E) do { Result r_=(E); if(r_.code)return r_; }while(0)
#define R_UNLESS(C,E) do {if(!(C))return (E);}while(0)
#define R_SUCCEED() return Result{}
#define R_SUCCEED_IF(C) do{if(C)R_SUCCEED();}while(0)
#define R_RETURN(E) return (E)
#define R_THROW(E) return (E)
#define AMS_ASSERT(C) assert(C)
#define AMS_ABORT() std::abort()
#define AMS_UNUSED(...) (void)std::tuple(__VA_ARGS__)
#define NON_COPYABLE(T)
#define NON_MOVEABLE(T)
namespace util {using std::size;using std::snprintf;template<class... T>int SNPrintf(char*p,size_t n,const char*f,T...a){return snprintf(p,n,f,a...);}void Strlcpy(char*out,const char*in,size_t size){std::snprintf(out,size,"%s",in);}}
namespace sf::cmif {using DomainObjectId=int;}
namespace fs {
constexpr size_t EntryNameLengthMax=768;
using ResultPathNotFound=Error<1>; using ResultPathAlreadyExists=Error<2>; using ResultTargetLocked=Error<3>;
using ResultInvalidArgument=Error<4>; using ResultSdCardNotPresent=Error<5>; using ResultTooLongPath=Error<6>;
using ResultAllocationMemoryFailedMakeUnique=Error<7>; using ResultOutOfRange=Error<8>; using ResultNotImplemented=Error<9>;
struct Path {std::string value;Path()=default;Path(const char*p):value(p){} Result Initialize(const char*p){value=p;return {};}bool IsEmpty()const{return value.empty();}const char*GetString()const{return value.c_str();}bool operator==(const char*p)const{return value==p;}};
enum DirectoryEntryType{DirectoryEntryType_File,DirectoryEntryType_Directory};
using OpenMode=int; constexpr int OpenMode_Read=1,OpenMode_Write=2;
using OpenDirectoryMode=int;constexpr int OpenDirectoryMode_Directory=1,OpenDirectoryMode_File=2;struct FileTimeStampRaw{};
struct DirectoryEntry{char name[768]{};DirectoryEntryType type{};s64 file_size{};};
namespace fsa {
enum QueryId{SetConcatenationFileAttribute=0,IsSignedSystemPartition=1};
struct IFile {std::string destination; explicit IFile(const char*p):destination(p){}virtual ~IFile()=default;};
struct IDirectory {
    std::string destination;std::vector<DirectoryEntry>entries;size_t cursor{};
    explicit IDirectory(const char*p="merged_usb"):destination(p){}virtual ~IDirectory()=default;
    Result Read(s64*out,DirectoryEntry*values,s64 n){return DoRead(out,values,n);}
    Result GetEntryCount(s64*out){return DoGetEntryCount(out);}
    virtual Result DoRead(s64*out,DirectoryEntry*values,s64 n){*out=0;while(*out<n&&cursor<entries.size())values[(*out)++]=entries[cursor++];return {};}
    virtual Result DoGetEntryCount(s64*out){*out=entries.size();return {};}
};
struct IFileSystem {
    std::vector<std::string> calls;
    std::map<std::string,bool> entries;
    Result entry_error{},open_error{};
    Result Record(const char*name){calls.emplace_back(name);return {};}
    Result CreateFile(const Path&,s64,int){return Record("create_file");}
    Result DeleteFile(const Path&){return Record("delete_file");}
    Result CreateDirectory(const Path&){return Record("create_dir");}
    Result DeleteDirectory(const Path&){return Record("delete_dir");}
    Result DeleteDirectoryRecursively(const Path&){return Record("delete_tree");}
    Result RenameFile(const Path&,const Path&){return Record("rename_file");}
    Result RenameDirectory(const Path&,const Path&){return Record("rename_dir");}
    Result GetEntryType(DirectoryEntryType*out,const Path&p){Record("entry_type");R_TRY(entry_error);auto i=entries.find(p.value);if(i==entries.end())return ResultPathNotFound();*out=i->second?DirectoryEntryType_Directory:DirectoryEntryType_File;return {};}
    Result OpenFile(std::unique_ptr<IFile>*out,const Path&p,OpenMode){Record("open_file");R_TRY(open_error);auto i=entries.find(p.value);if(i==entries.end()||i->second)return ResultPathNotFound();*out=std::make_unique<IFile>("native");return {};}
    Result OpenDirectory(std::unique_ptr<IDirectory>*out,const Path&path,OpenDirectoryMode mode){
        Record("open_dir");std::string p=path.IsEmpty()?"/":path.value;auto found=entries.find(p);if(found==entries.end()||!found->second)return ResultPathNotFound();
        auto result=std::make_unique<IDirectory>("native");std::string prefix=p=="/"?"/":p+"/";
        for(const auto&[name,dir]:entries)if(name.starts_with(prefix)&&name.find('/',prefix.size())==std::string::npos&&name.size()>prefix.size()&&(mode&(dir?OpenDirectoryMode_Directory:OpenDirectoryMode_File))){DirectoryEntry e{};std::strcpy(e.name,name.c_str()+prefix.size());e.type=dir?DirectoryEntryType_Directory:DirectoryEntryType_File;result->entries.push_back(e);}
        *out=std::move(result);return {};
    }
    Result Commit(){return Record("commit");}
    Result GetFreeSpaceSize(s64*out,const Path&){*out=101;return Record("free_space");}
    Result GetTotalSpaceSize(s64*out,const Path&){*out=202;return Record("total_space");}
    Result CleanDirectoryRecursively(const Path&){return Record("clean_tree");}
    Result GetFileTimeStampRaw(FileTimeStampRaw*,const Path&){return Record("timestamp");}
    Result QueryEntry(char*,size_t,const char*,size_t,QueryId,const Path&){return Record("query");}
    Result CommitProvisionally(s64){return Record("provisional");}
    Result Rollback(){return Record("rollback");}
    Result Flush(){return Record("flush");}
};
}}
bool route_ready=true, route_writable=true;u64 route_generation=17;
bool IsUsbContentFileSystemRouteReady(u64*out,bool writable){*out=route_generation;return route_ready&&(!writable||route_writable);}
namespace usb {
using ResultFileNotFound=Error<101>;using ResultAlreadyInitialized=Error<102>;using ResultOperationDenied=Error<103>;using ResultResourceBusy=Error<104>;using ResultInvalidParameter=Error<105>;using ResultNotInitialized=Error<106>;using ResultMediaChanged=Error<107>;using ResultInterrupted=Error<108>;
namespace volume {struct FileInfo{bool directory;};struct DirectoryEntryInfo{char name[768]{};bool directory{};u64 size{};};}
namespace storage {
struct Service {
    std::map<std::string,bool> entries;
    std::vector<std::string> calls;
    Result Check(u64 generation){return !route_ready||generation!=route_generation?Result(ResultMediaChanged()):Result{};}
    Result GetFileInfoAtGeneration(const char*p,volume::FileInfo*out,u64 g){R_TRY(Check(g));calls.emplace_back("info");auto i=entries.find(p);if(i==entries.end())return ResultFileNotFound();out->directory=i->second;return {};}
    Result CreateFileAtGeneration(const char*p,u64,u64 g){R_TRY(Check(g));calls.emplace_back("create_file");entries[p]=false;return {};}
    Result CreateDirectoryAtGeneration(const char*p,u64 g){R_TRY(Check(g));calls.emplace_back("create_dir");entries[p]=true;return {};}
    Result DeleteFileAtGeneration(const char*p,u64 g){R_TRY(Check(g));calls.emplace_back("delete");entries.erase(p);return {};}
    Result RenameFileAtGeneration(const char*p,const char*q,u64 g){R_TRY(Check(g));calls.emplace_back("rename");entries[q]=entries.at(p);entries.erase(p);return {};}
    Result FlushAtGeneration(u64 g){R_TRY(Check(g));calls.emplace_back("flush");return {};}
    Result GetSpaceSize(u64*f,u64*t){calls.emplace_back("space");*f=303;*t=404;return {};}
    Result ReadDirectoryAtGeneration(const char*p,volume::DirectoryEntryInfo*out,size_t limit,size_t skip,bool directories,bool files,size_t*count,u64 g){
        R_TRY(Check(g));calls.emplace_back("read_dir");*count=0;std::string prefix=std::string(p)=="/"?"/":std::string(p)+"/";
        for(const auto&[name,dir]:entries) {
            if(name.starts_with(prefix)&&name.find('/',prefix.size())==std::string::npos&&name.size()>prefix.size()&&(dir?directories:files)){
                if(skip){--skip;continue;}if(*count==limit)break;
                auto&e=out[(*count)++];std::strcpy(e.name,name.c_str()+prefix.size());e.directory=dir;e.size=0;
            }
        }
        return {};
    }
};
struct EjectLease{Result Acquire(Service*s,u64 g){return s->Check(g);}};
}}
namespace content_stats {std::atomic<int>create_attempts{},creates{},renames{};void ResultRecorded(int){}}
#include "u1_helpers.inc"
struct UsbContentFile:fs::fsa::IFile {usb::storage::Service*service;u64 generation;UsbContentFile(usb::storage::Service*s,const fs::Path&,fs::OpenMode,u64 g):IFile("usb"),service(s),generation(g){}Result AcquireLease(){return ConvertUsbFileSystemResult(service->Check(generation));}};
#include "u1_directory.inc"
#include "u1_overlay.inc"
size_t assertions=0,scenarios=0;
void Check(bool condition,const std::string&label){++assertions;if(!condition){std::fprintf(stderr,"FAIL %s\n",label.c_str());std::abort();}}
struct Fixture {
    usb::storage::Service usb;
    std::shared_ptr<fs::fsa::IFileSystem>native=std::make_shared<fs::fsa::IFileSystem>();
    UsbContentOverlayFileSystem overlay;
    explicit Fixture(bool pin):overlay(&usb,pin?nullptr:native,pin?17:0){
        route_ready=route_writable=true;route_generation=17;
        usb.entries["/"]=native->entries["/"]=true;
        for(const char*root:{"/registered","/placehld"}){usb.entries[root]=native->entries[root]=true;usb.entries[std::string(root)+"/file"]=native->entries[std::string(root)+"/file"]=false;usb.entries[std::string(root)+"/dir"]=native->entries[std::string(root)+"/dir"]=true;usb.entries[std::string(root)+"/dir/child"]=false;}
        native->entries["/other/file"]=false;
    }
};
using Action=std::function<Result(Fixture&,const std::string&)>;
std::vector<std::pair<std::string,Action>> Operations(){return {
    {"create_file",[](Fixture&f,const std::string&p){return f.overlay.DoCreateFile((p+"/new").c_str(),4,0);}},
    {"delete_file",[](Fixture&f,const std::string&p){return f.overlay.DoDeleteFile((p+"/file").c_str());}},
    {"create_dir",[](Fixture&f,const std::string&p){return f.overlay.DoCreateDirectory((p+"/newdir").c_str());}},
    {"delete_dir",[](Fixture&f,const std::string&p){return f.overlay.DoDeleteDirectory((p+"/dir").c_str());}},
    {"delete_tree",[](Fixture&f,const std::string&p){return f.overlay.DoDeleteDirectoryRecursively((p+"/dir").c_str());}},
    {"rename_file",[](Fixture&f,const std::string&p){return f.overlay.DoRenameFile((p+"/file").c_str(),(p+"/renamed").c_str());}},
    {"rename_dir",[](Fixture&f,const std::string&p){return f.overlay.DoRenameDirectory((p+"/dir").c_str(),(p+"/renamed").c_str());}},
    {"clean_tree",[](Fixture&f,const std::string&p){return f.overlay.DoCleanDirectoryRecursively((p+"/dir").c_str());}},
    {"open_file_write",[](Fixture&f,const std::string&p){std::unique_ptr<fs::fsa::IFile>file;return f.overlay.DoOpenFile(&file,(p+"/file").c_str(),fs::OpenMode_Write);}},
    {"open_file_readwrite",[](Fixture&f,const std::string&p){std::unique_ptr<fs::fsa::IFile>file;return f.overlay.DoOpenFile(&file,(p+"/file").c_str(),fs::OpenMode_Read|fs::OpenMode_Write);}},
    {"query",[](Fixture&f,const std::string&p){return f.overlay.DoQueryEntry(nullptr,0,nullptr,0,fs::fsa::SetConcatenationFileAttribute,(p+"/file").c_str());}},
    {"commit",[](Fixture&f,const std::string&){return f.overlay.DoCommit();}},
    {"provisional",[](Fixture&f,const std::string&){return f.overlay.DoCommitProvisionally(1);}},
    {"rollback",[](Fixture&f,const std::string&){return f.overlay.DoRollback();}},
    {"flush",[](Fixture&f,const std::string&){return f.overlay.DoFlush();}},
    {"free_space",[](Fixture&f,const std::string&p){s64 value=0;return f.overlay.DoGetFreeSpaceSize(&value,p.c_str());}},
    {"total_space",[](Fixture&f,const std::string&p){s64 value=0;return f.overlay.DoGetTotalSpaceSize(&value,p.c_str());}}
};}
void NativeMatrix(){
    for(bool ready:{false,true})for(bool writable:{false,true})for(const char*root:{"/registered","/placehld"})for(const auto&[name,run]:Operations()){
        Fixture f(false);route_ready=ready;route_writable=writable;++scenarios;
        Check(run(f,root).code==0,"native operation "+name);
        Check(f.native->calls.size()==1,"exactly one native dispatch "+name);
        Check(f.usb.calls.empty(),"no USB calls on native operation "+name);
    }
    for(const char*path:{"/","","/other","/registered-other","/placehld-other"}){
        Fixture f(false);s64 size=0;++scenarios;
        Check(f.overlay.DoGetFreeSpaceSize(&size,path).code==0&&size==101,"native space for every root");
        Check(f.overlay.DoGetTotalSpaceSize(&size,path).code==0&&size==202,"native total space");
        Check(f.usb.calls.empty(),"native space never queries USB");
    }
}
void PinnedMatrix(){
    for(const char*root:{"/registered","/placehld"})for(const auto&[name,run]:Operations()){
        Fixture f(true);++scenarios;const auto result=run(f,root);
        const int expected=name=="query"?9:(name=="rollback"||name=="provisional")?1:0;
        Check(result.code==expected,"pinned result "+name);
        Check(f.native->calls.empty(),"pinned never falls back native "+name);
        if(name!="rollback")Check(!f.usb.calls.empty(),"pinned calls USB "+name);
    }
    for(int stale:{0,1,2})for(const auto&[name,run]:Operations()){
        Fixture f(true);++scenarios;if(stale==0)route_ready=false;else if(stale==1)route_generation=18;else route_writable=false;
        Check(run(f,"/registered").code==5,"pinned stale/unwritable fails closed "+name);
        Check(f.native->calls.empty()&&f.usb.calls.empty(),"no I/O after invalid pinned check "+name);
    }
}
void ReadAndRenameMatrix(){
    for(bool pin:{false,true})for(const char*root:{"/registered","/placehld"}){
        Fixture f(pin);std::unique_ptr<fs::fsa::IFile>file;std::unique_ptr<fs::fsa::IDirectory>directory;fs::DirectoryEntryType type{};++scenarios;
        const bool usb_read=pin;
        Check(f.overlay.DoOpenFile(&file,(std::string(root)+"/file").c_str(),fs::OpenMode_Read).code==0,"read collision succeeds");
        Check(file->destination==(usb_read?"usb":"native"),"native copy wins unpinned collision, pinned stays USB");
        f.usb.entries[std::string(root)+"/file"]=true;
        Check(f.overlay.DoGetEntryType(&type,(std::string(root)+"/file").c_str()).code==0,"entry collision succeeds");
        Check(type==(usb_read?fs::DirectoryEntryType_Directory:fs::DirectoryEntryType_File),"entry type comes from same route as read");
        Check(f.overlay.DoOpenDirectory(&directory,root,0).code==0,"directory succeeds");
        Check(directory->destination==(pin||std::string(root)=="/registered"?"merged_usb":"native"),"directory route selection");
    }
    for(bool pin:{false,true})for(const char*root:{"/registered","/placehld"}){
        Fixture f(pin);std::unique_ptr<fs::fsa::IFile>file;++scenarios;f.usb.entries.erase(std::string(root)+"/file");
        Check(f.overlay.DoOpenFile(&file,(std::string(root)+"/file").c_str(),fs::OpenMode_Read).code==(pin?1:0),"missing USB content native fallback only unpinned");
        Check(pin?f.native->calls.empty():file->destination=="native","pinned missing never opens native");
    }
    for(bool pin:{false,true})for(const auto&paths:std::vector<std::pair<const char*,const char*>>{{"/placehld/file","/registered/new"},{"/registered/file","/other/new"},{"/other/file","/registered/new"},{"/registered-other/file","/registered/new"}}){
        Fixture f(pin);++scenarios;auto result=f.overlay.DoRenameFile(paths.first,paths.second);
        bool internal=std::string(paths.first)=="/placehld/file";
        Check(result.code==(pin&&!internal?1:0),"rename path boundary result");
        Check(pin?f.native->calls.empty():f.native->calls.size()==1,"rename respects destination owner");
        if(!pin||!internal)Check(f.usb.calls.empty(),"native/cross-root rename no USB writes");
    }
    Fixture f(false);route_ready=false;std::unique_ptr<fs::fsa::IFile>file;
    Check(f.overlay.DoOpenFile(&file,"/registered/file",fs::OpenMode_Read).code==0&&file->destination=="native","disconnected USB retains native reads");
}
void ReadPriorityAndFailures(){
    for(bool writable:{false,true}){
        Fixture f(false);route_writable=writable;f.native->entries.erase("/registered/file");std::unique_ptr<fs::fsa::IFile>file;fs::DirectoryEntryType type{};++scenarios;
        Check(f.overlay.DoOpenFile(&file,"/registered/file",fs::OpenMode_Read).code==0&&file->destination=="usb","USB-only registered game remains readable, including readonly USB");
        Check(f.overlay.DoGetEntryType(&type,"/registered/file").code==0&&type==fs::DirectoryEntryType_File,"USB-only entry remains visible");
    }
    for(bool lookup_error:{false,true}){
        Fixture f(false);std::unique_ptr<fs::fsa::IFile>file;++scenarios;
        if(lookup_error)f.native->entry_error=Result(75);else f.native->open_error=Result(76);
        Check(f.overlay.DoOpenFile(&file,"/registered/file",fs::OpenMode_Read).code==(lookup_error?75:76),"native lookup/open failure propagated with duplicate USB present");
        Check(!file&&f.usb.calls.empty(),"native error cannot fall through to another drive");
    }
    {
        Fixture f(false);f.native->entries["/registered/file"]=true;std::unique_ptr<fs::fsa::IFile>file;fs::DirectoryEntryType type{};++scenarios;
        Check(f.overlay.DoGetEntryType(&type,"/registered/file").code==0&&type==fs::DirectoryEntryType_Directory,"native directory wins over USB file with same name");
        Check(f.overlay.DoOpenFile(&file,"/registered/file",fs::OpenMode_Read).code!=0&&!file,"native incompatible type never opens USB duplicate");
        Check(f.usb.calls.empty(),"native type collision avoids USB access");
    }
    {
        Fixture f(false);f.native->entry_error=Result(75);fs::DirectoryEntryType type{};++scenarios;
        Check(f.overlay.DoGetEntryType(&type,"/registered/file").code==75&&f.usb.calls.empty(),"entry lookup errors do not select USB");
    }
    {
        Fixture f(false);f.usb.entries["/registered/file"]=true;std::unique_ptr<fs::fsa::IDirectory>directory;++scenarios;
        Check(f.overlay.DoOpenDirectory(&directory,"/registered/file",3).code!=0&&!directory,"native file prevents opening same-name USB directory");
        Check(f.usb.calls.empty(),"directory type collision does not touch USB");
    }
    for(bool native_exists:{false,true}){
        Fixture f(false);if(!native_exists)f.native->entries.erase("/registered/file");fs::FileTimeStampRaw stamp{};++scenarios;
        Check(f.overlay.DoGetFileTimeStampRaw(&stamp,"/registered/file").code==(native_exists?0:9),"timestamp follows native priority and existing USB unsupported result");
        if(native_exists)Check(f.usb.calls.empty(),"native timestamp does not query USB duplicate");
    }
    Check(IsVisibleUsbRootEntry("registered",false)&&!IsVisibleUsbRootEntry("placehld",false),"generic root exposes registered games but not USB placeholders");
    Check(IsVisibleUsbRootEntry("registered",true)&&IsVisibleUsbRootEntry("placehld",true)&&!IsVisibleUsbRootEntry("unrelated",true),"pinned root exposes only USB content trees");
}
std::map<std::string,fs::DirectoryEntryType> Listing(fs::fsa::IDirectory&directory){
    std::map<std::string,fs::DirectoryEntryType> result;
    for(;;){fs::DirectoryEntry entries[2]{};s64 count=0;Check(directory.Read(&count,entries,2).code==0,"merged read succeeds");if(!count)break;for(s64 i=0;i<count;++i)Check(result.emplace(entries[i].name,entries[i].type).second,"merged listing has no duplicates");}
    return result;
}
void MergedDirectoryCases(){
    for(bool pin:{false,true}){
        Fixture f(pin);f.native->entries.erase("/placehld");f.usb.entries["/unrelated"]=true;std::unique_ptr<fs::fsa::IDirectory>directory;++scenarios;
        Check(f.overlay.DoOpenDirectory(&directory,"/",3).code==0,"root merged directory opens");
        s64 count=0;Check(directory->GetEntryCount(&count).code==0&&count==(pin?2:1),"root count excludes unrelated paths and unpinned USB placeholders");
        auto entries=Listing(*directory);Check(entries.size()==size_t(count)&&entries.contains("registered")&&entries.contains("placehld")==pin,"root enumeration agrees with filtered count");
    }
    for(int mode:{1,2,3}){
        Fixture f(false);f.native->entries["/registered/file"]=true;f.native->entries["/registered/nativeonly"]=false;f.usb.entries["/registered/usbonly"]=false;std::unique_ptr<fs::fsa::IDirectory>directory;++scenarios;
        Check(f.overlay.DoOpenDirectory(&directory,"/registered",mode).code==0,"registered merged directory opens");
        s64 count=0;Check(directory->GetEntryCount(&count).code==0&&count==(mode==3?4:2),"native duplicate precedence keeps count consistent for type filter");
        auto entries=Listing(*directory);Check(entries.size()==size_t(count),"merged count matches enumerated entries");
        if(mode&1)Check(entries.at("file")==fs::DirectoryEntryType_Directory,"native directory type wins USB file duplicate");
        else Check(!entries.contains("file"),"native directory duplicate does not reappear as USB file under file-only filter");
        if(mode&2)Check(entries.contains("nativeonly")&&entries.contains("usbonly"),"native and USB-only entries retained");
    }
    {
        Fixture f(false);std::unique_ptr<fs::fsa::IDirectory>directory;++scenarios;
        Check(f.overlay.DoOpenDirectory(&directory,"/registered",3).code==0,"directory opens before child error");f.native->entry_error=Result(75);
        s64 count=0;fs::DirectoryEntry entry{};
        Check(directory->GetEntryCount(&count).code==75,"native child lookup error propagates during count");
        Check(directory->Read(&count,&entry,1).code==75,"native child lookup error propagates during read");
    }
}
int main(){NativeMatrix();PinnedMatrix();ReadAndRenameMatrix();ReadPriorityAndFailures();MergedDirectoryCases();std::printf("PASS U1 destination matrix: %zu scenarios, %zu assertions\n",scenarios,assertions);}
