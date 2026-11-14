"""Exercise the firmware's game request instrumentation and actual report formatter."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

tests = Path(__file__).resolve().parent
module = tests.parent.parent / 'stratosphere/ams_mitm/source'
header = module / 'usb_mitm/usb_game_wake_trace.hpp'
source = module / 'fs_mitm/fsmitm_domain_filesystem.cpp'
fixture = tests / 'usb_game_wake_trace_tests.cpp'
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
files = [header, source, fixture, Path(__file__).resolve()]
def pins():
    return {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
frozen = pins()
production = source.read_text()
start = production.index('namespace ams::mitm::usb_storage::game_wake {')
stop = production.index('namespace ams::mitm::fs {', start)
(out / 'actual_game_wake_format.hpp').write_text(production[start:stop])
checks = []
for opt in ('O0', 'O2'):
    binary = out / ('game-trace-' + opt)
    subprocess.run(['g++', '-std=c++20', '-' + opt, '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I' + str(out), '-I' + str(header.parent), str(fixture), '-o', str(binary)], check=True)
    test = subprocess.run([str(binary)], capture_output=True, text=True, check=True, timeout=60)
    print(opt, test.stdout, flush=True)
    checks.append({'optimization': opt, 'output': test.stdout.splitlines()})
assert pins() == frozen
(out / 'game-trace-review.json').write_text(json.dumps({'passed': True, 'build': 'SL7', 'inputs': frozen,
    'checks': checks, 'hardware_tested': False}, indent=2) + '\n')
