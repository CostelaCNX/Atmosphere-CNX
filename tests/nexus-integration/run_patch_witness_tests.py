"""Run the real SL7 metadata input observer, lifecycle, comparisons, monitor and report."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

tests = Path(__file__).resolve().parent
src = tests.parents[1]
lib = src / "libraries/libstratosphere/source/fssystem"
module = src / "stratosphere/ams_mitm/source/usb_mitm/usbmitm_module.cpp"
driver = lib / "fssystem_nca_file_system_driver.cpp"
header = lib / "fssystem_patch_witness.hpp"
fixture = tests / "patch_witness_tests.cpp"
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
inputs = [Path(__file__).resolve(), fixture, module, driver, header]

def pins():
    return {str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}

def extract(text, start, stop, dest):
    i = text.index(start)
    (out / dest).write_text(text[i:text.index(stop, i)] + "\n")

frozen = pins()
usb = module.read_text()
nca = driver.read_text()
extract(usb, "namespace pw = fssystem::patch_witness;", "os::SdkMutex g_launch_snapshot_mutex", "witness_state.inc")
extract(usb, "void RunPatchWitness() {", "\n    }\n\n    int wake_compare::Format", "monitor_report.inc")
extract(nca, "class ObservedPatchStorage final", "constexpr inline s32 AesCtrExTableCacheBlockSize", "observer.inc")
extract(nca, "const s64 encrypted_size = meta_data_hash_data_offset", "/* Create the decrypted storage. */", "raw_capture.inc")
extract(nca, "if (auto *sample = patch_witness::Find(m_reader.get());", "/* Set the out layer info storage", "normal_hash_check.inc")
checks = []
for opt in ("-O0", "-O2"):
    exe = out / ("patch-witness" + opt)
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", opt,
                    "-I" + str(lib), "-I" + str(out), str(fixture), "-lcrypto", "-o", str(exe)], check=True)
    result = subprocess.run([str(exe)], check=True, timeout=45, capture_output=True, text=True)
    print(opt, result.stdout, flush=True)
    checks.append({"optimization": opt, "output": result.stdout.splitlines()})
assert frozen == pins()
(out / "witness-review.json").write_text(json.dumps({"passed": True, "build": "SL7", "inputs": frozen,
    "checks": checks, "hardware_tested": False, "scope": "Production observer, allocation fallback, actual verification condition, launch pin, wake probe and report. Controlled storage, simulated decryption, real SHA-256."}, indent=2) + "\n")
