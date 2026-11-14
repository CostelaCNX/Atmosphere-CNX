"""Exercise the USB-backed NCA file range handlers against a removable-volume fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("output", type=Path)
out = p.parse_args().output.resolve()
out.mkdir(parents=True, exist_ok=True)
tests = Path(__file__).resolve().parent
sources = tests.parent.parent / "libraries/libstratosphere/source/usb"
fixture = tests / "usb_file_range_tests.cpp"
methods = {
    "file_op": ("usb_filesystem.cpp", "virtual Result DoOperateRange("),
    "facade_invalidate": ("usb_filesystem.cpp", "Result Service::InvalidateReadCacheAtGeneration("),
    "service_invalidate": ("usb_storage.cpp", "Result Service::InvalidateReadCacheAtGeneration("),
    "volume_invalidate": ("usb_writable_volume.cpp", "Result WritableVolume::InvalidateReadCache("),
    "close_readers": ("usb_writable_volume.cpp", "Result CloseReaders()"),
    "discard_sectors": ("usb_fatfs_diskio.cpp", "void DiscardCleanSectors()"),
}
def pins():
    paths = {sources / name for name, _ in methods.values()} | {Path(__file__).resolve(), fixture}
    return {str(f).replace('/mnt/c/', 'C:/'): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(paths)}

frozen = pins()
for label, (name, method) in methods.items():
    content = (sources / name).read_text()
    start = content.index("{", content.index(method))
    depth = 1
    mark = start + 1
    while depth:
        depth += int(content[mark] == "{") - int(content[mark] == "}")
        mark += 1
    (out / (label + ".inc")).write_text(content[start:mark])
checks = []
for optimization in ["-O0", "-O2"]:
    exe = out / ("usb-file-range" + optimization)
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", optimization,
                    "-I" + str(out), str(fixture), "-o", str(exe)], check=True)
    checks.extend(subprocess.check_output([str(exe)], text=True).splitlines())
assert pins() == frozen
(out / "range-test-receipt.json").write_text(json.dumps({"passed": True, "checks": checks,
    "files": frozen, "scope": "Actual file, facade, storage, writable-volume range methods and reader close loop. Simulated FAT handles, power admission and media generation."}, indent=2))
for check in checks:
    print(check)
