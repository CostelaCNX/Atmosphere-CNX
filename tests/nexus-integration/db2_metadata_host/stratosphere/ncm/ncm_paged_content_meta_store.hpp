#pragma once
#include <stratosphere.hpp>
namespace ams::ncm {
class PagedContentMetaStore {
public:
struct RecordRef{ContentMetaKey key;u64 offset,size,generation;bool staged;};
inline static int read_calls=0,fail_read_at=-1,at_calls=0,fail_at_at=-1;
inline static size_t largest_read=0;inline static bool fail_find=false;
std::map<ContentMetaKey,std::vector<u8>> entries;bool available=false;u64 generation=1;
Result Initialize(const char*,const char*,fs::SaveDataSpaceId,u64){available=true;return {};}
u32 GetCount()const{return entries.size();}
Result At(RecordRef*out,u32 index){if(at_calls++==fail_at_at)return Result(902);if(index>=entries.size())return Result(903);auto it=entries.begin();std::advance(it,index);*out={it->first,0,it->second.size(),generation,false};return {};}
Result Find(RecordRef*out,bool*found,const ContentMetaKey&key){if(fail_find)return Result(904);auto it=entries.find(key);*found=it!=entries.end();if(*found)*out={it->first,0,it->second.size(),generation,false};return {};}
Result LowerBound(u32*out,const ContentMetaKey&key){*out=std::distance(entries.begin(),entries.lower_bound(key));return {};}
Result Read(const RecordRef&ref,u64 off,void*dst,size_t size){largest_read=std::max(largest_read,size);if(read_calls++==fail_read_at)return Result(905);auto it=entries.find(ref.key);if(ref.generation!=generation||it==entries.end()||off>it->second.size()||size>it->second.size()-off)return Result(906);if(size)std::memcpy(dst,it->second.data()+off,size);return {};}
Result Set(const ContentMetaKey&key,const void*p,size_t n){entries[key]=std::vector<u8>((const u8*)p,(const u8*)p+n);++generation;return {};}
Result Remove(const ContentMetaKey&key){if(!entries.erase(key))return kvdb::ResultKeyNotFound();++generation;return {};}
Result Commit(){return fs::CommitSaveData("db");}void Invalidate(){available=false;++generation;}bool IsAvailable()const{return available;}
static void ResetFaults(){read_calls=at_calls=0;fail_read_at=fail_at_at=-1;largest_read=0;fail_find=false;}
};
}
