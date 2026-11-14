// SPDX-License-Identifier: GPL-2.0-or-later
#include <stratosphere.hpp>
#include <cassert>
#include <vector>
#include "ff.h"
#include "diskio.h"
using namespace ams::usb;
static std::vector<u8> media(131072 * 512);
static size_t writes;
DSTATUS disk_initialize(BYTE drive){return drive ? STA_NOINIT : 0;}
DSTATUS disk_status(BYTE drive){return disk_initialize(drive);}
DRESULT disk_read(BYTE drive,BYTE *out,LBA_t lba,UINT count){
    if(drive || !count || lba>media.size()/512 || count>media.size()/512-lba)return RES_PARERR;
    std::memcpy(out,media.data()+lba*512,count*512);return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE *in,LBA_t lba,UINT count){
    if(drive || !count || lba>media.size()/512 || count>media.size()/512-lba)return RES_PARERR;
    ++writes;std::memcpy(media.data()+lba*512,in,count*512);return RES_OK;
}
DRESULT disk_ioctl(BYTE drive,BYTE cmd,void *out){
    if(drive)return RES_PARERR;
    switch(cmd){
        case CTRL_SYNC:return RES_OK;
        case GET_SECTOR_SIZE:*static_cast<WORD*>(out)=512;return RES_OK;
        case GET_SECTOR_COUNT:*static_cast<LBA_t*>(out)=media.size()/512;return RES_OK;
        case GET_BLOCK_SIZE:*static_cast<DWORD*>(out)=1;return RES_OK;
        default:return RES_PARERR;
    }
}
Result mass_storage::Device::ReadBlocks(u64 lba,u32 count,void *out,size_t size){
    return !quarantined && count*512ull<=size && disk_read(0,static_cast<BYTE*>(out),lba,count)==RES_OK ? 0 : 1;
}
static void create(const char *path,const void *bytes,UINT size){
    FIL file{}; UINT n{};
    assert(f_open(&file,path,FA_WRITE|FA_CREATE_ALWAYS)==FR_OK);
    assert(f_write(&file,bytes,size,&n)==FR_OK && n==size);
    assert(f_close(&file)==FR_OK);
}
template<class Reader>
void one_format(BYTE format,DWORD cluster_size,const char *name){
    std::fill(media.begin(),media.end(),0);
    u8 scratch[4096]{};MKFS_PARM opts{};
    opts.fmt=format|FM_SFD;opts.au_size=cluster_size;
    assert(f_mkfs("0:",&opts,scratch,sizeof(scratch))==FR_OK);
    mass_storage::Device device{};
    volume::Partition part[16]{};size_t parts{};
    assert(volume::DiscoverPartitions(&device,part,16,&parts)==0 && parts==1);
    volume::WritableVolume writer; Reader raw;
    assert(writer.Initialize(&device,part[0])==0);
    assert(raw.Initialize(&device,part[0])==0);
    sleep::VolumeMarker current{},old{};
    std::memcpy(current.signature,sleep::MarkerSignature,8);
    std::fill(std::begin(current.challenge),std::end(current.challenge),17);
    std::memset(&old,0x4b,sizeof(old));
    create("0:/GAME.DAT","still here",10);
    create("0:/NEXUS.RSM",&old,sizeof(old));
    bool armed=true;
    assert(writer.WriteSleepMarker(current,&armed)==0 && !armed);
    const auto read_marker=[&](){
        size_t n{};sleep::VolumeMarker value{};volume::FileInfo file{};
        const size_t initial_writes=writes;
        assert(raw.GetFileInfo(sleep::MarkerPath,&file)==0 && file.size==sizeof(value) && !file.directory);
        assert(raw.ReadFile(sleep::MarkerPath,0,&value,sizeof(value),&n)==0 && n==sizeof(value));
        assert(writes==initial_writes);return value;
    };
    auto actual=read_marker();assert(std::memcmp(&actual,&old,sizeof(old))==0);
    assert(f_unlink("0:/NEXUS.RSM")==FR_OK);
    assert(writer.WriteSleepMarker(current,&armed)==0 && armed);
    actual=read_marker();assert(std::memcmp(&actual,&current,sizeof(current))==0);
    const auto before_next_sleep=media;
    std::fill(std::begin(current.challenge),std::end(current.challenge),43);
    assert(writer.WriteSleepMarker(current,&armed)==0 && armed);
    assert(writer.Finalize()==0 && raw.Finalize()==0);
    auto before_probe=writes;
    assert(raw.Initialize(&device,part[0])==0);
    actual=read_marker();assert(std::memcmp(&actual,&current,sizeof(current))==0);
    char game[10];size_t read{};
    assert(raw.ReadFile("/GAME.DAT",0,game,sizeof(game),&read)==0 && read==10 && !std::memcmp(game,"still here",10));
    assert(raw.Finalize()==0);
    media=before_next_sleep; // Same formatted disk, geometry, layout, old challenge.
    assert(raw.Initialize(&device,part[0])==0);
    actual=read_marker();assert(std::memcmp(&actual,&current,sizeof(current))!=0);
    assert(writes==before_probe && !device.quarantined);
    std::printf("PASS %s: foreign file preserved, current challenge readable after remount, stale clone rejected, game data intact, read-only probe issued zero writes\n",name);
}
int main(){
    one_format<volume::Fat32Volume>(FM_FAT32,512,"FAT32");
    one_format<volume::ExFatVolume>(FM_EXFAT,128*1024,"exFAT 128 KiB");
}
