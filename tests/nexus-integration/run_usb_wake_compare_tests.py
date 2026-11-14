"""Exercise the production SL7 wake comparison against controlled storage handles."""
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
src = tests.parent.parent / "stratosphere/ams_mitm/source"
fixture = tests / "usb_wake_compare_tests.cpp"


def pins(paths):
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(set(paths))}


def body(path, signature, destination, after=""):
    code = path.read_text()
    pos = code.index(signature, code.index(after))
    first = code.index("{", pos)
    depth = 1
    end = first + 1
    while depth:
        depth += int(code[end] == "{") - int(code[end] == "}")
        end += 1
    (out / destination).write_text(code[first:end] + "\n")


def build_and_run(paths, frozen):
    checks = []
    for optimization in ("-O0", "-O2"):
        exe = out / ("usb-wake-compare" + optimization)
        subprocess.run(["g++", "-std=c++20", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                        "-pthread", optimization, "-I" + str(out), "-I" + str(src / "usb_mitm"),
                        str(fixture), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60)
        print(optimization, run.stdout, flush=True)
        checks.append({"optimization": optimization, "output": run.stdout.splitlines()})
    assert frozen == pins(paths)
    (out / "compare-review.json").write_text(json.dumps({
        "passed": True, "build": "SL7", "inputs": frozen, "checks": checks, "hardware_tested": False,
    }, indent=2) + "\n")


module = src / "usb_mitm/usbmitm_module.cpp"
domain = src / "fs_mitm/fsmitm_domain_filesystem.cpp"
service = src / "fs_mitm/fs_mitm_service.cpp"
power = src / "usb_mitm/usb_system_power.cpp"
header = src / "usb_mitm/usb_wake_compare.hpp"
pin_header = src / "usb_mitm/usb_romfs_probe.hpp"
trace_header = src / "usb_mitm/usb_game_wake_trace.hpp"
paths = [fixture, Path(__file__).resolve(), module, domain, service, power, header, pin_header, trace_header]
frozen = pins(paths)
body(domain, "Result Read(s64 offset,", "domain_read.inc", "class DomainStorage final")
body(power, "void Recover(void *)", "recover.inc")
body(module, "Result OpenPinnedProcessRomFs(", "original_open.inc")
body(module, "void QueueWakeComparison(", "queue.inc")
body(module, "void RunWakeComparison() {", "consume.inc")
body(module, "int wake_compare::Format(", "format.inc")
body(module, "struct Comparison {", "comparison.inc")
build_and_run(paths, frozen)
