#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>
using u32=uint32_t;using u64=uint64_t;using Result=int;
#define R_TRY(E) do{Result r_=(E);if(r_)return r_;}while(0)
#define R_RETURN(E) return (E)
#define R_UNLESS(C,E) do{if(!(C))return (E);}while(0)
#define R_SUCCEED() return 0
using ContentId=u64;using PlaceHolderId=u64;struct ProgramId{u64 value;};
namespace fs{Result ResultInvalidArgument(){return 4;}}
namespace ncm{Result ResultInvalidOperation(){return 5;}}
std::vector<int>order;Result base_result{},unregister_result{};
std::map<ContentId,bool>content_registered,content_boot;
struct ContentStorageImpl{virtual ~ContentStorageImpl()=default;virtual Result Register(PlaceHolderId,ContentId){order.push_back(1);return base_result;}};
Result UnregisterUsbContentLocation(ContentId id){order.push_back(2);if(unregister_result)return unregister_result;content_registered.erase(id);content_boot.erase(id);return 0;}
#include "u1_native_registration.inc"
struct TitleEntry{u64 program_id{},alias_program_id{};bool operator==(const TitleEntry&)const=default;};
struct ContentEntry{u64 program_id{},meta{},control{},program{};bool operator==(const ContentEntry&)const=default;};
std::mutex g_title_map_mutex;
std::array<TitleEntry,16>g_title_map_scratch;
std::array<ContentEntry,16>g_title_content_map_scratch;
std::vector<TitleEntry>titles,title_cache;
std::vector<ContentEntry>contents,content_cache;
Result pending_result{},title_write_result{},content_write_result{};
bool title_read_ok=true,content_read_ok=true;
int title_writes{},content_writes{};
Result CheckNoPendingRepair(){return pending_result;}
bool ReadTitleMap(std::array<TitleEntry,16>&entries,u32*count,bool){if(!title_read_ok)return false;*count=titles.size();std::copy(titles.begin(),titles.end(),entries.begin());return true;}
bool ReadTitleContentMap(std::array<ContentEntry,16>&entries,u32*count,bool){if(!content_read_ok)return false;*count=contents.size();std::copy(contents.begin(),contents.end(),entries.begin());return true;}
Result WriteTitleMap(const std::array<TitleEntry,16>&entries,u32 count){++title_writes;if(title_write_result)return title_write_result;titles.assign(entries.begin(),entries.begin()+count);title_cache=titles;return 0;}
Result WriteTitleContentMap(const std::array<ContentEntry,16>&entries,u32 count){++content_writes;if(content_write_result)return content_write_result;contents.assign(entries.begin(),entries.begin()+count);content_cache=contents;return 0;}
void UpdateProgramIdTitleCache(const std::array<TitleEntry,16>&entries,u32 count){title_cache.assign(entries.begin(),entries.begin()+count);}
void UpdateProgramIdContentCache(const std::array<ContentEntry,16>&entries,u32 count){content_cache.assign(entries.begin(),entries.begin()+count);}
#include "u1_title_unmap.inc"
size_t checks{};
void Check(bool condition,const char*message){++checks;if(!condition){std::fprintf(stderr,"FAIL %s\n",message);std::abort();}}
void Reset(){order.clear();base_result=unregister_result=pending_result=title_write_result=content_write_result=0;title_read_ok=content_read_ok=true;title_writes=content_writes=0;content_registered={{11,true},{22,true}};content_boot=content_registered;titles={{100,1000},{200,2000},{300,100}};contents={{100,11,12,13},{200,21,22,23},{300,31,32,33}};title_cache=titles;content_cache=contents;}
void NativeRegister(){
    NativeSdContentStorage storage;Reset();
    Check(storage.Register(1,11)==0,"native registration succeeds");
    Check(order==std::vector<int>({1,2}),"native write precedes ownership removal");
    Check(!content_registered.contains(11)&&!content_boot.contains(11),"native registration clears persistent and boot proof for exact content");
    Check(content_registered.contains(22)&&content_boot.contains(22),"unrelated USB content ownership retained");
    Reset();base_result=71;Check(storage.Register(1,11)==71,"native failure preserved");
    Check(order==std::vector<int>({1})&&content_registered.contains(11)&&content_boot.contains(11),"failed native registration cannot remove USB proof");
    Reset();unregister_result=72;Check(storage.Register(1,11)==72,"ownership publication failure is returned");
    Check(order==std::vector<int>({1,2})&&content_registered.contains(11),"cleanup failure retains registered ownership in mock durable state");
}
void TitleCleanup(){
    Reset();Check(UnregisterUsbTitle({100})==0,"native title cleanup succeeds");
    Check(titles==std::vector<TitleEntry>({{200,2000},{300,0}}),"target canonical row removed and only conflicting alias cleared");
    Check(contents==std::vector<ContentEntry>({{200,21,22,23},{300,31,32,33}}),"unrelated title-content rows retained");
    Check(title_cache==titles&&content_cache==contents,"loader caches receive cleaned maps");
    Check(content_registered.contains(22),"title cleanup does not globally clear shared content locations");
    Reset();Check(UnregisterUsbTitle({2000})==0,"alias cleanup succeeds");
    Check(titles==std::vector<TitleEntry>({{100,1000},{200,0},{300,100}})&&contents.size()==3,"alias ownership does not erase canonical USB game");
    Reset();Check(UnregisterUsbTitle({999})==0&&title_writes==0&&content_writes==0,"unrelated native title causes no map rewrite");
    Check(title_cache==titles&&content_cache==contents,"no-change path refreshes exact existing cache");
    Reset();Check(UnregisterUsbTitle({0})==4&&title_writes==0,"invalid identity rejected before mutation");
    Reset();pending_result=73;Check(UnregisterUsbTitle({100})==73&&title_writes==0&&content_writes==0,"pending repair prevents cleanup");
    Reset();title_read_ok=false;Check(UnregisterUsbTitle({100})==5&&title_writes==0&&content_writes==0,"unreadable map cannot be overwritten");
    Reset();title_write_result=74;Check(UnregisterUsbTitle({100})==74&&content_writes==0,"first map publication failure returned before content-map change");
}
int main(){NativeRegister();TitleCleanup();std::printf("PASS U1 native ownership and targeted stale-title cleanup: %zu assertions\n",checks);}
