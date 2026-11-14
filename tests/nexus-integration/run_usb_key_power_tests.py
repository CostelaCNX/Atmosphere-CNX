"""Exercise the actual NCA key cache and decryptor against simulated LP0 slot loss."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

tests = Path(__file__).resolve().parent
tree = tests.parents[1]
lib = tree / "libraries/libstratosphere/source/fssystem"
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
fixture = tests / "usb_hardware_key_power_tests.cpp"
crypto = lib / "fssystem_crypto_configuration.cpp"
power = tree / "stratosphere/ams_mitm/source/usb_mitm/usb_system_power.cpp"
files = [fixture, lib / "fssystem_key_slot_cache.hpp", lib / "fssystem_hardware_key_power.hpp", crypto, power, Path(__file__)]
def pins():
    return {str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}

def definition(text, name):
    start = text.index(name)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

original = pins()
src = crypto.read_text()
functions = ["void DecryptAesCtr(", "void DecryptAesCtrForPreparedKey(", "void BeginHardwareKeySleep(", "bool EndHardwareKeySleep("]
(out / "crypto_functions.inc").write_text("\n".join(definition(src, fn) for fn in functions))
(out / "stratosphere.hpp").write_text("#pragma once\n")
psc = power.read_text()
recover = definition(psc, "void Recover(void *)")
assert recover.index("RestoreHardwareKeyCache();") < recover.index("->ResumeSystemSleep()")
prepare = definition(psc, "u32 PrepareStorage()")
assert prepare.index("->PrepareSystemSleep()") < prepare.index("if (R_SUCCEEDED(result)) fssystem::BeginHardwareKeySleep()")
checks = []
for level in ("-O0", "-O2"):
    exe = out / ("key-power" + level)
    subprocess.run(["g++", "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror", level,
                    "-I" + str(out), "-I" + str(lib), str(fixture), "-o", str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=20)
    print(level, run.stdout.strip())
    checks.append({"optimization": level, "output": run.stdout.splitlines()})
assert original == pins()
(out / "key-power-review.json").write_text(json.dumps({"build": "SL7", "passed": True, "hardware_tested": False,
    "inputs": original, "checks": checks}, indent=2) + "\n")
