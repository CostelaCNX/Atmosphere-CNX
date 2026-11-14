"""Exercise the real marker writer and raw FAT32/exFAT readers on RAM images."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

project = Path(__file__).resolve().parents[2]
library = project / "libraries/libstratosphere"
usb = library / "source/usb"
parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
out = parser.parse_args().output.resolve()
out.mkdir(parents=True, exist_ok=True)
fat = out / "fatfs"
fat.mkdir(exist_ok=True)
for path in (library / "usb_fatfs").iterdir():
    if path.is_file():
        shutil.copyfile(path, fat / path.name)
config = fat / "ffconf.h"
# Enable only generation of the fixture images in this test-only FatFs copy.
text, edits = re.subn(r"(#define FF_USE_MKFS\s+)0", r"\g<1>1", config.read_text())
assert edits == 1
config.write_text(text)
header = out / "stratosphere/usb"
header.mkdir(parents=True, exist_ok=True)
(out / "vapours.hpp").write_text(r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
using u8=std::uint8_t; using u16=std::uint16_t; using u32=std::uint32_t; using u64=std::uint64_t;
using Result=int;
constexpr int ResultSuccess(){return 0;}
#define NON_COPYABLE(C) C(const C&)=delete; C& operator=(const C&)=delete
#define NON_MOVEABLE(C) C(C&&)=delete; C& operator=(C&&)=delete
#define R_RETURN(r) return (r)
#define R_TRY(expr) do { const auto rc_=(expr); if(rc_) return rc_; } while(0)
#define R_UNLESS(c,r) do { if(!(c))return (r); }while(0)
#define R_SUCCEED() return 0
#define R_FAILED(r) ((r)!=0)
#define R_SUCCEEDED(r) ((r)==0)
namespace ams::util {using std::size; template<class T> bool IsPowerOfTwo(T n){return n && !(n & (n-1));}}
namespace ams::usb {
enum {Fail=1}; constexpr int ResultInvalidParameter(){return 1;} constexpr int ResultNotInitialized(){return 2;}
constexpr int ResultFileNotFound(){return 3;} constexpr int ResultAlreadyInitialized(){return 4;}
constexpr int ResultCorruptedFilesystem(){return 5;} constexpr int ResultOperationDenied(){return 6;}
constexpr int ResultUnsupportedFilesystem(){return 7;} constexpr int ResultMemAllocFailure(){return 8;}
constexpr int ResultResourceBusy(){return 9;} constexpr int ResultTransactionError(){return 10;}
}
''')
(header / "usb_mass_storage.hpp").write_text(r'''
#pragma once
#include <vapours.hpp>
namespace ams::usb::mass_storage {
struct Geometry {u64 block_count; u32 block_size;};
struct Device {
    Geometry geometry{131072,512}; bool quarantined{};
    bool IsInitialized() const{return true;}
    const Geometry &GetGeometry() const{return geometry;}
    void Quarantine(){quarantined=true;}
    Result ReadBlocks(u64,u32,void*,size_t);
};
}
''')
(out / "stratosphere.hpp").write_text('#include <stratosphere/usb/usb_volume.hpp>\n')
source = (usb / "usb_writable_volume.cpp").read_text()
helpers = source[source.index("        constexpr size_t FatPathBufferSize"):source.index("        Result MakeFatPathPair")]
handles = source[source.index("        struct ScopedFile"):source.index("    struct WritableVolume::Impl")]
start = source.index("    Result WritableVolume::WriteSleepMarker(")
end = source.index("\n    }", start) + len("\n    }")
writer = source[start:end]
(out / "actual_writer.cpp").write_text(r'''
#include <stratosphere.hpp>
#include "ff.h"
namespace ams::usb::volume { namespace {
''' + helpers + handles + r'''
struct WritableVolume::Impl { FATFS fs{}; struct {ScopedFile handle;} writer; mass_storage::Device *device{};};
Result WritableVolume::Initialize(mass_storage::Device *device,const Partition&){
  m_impl=new Impl; m_impl->device=device; return TranslateFatResult(f_mount(&m_impl->fs,"0:",1));
}
Result WritableVolume::Finalize(){auto rc=f_mount(nullptr,"0:",0);delete m_impl;m_impl=nullptr;return TranslateFatResult(rc);}
WritableVolume::~WritableVolume(){if(m_impl) Finalize();}
''' + writer + "\n}\n")
inputs = [Path(__file__), project / "tests/nexus-integration/usb_wake_volume_images.cpp", usb / "usb_writable_volume.cpp",
          usb / "usb_volume.cpp", usb / "usb_partition_probe.hpp",
          library / "include/stratosphere/usb/usb_volume.hpp",
          library / "include/stratosphere/usb/usb_resume_marker.hpp", *(library / "usb_fatfs").glob("*")]
inputs = [f for f in inputs if f.is_file()]
def pins():
    return {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
frozen = pins()
objects = []
for file in ("ff.c","ffunicode.c","ffsystem.c"):
    obj=out/(file+".o")
    subprocess.run(["gcc","-O2","-c",str(fat/file),"-o",str(obj)],check=True)
    objects.append(str(obj))
exe = out / "wake-volume-images"
subprocess.run(["g++","-O2","-std=c++20","-DATMOSPHERE_OS_HORIZON","-I"+str(out),
                "-I"+str(fat),"-I"+str(library/"include"),str(usb/"usb_volume.cpp"),
                str(out/"actual_writer.cpp"),str(project/"tests/nexus-integration/usb_wake_volume_images.cpp"),
                *objects,"-o",str(exe)],check=True)
result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
assert frozen==pins()
(out/"marker-volume-review.json").write_text(json.dumps({"passed":True,"checks":result.stdout.splitlines(),
    "inputs":frozen,"scope":"Real FatFs, SL2 marker writer, raw partition discovery and raw FAT32/exFAT readers. RAM disk and deterministic nonce. No USB hardware."},indent=2))
print(result.stdout,end="")
