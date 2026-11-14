#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <vector>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
using s32=int32_t; using s64=int64_t;
#define NON_COPYABLE(T) T(const T&)=delete; T& operator=(const T&)=delete
#define NON_MOVEABLE(T) T(T&&)=delete; T& operator=(T&&)=delete
#define AMS_OFFSETOF(T,F) offsetof(T,F)
#define AMS_UNUSED(...) (void)std::tuple(__VA_ARGS__)
#define AMS_ABORT_UNLESS(C) assert(C)
#define AMS_UNREACHABLE_DEFAULT_CASE() default: assert(false); break
#define ALWAYS_INLINE_LAMBDA
namespace ams {
struct Result { int v{}; constexpr Result(int n=0):v(n){} constexpr operator int()const{return v;} u32 GetValue()const{return v;} };
inline Result ResultSuccess(){return {};}
template<int Code> struct Error { operator Result()const{return Result(Code);} operator int()const{return Code;} static bool Includes(Result r){return r.v==Code;} };
namespace util {template<class T>using optional=std::optional<T>;inline constexpr auto nullopt=std::nullopt;using std::make_optional;}
namespace os {using SdkRecursiveMutex=std::recursive_mutex;}
namespace hos {inline constexpr int Version_9_0_0=9;inline int version=23;inline int GetVersion(){return version;}}
namespace fs {inline constexpr size_t MountNameLengthMax=15;enum class SaveDataSpaceId{System,SdSystem};using ResultInvalidArgument=Error<700>;inline bool fail_commit=false;inline int commit_count=0;inline Result CommitSaveData(const char*){++commit_count;return fail_commit?Result(900):Result();}}
namespace sf {
template<class T> class Out {T*p; public: Out(T*v):p(v){} T*GetPointer()const{return p;}void SetValue(T v)const{*p=v;}T&operator*()const{return *p;}};
template<class T> class OutArray {T*p;size_t n;public:OutArray(T*v,size_t s):p(v),n(s){}T*GetPointer()const{return p;}size_t GetSize()const{return n;}T&operator[](size_t i)const{return p[i];}};
template<class T> class InArray {const T*p;size_t n;public:InArray(const T*v,size_t s):p(v),n(s){}const T*GetPointer()const{return p;}size_t GetSize()const{return n;}const T&operator[](size_t i)const{return p[i];}};
class InBuffer {const void*p;size_t n;public:InBuffer(const void*v,size_t s):p(v),n(s){}const void*GetPointer()const{return p;}size_t GetSize()const{return n;}};
class OutBuffer {void*p;size_t n;public:OutBuffer(void*v,size_t s):p(v),n(s){}void*GetPointer()const{return p;}size_t GetSize()const{return n;}};
}
namespace ncm {
enum class ContentMetaType:u8 {Unknown=0,SystemProgram=1,Application=0x80,Patch=0x81,AddOnContent=0x82,Delta=0x83};
enum class ContentInstallType:u8 {Full=0,FragmentOnly=1,Unknown=7};
enum class ContentType:u8 {Meta=0,Program=1,Data=2,Control=3,HtmlDocument=4,LegalInformation=5,DeltaFragment=6};
enum class ContentMetaPlatform:u8{Nx=0};enum class StorageId:u8{None=0,BuiltInUser=4,SdCard=5};
struct ApplicationId {u64 value;bool operator==(const ApplicationId&)const=default;};
struct ProgramId{u64 value;};struct PatchId{u64 value;};struct DataPatchId{u64 value;};
inline constexpr ApplicationId InvalidApplicationId{};
struct ContentId{u8 data[16]{};bool operator==(const ContentId&)const=default;};inline constexpr ContentId InvalidContentId{};
struct ContentInfo{ContentId id;u32 size_low;u16 size_high;ContentType type;u8 id_offset;ContentId GetId()const{return id;}ContentType GetType()const{return type;}u8 GetIdOffset()const{return id_offset;}};
struct ContentMetaKey {u64 id;u32 version;ContentMetaType type;ContentInstallType install_type;u8 padding[2];
 bool operator<(const ContentMetaKey&r)const{return std::tie(id,version,type,install_type)<std::tie(r.id,r.version,r.type,r.install_type);}
 bool operator==(const ContentMetaKey&r)const{return std::tie(id,version,type,install_type)==std::tie(r.id,r.version,r.type,r.install_type);}
 static ContentMetaKey MakeUnknownType(u64 id,u32 v){return {id,v,ContentMetaType::Unknown,ContentInstallType::Full,{}};}};
struct ApplicationContentMetaKey{ContentMetaKey key;ApplicationId application_id;};
struct ContentMetaInfo{u64 id;u32 version;ContentMetaType type;u8 attributes;u8 padding[2];};
struct ContentMetaHeader{u16 extended_header_size,content_count,content_meta_count;u8 attributes;ContentMetaPlatform platform;};
struct ApplicationMetaExtendedHeader{PatchId patch_id;u32 required_system_version,required_application_version;};
struct PatchMetaExtendedHeader{ApplicationId application_id;u32 required_system_version,extended_data_size;u8 reserved[8];};
struct AddOnContentMetaExtendedHeader{ApplicationId application_id;u32 required_application_version;u8 content_accessibilities;u8 padding[3];DataPatchId data_patch_id;};
struct LegacyAddOnContentMetaExtendedHeader{ApplicationId application_id;u32 required_application_version,padding;};
struct DeltaMetaExtendedHeader{ApplicationId application_id;u32 extended_data_size,padding;};
static_assert(sizeof(ContentMetaHeader)==8 && sizeof(ContentInfo)==24 && sizeof(ContentMetaInfo)==16 && sizeof(ContentMetaKey)==16);
using ResultContentMetaNotFound=Error<7>;using ResultContentNotFound=Error<5>;using ResultInvalidContentMetaDatabase=Error<110>;using ResultInvalidContentMetaFileSize=Error<390>;using ResultInvalidOffset=Error<8182>;using ResultInvalidContentMetaKey=Error<240>;using ResultBufferInsufficient=Error<180>;using ResultInvalidAddOnContentMetaExtendedHeader=Error<400>;using ResultNotSupported=Error<190>;
template<class T>inline constexpr bool IsIContentMetaDatabase=true;
class ContentMetaReader {const u8*p;public:ContentMetaReader(const void*v,size_t):p((const u8*)v){}const ContentMetaHeader*GetHeader()const{return (const ContentMetaHeader*)p;}size_t GetExtendedHeaderSize()const{return GetHeader()->extended_header_size;}template<class T>const T*GetExtendedHeader()const{return (const T*)(p+8);}size_t GetContentCount()const{return GetHeader()->content_count;}size_t GetContentMetaCount()const{return GetHeader()->content_meta_count;}
const ContentInfo*GetContentInfo(size_t i)const{return (const ContentInfo*)(p+8+GetExtendedHeaderSize()+i*24);}const ContentMetaInfo*GetContentMetaInfo(size_t i)const{return (const ContentMetaInfo*)(p+8+GetExtendedHeaderSize()+GetContentCount()*24+i*16);}
const ContentInfo*GetContentInfo(ContentType type)const{const ContentInfo*best=nullptr;for(size_t i=0;i<GetContentCount();++i){auto*c=GetContentInfo(i);if(c->GetType()==type&&(!best||c->GetIdOffset()<best->GetIdOffset()))best=c;}return best;}
const ContentInfo*GetContentInfo(ContentType type,u8 off)const{for(size_t i=0;i<GetContentCount();++i){auto*c=GetContentInfo(i);if(c->GetType()==type&&c->GetIdOffset()==off)return c;}return nullptr;}
util::optional<ApplicationId>GetApplicationId(const ContentMetaKey&key)const{switch(key.type){case ContentMetaType::Application:return ApplicationId{key.id};case ContentMetaType::Patch:case ContentMetaType::AddOnContent:case ContentMetaType::Delta:return *GetExtendedHeader<ApplicationId>();default:return {};}}};
inline bool usb_enabled=false,usb_install=false,usb_ready=false;inline int mappings=0,unmaps=0;inline u64 mapped_title=0;inline ContentId mapped_meta{},mapped_control{},mapped_program{};inline bool mapping_failed=false;
struct UsbContentStatus{u32 enabled,running,ready,writable,mounted;};
inline bool IsUsbContentEnabled(){return usb_enabled;}inline bool IsUsbContentEnabledForInstall(){return usb_install;}inline Result GetUsbContentStatusImpl(UsbContentStatus*out){*out=usb_ready?UsbContentStatus{1,1,1,1,1}:UsbContentStatus{};return {};}
inline bool IsUsbContentLocationRegistered(ContentId){return true;}inline bool WasUsbContentRegisteredThisBoot(ContentId){return true;}
inline Result RegisterUsbTitleContent(ProgramId id,ContentId m,ContentId c,ContentId p){if(mapping_failed)return Result(901);++mappings;mapped_title=id.value;mapped_meta=m;mapped_control=c;mapped_program=p;return {};}
inline Result RegisterUsbTitle(ProgramId){return {};}inline Result UnregisterUsbTitle(ProgramId){++unmaps;return {};}
}
namespace usb::content {inline constexpr bool UseFileSystemContentRoute=true;}
namespace kvdb {
using ResultKeyNotFound=Error<20>;
template<class Key>class MemoryKeyValueStore {
public: struct Entry{Key key;std::vector<u8>value;const Key&GetKey()const{return key;}const void*GetValuePointer()const{return value.data();}size_t GetValueSize()const{return value.size();}};std::vector<Entry> entries;
auto begin(){return entries.begin();}auto end(){return entries.end();}auto lower_bound(const Key&k){return std::lower_bound(begin(),end(),k,[](const Entry&e,const Key&k){return e.key<k;});}size_t GetCount()const{return entries.size();}
Result Set(const Key&k,const void*p,size_t n){auto it=lower_bound(k);if(it==end()||!(it->key==k))it=entries.insert(it,Entry{k,{}});it->value.assign((const u8*)p,(const u8*)p+n);return {};}
Result Remove(const Key&k){auto it=lower_bound(k);if(it==end()||!(it->key==k))return ResultKeyNotFound();entries.erase(it);return {};}
Result GetValueSize(size_t*out,const Key&k){auto it=lower_bound(k);if(it==end()||!(it->key==k))return ResultKeyNotFound();*out=it->value.size();return {};}
template<class T>Result GetValuePointer(T**out,const Key&k){auto it=lower_bound(k);if(it==end()||!(it->key==k))return ResultKeyNotFound();*out=(T*)it->value.data();return {};}
Result Get(size_t*out,void*p,size_t n,const Key&k){auto it=lower_bound(k);if(it==end()||!(it->key==k))return ResultKeyNotFound();*out=std::min(n,it->value.size());std::memcpy(p,it->value.data(),*out);return {};}
Result SaveStreaming(){return {};}
};
}
}
#define R_SUCCEEDED(R) (int(R)==0)
#define R_FAILED(R) (int(R)!=0)
#define R_TRY(E) do{ams::Result _r=(E);if(R_FAILED(_r))return _r;}while(0)
#define R_UNLESS(C,E) do{if(!(C))return (E);}while(0)
#define R_SUCCEED() return ams::ResultSuccess()
#define R_SUCCEED_IF(C) do{if(C)R_SUCCEED();}while(0)
#define R_RETURN(E) return (E)
#define R_TRY_CATCH(E) {ams::Result _caught=(E);if(R_FAILED(_caught))
#define R_CONVERT(T,E) if(T::Includes(_caught))return (E);
#define R_END_TRY_CATCH if(R_FAILED(_caught))return _caught;}
