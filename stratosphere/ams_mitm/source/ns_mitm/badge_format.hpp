// SPDX-License-Identifier: MIT
// Shared, byte-for-byte cache contract. No device IDs, keys or game data.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace nexus_badge {
constexpr std::size_t NacpSize=0x4000, MaxJpeg=0x20000;
constexpr char Directory[]="/atmosphere/usb_badges";
struct Header {
    char magic[8];
    std::uint32_t version, header_size;
    std::uint64_t title;
    std::uint32_t original_size, jpeg_size, dimension, flags;
    std::uint8_t original_hash[32], jpeg_hash[32];
};
static_assert(sizeof(Header)==104);
inline bool Title(std::uint64_t id) {
    return (id>=0x0100000000010000ULL && id<0x0200000000000000ULL) ||
           (id>=0x0500000000010000ULL && id<0x0600000000000000ULL);
}
inline bool Valid(const Header& h,std::uint64_t id,std::size_t file_size) {
    return std::memcmp(h.magic,"NXUBDG1",8)==0 && h.version==1 && h.header_size==sizeof(Header) &&
        Title(id) && h.title==id && h.original_size>=4 && h.original_size<=MaxJpeg &&
        h.jpeg_size>=4 && h.jpeg_size<=MaxJpeg && file_size==sizeof(Header)+h.jpeg_size &&
        (h.dimension==256 || h.dimension==174) && h.flags>=1 && h.flags<=3;
}
// Strict baseline JPEG header: bounded segment traversal, dimensions before SOS.
// Full pixel decoding is performed by the app, never on HOME's IPC thread.
inline bool Jpeg(const std::uint8_t* p,std::size_t n,std::uint32_t dimension) {
    if(!p || n<4 || p[0]!=0xff || p[1]!=0xd8 || p[n-2]!=0xff || p[n-1]!=0xd9)return false;
    std::size_t at=2;bool frame=false;
    for(unsigned segments=0;segments<256 && at<n-2;++segments) {
        if(p[at++]!=0xff)return false;
        while(at<n && p[at]==0xff)++at;
        if(at>=n)return false;
        const auto marker=p[at++];
        if(marker==0 || marker==0xd8 || marker==0xd9 || (marker>=0xd0 && marker<=0xd7))return false;
        if(n-at<2)return false;
        const std::size_t len=(p[at]<<8)|p[at+1];
        if(len<2 || len>n-at)return false;
        if(marker==0xc0) {
            if(frame || len!=17 || p[at+2]!=8 || p[at+7]!=3 ||
                static_cast<std::uint32_t>((p[at+3]<<8)|p[at+4])!=dimension ||
                static_cast<std::uint32_t>((p[at+5]<<8)|p[at+6])!=dimension)return false;
            frame=true;
        } else if(marker>=0xc1 && marker<=0xcf && marker!=0xc4)return false;
        if(marker==0xda)return frame && len==12 && at+len<n-2;
        at+=len;
    }
    return false;
}
// These are the only control-data wire formats implemented by B1.
inline int SizeOffset(std::uint32_t command,std::size_t response_size) {
    if(command==0 && response_size>=4)return 0;
    if(command==6 && response_size>=8)return 4;
    if(command==19 && response_size>=12)return 8;
    return -1;
}
inline bool Fits(std::size_t actual,std::size_t capacity,std::size_t jpeg_size) {
    return actual>NacpSize && actual<=capacity && actual-NacpSize<=MaxJpeg &&
        jpeg_size>=4 && jpeg_size<=MaxJpeg && capacity>=NacpSize && jpeg_size<=capacity-NacpSize;
}
}
