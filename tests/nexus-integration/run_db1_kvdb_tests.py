#!/usr/bin/env python3
"""Build real DB1 KVDB template and legacy archive codec against a fault-injection filesystem."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='g++')
args = parser.parse_args()
cxx = shutil.which(args.cxx)
if not cxx:
    parser.error('Host g++ required')
lib = root / 'libraries/libstratosphere'
testdir = root / 'tests/nexus-integration'
with tempfile.TemporaryDirectory(prefix='nexus-db1-kvdb-') as tmp:
    exe = Path(tmp) / ('kvdb-tests.exe' if os.name == 'nt' else 'kvdb-tests')
    command = [cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
               '-I' + str(testdir / 'db1_kvdb_host'), '-I' + str(lib / 'include'),
               str(testdir / 'db1_kvdb_tests.cpp'), str(lib / 'source/kvdb/kvdb_archive.cpp'), '-o', str(exe)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=90)
    print(result.stdout, end='')
    print(result.stderr, end='')
    (testdir / 'DB1-kvdb-tests.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    matched = re.search(r'PASS (\d+) checks', result.stdout)
    if not matched:
        raise RuntimeError('Missing test completion count')
    sources = [lib / 'include/stratosphere/kvdb/kvdb_memory_key_value_store.hpp',
               lib / 'include/stratosphere/kvdb/kvdb_file_archive.hpp',
               lib / 'source/kvdb/kvdb_archive.cpp', testdir / 'db1_kvdb_tests.cpp', Path(__file__)]
    sources += list((testdir / 'db1_kvdb_host').rglob('*.hpp'))
    receipt = {'passed': True, 'checks': int(matched.group(1)), 'hardware_tested': False, 'drive_writes': False,
               'scope': 'Production KVDB template, streaming reader/writer and legacy archive codec; stub filesystem and tracked allocator only.',
               'source_pins': {str(p.relative_to(root)).replace('\\', '/'): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}
    (testdir / 'DB1-kvdb-test-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
