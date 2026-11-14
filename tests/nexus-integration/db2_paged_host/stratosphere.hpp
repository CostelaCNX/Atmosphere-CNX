#pragma once
#include <vapours.hpp>
namespace ams::hos {inline constexpr int Version_3_0_0=3;inline int GetVersion(){return 23;}}
namespace ams::fs {
constexpr size_t MountNameLengthMax=15,EntryNameLengthMax=768;
using SaveDataId=u64;using SystemSaveDataId=u64;
enum class SaveDataSpaceId:u8 {System=0,User=1,SdSystem=2};
enum DirectoryEntryType {DirectoryEntryType_Directory=0,DirectoryEntryType_File=1};
enum {OpenMode_Read=1,OpenMode_Write=2,OpenMode_ReadWrite=3,OpenMode_AllowAppend=4};
struct FileHandle{void* handle=nullptr;};
struct WriteOption{unsigned value;static const WriteOption None,Flush;};
inline const WriteOption WriteOption::None{0},WriteOption::Flush{1};
struct SaveDataExtraData {char before[0x58]{};s64 available_size{},journal_size{};u64 commit_id{};char rest[0x190]{};};
static_assert(sizeof(SaveDataExtraData)==0x200);
namespace impl {Result ReadSaveDataFileSystemExtraData(SaveDataExtraData*,SaveDataSpaceId,SaveDataId);}
using ResultPathNotFound=Error<0x202>;using ResultPathAlreadyExists=Error<0x402>;
using ResultUsableSpaceNotEnough=Error<0xC02>;using ResultUnsupportedOperation=Error<0xE02>;
using ResultNotEnoughFreeSpace=ResultUsableSpaceNotEnough;
using ResultInvalidArgument=Error<0x1002>;using ResultInvalidSize=Error<0x1202>;
Result GetEntryType(DirectoryEntryType*,const char*);
Result OpenFile(FileHandle*,const char*,int);void CloseFile(FileHandle);
Result ReadFile(size_t*,FileHandle,s64,void*,size_t);Result ReadFile(FileHandle,s64,void*,size_t);
Result WriteFile(FileHandle,s64,const void*,size_t,const WriteOption&);Result FlushFile(FileHandle);
Result GetFileSize(s64*,FileHandle);Result SetFileSize(FileHandle,s64);
Result CreateFile(const char*,s64);Result DeleteFile(const char*);Result RenameFile(const char*,const char*);
Result GetFreeSpaceSize(s64*,const char*);Result GetTotalSpaceSize(s64*,const char*);
Result CommitSaveData(const char*);void Unmount(const char*);
Result MountSystemSaveData(const char*,SaveDataSpaceId,SystemSaveDataId);
Result ExtendSaveData(SaveDataSpaceId,SaveDataId,s64,s64);
}
namespace ams::ncm {
enum class ContentMetaType:u8 {Unknown=0,SystemProgram=1,SystemData=2,SystemUpdate=3,Application=0x80,Patch=0x81,AddOnContent=0x82,Delta=0x83};
enum class ContentInstallType:u8 {Full=0,FragmentOnly=1,Unknown=7};
struct ContentMetaKey {
u64 id;u32 version;ContentMetaType type;ContentInstallType install_type;u8 padding[2];
bool operator<(const ContentMetaKey&r)const{return std::tie(id,version,type,install_type)<std::tie(r.id,r.version,r.type,r.install_type);}
bool operator==(const ContentMetaKey&r)const{return std::tie(id,version,type,install_type)==std::tie(r.id,r.version,r.type,r.install_type);}
bool operator!=(const ContentMetaKey&r)const{return !(*this==r);}
};
static_assert(sizeof(ContentMetaKey)==16);
using ResultInvalidContentMetaDatabase=Error<0x505>;using ResultContentMetaNotFound=Error<0x705>;
using ResultBufferInsufficient=Error<0x905>;using ResultInvalidOffset=Error<0xB05>;
using ResultAllocationFailed=Error<0x1005>;
}
