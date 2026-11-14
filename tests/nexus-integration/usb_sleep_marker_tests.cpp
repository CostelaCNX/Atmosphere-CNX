// SPDX-License-Identifier: GPL-2.0-or-later
// Compile the real marker writer and inject FatFs operations at its boundary.
#include <stratosphere/usb/usb_resume_marker.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ams::usb;
using Result = unsigned;
using UINT = unsigned;
using FRESULT = unsigned;
constexpr unsigned FR_OK=0,FR_NO_FILE=1,FR_DENIED=2,FR_WRITE_PROTECTED=3,FR_EXIST=4,FR_LOCKED=5,FR_DISK_ERR=6;
constexpr unsigned FA_OPEN_EXISTING=0,FA_READ=1,FA_WRITE=2,FA_CREATE_NEW=4;
constexpr std::size_t FatPathBufferSize=64;
unsigned checks;
#define CHECK(test) do {++checks;if(!(test)){std::fprintf(stderr,"check failed at %d: %s\n",__LINE__,#test);std::exit(1);}}while(0)
#define R_UNLESS(c,r) do{if(!(c))return r;}while(0)
#define R_SUCCEED() return 0
#define R_RETURN(r) return (r)
#define R_TRY(e) do{const auto check_result=(e);if(check_result)return check_result;}while(0)
#define R_FAILED(rc) ((rc)!=0)
#define R_SUCCEEDED(rc) ((rc)==0)
constexpr Result ResultSuccess(){return 0;}
namespace usb{
constexpr Result ResultInvalidParameter(){return 20;}constexpr Result ResultResourceBusy(){return 21;}
constexpr Result ResultTransactionError(){return 22;}
}
constexpr Result TranslateFatResult(FRESULT result){return result;}
struct Device{bool quarantined{};void Quarantine(){quarantined=true;}};
struct Disk {
    bool exists{},read_only{},short_write{},occupied_during_create{};
    unsigned read_result{},write_result{},close_result{},mutations{},attempted_replay{},writes{},seek_result{};
    std::vector<unsigned char> data;
    Device device;
};
struct File{Disk *disk{};std::size_t cursor{};};
struct Handle{File file;bool open{};};
Result MakeFatPath(char *out,std::size_t capacity,const char *path) {
    CHECK(std::strcmp(path,"/NEXUS.RSM")==0);CHECK(capacity>12);
    std::snprintf(out,capacity,"0:%s",path);return {};
}
FRESULT f_open(File *file,const char *path,unsigned flags) {
    CHECK(std::strcmp(path,"0:/NEXUS.RSM")==0);
    CHECK(flags==(FA_READ|FA_WRITE|FA_OPEN_EXISTING)||flags==(FA_CREATE_NEW|FA_WRITE));
    auto &disk=*file->disk;file->cursor=0;
    if ((flags&FA_CREATE_NEW)!=0) {
        if(disk.exists||disk.occupied_during_create)return FR_EXIST;
        if(disk.read_only)return FR_WRITE_PROTECTED;
        disk.exists=true;++disk.mutations;
    } else if(!disk.exists)return FR_NO_FILE;
    if(disk.read_only)return FR_WRITE_PROTECTED;
    return FR_OK;
}
std::size_t f_size(File *file){return file->disk->data.size();}
FRESULT f_read(File *file,void *dst,UINT size,UINT *actual){
    if(file->disk->read_result)return file->disk->read_result;
    *actual=std::min<std::size_t>(size,file->disk->data.size());
    std::memcpy(dst,file->disk->data.data(),*actual);file->cursor+=*actual;return FR_OK;
}
FRESULT f_lseek(File *file,std::size_t position){file->cursor=position;return file->disk->seek_result;}
FRESULT f_write(File *file,const void *src,UINT size,UINT *actual){
    auto &disk=*file->disk;CHECK(!disk.device.quarantined&&file->cursor==0&&size==40);++disk.writes;
    if(disk.write_result)return disk.write_result;
    *actual=disk.short_write?size-1:size;disk.data.resize(*actual);std::memcpy(disk.data.data(),src,*actual);++disk.mutations;return FR_OK;
}
Result CloseFile(Handle *handle,Result rc){
    auto &disk=*handle->file.disk;
    if (disk.writes && (disk.short_write||disk.write_result) && !disk.device.quarantined) ++disk.attempted_replay;
    if(!disk.close_result)handle->open=false;
    return rc?rc:disk.close_result;
}
struct Writable {
    struct {struct {Handle handle;} writer;Device *device;} impl{};
    decltype(impl)* m_impl=&impl;
    explicit Writable(Disk&disk){impl.writer.handle.file.disk=&disk;impl.device=&disk.device;}
    bool IsInitialized()const{return m_impl!=nullptr;}
    Result WriteSleepMarker(const sleep::VolumeMarker &marker,bool *armed){
#include "actual_write_sleep_marker.hpp"
    }
};
int main(){
    sleep::VolumeMarker marker{},next{};std::memcpy(marker.signature,sleep::MarkerSignature,sizeof(marker.signature));
    next=marker;std::memset(marker.challenge,1,32);std::memset(next.challenge,2,32);
    Disk disk;Writable volume(disk);bool armed{};
    CHECK(volume.WriteSleepMarker(marker,&armed)==0&&armed&&disk.exists&&disk.data.size()==40);
    CHECK(volume.WriteSleepMarker(next,&armed)==0&&armed&&disk.writes==2&&std::memcmp(disk.data.data(),&next,40)==0);
    for(unsigned fault=0;fault<5;++fault){
        Disk other;Writable target(other);
        if(fault<3){other.exists=true;other.data.resize(fault==0?1:40,13);
            if(fault==2)other.read_only=true;}
        if(fault==3)other.occupied_during_create=true;
        if(fault==4)other.read_only=true;
        const auto before=other.data;armed=true;
        CHECK(target.WriteSleepMarker(marker,&armed)==0&&!armed&&other.data==before&&other.mutations==0&&other.writes==0);
    }
    for(unsigned fault=0;fault<5;++fault){
        Disk bad;Writable target(bad);bad.exists=true;bad.data.resize(40);std::memcpy(bad.data.data(),&next,40);
        if(fault==0)bad.short_write=true;
        if(fault==1)bad.write_result=FR_DISK_ERR;
        if(fault==2)bad.close_result=FR_DISK_ERR;
        if(fault==3)bad.read_result=FR_DISK_ERR;
        if(fault==4)bad.seek_result=FR_DISK_ERR;
        armed=true;CHECK(target.WriteSleepMarker(marker,&armed)!=0&&!armed&&bad.attempted_replay==0);
        CHECK(fault<=2 ? bad.device.quarantined && bad.writes==1 : bad.writes==0);
    }
    std::printf("PASS %u actual sleep-marker creation, foreign-file preservation and I/O fault checks\n",checks);
}
