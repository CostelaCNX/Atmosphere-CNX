"""Reproduce the legacy-forwarder regression against frozen DB2 and fixed DB2R1."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='g++')
parser.add_argument('--before', type=Path, required=True, help='Original DB2 Atmosphere source root for the negative control')
args = parser.parse_args()
compiler = shutil.which(args.cxx)
if not compiler:
    parser.error('A host C++20 compiler is required')
host = root/'tests/nexus-integration/db2_metadata_host'
fixture = root/'tests/nexus-integration/db2r1_legacy_repro.cpp'
logs = []
pins = {}
for label, tree, expected in [('before', args.before, 390), ('after', root, 0)]:
    src = tree/'libraries/libstratosphere/source/ncm'
    with tempfile.TemporaryDirectory(prefix='nexus-db2r1-repro-') as folder:
        tmp = Path(folder)
        shutil.copy2(host/'ncm_lookup_trace.hpp', tmp/'ncm_lookup_trace.hpp')
        units = ['ncm_paged_content_meta_database_impl.cpp', 'ncm_content_meta_database_impl.cpp']
        for name in units:
            shutil.copy2(src/name, tmp/name)
        exe = tmp/('repro.exe' if os.name == 'nt' else 'repro')
        subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
            '-I'+str(host), '-I'+str(src), str(fixture), *[str(tmp/n) for n in units], '-o', str(exe)], check=True)
        result = subprocess.run([str(exe), str(expected)], capture_output=True, text=True, timeout=30)
        log = f'{label.upper()}: {tree.name}\n'+result.stdout+result.stderr
        print(log, end='')
        (root/f'tests/nexus-integration/DB2R1-legacy-{label}.log').write_text(log)
        result.check_returncode()
        logs.append(log)
        for path in [src/n for n in units]+[src/'ncm_content_meta_range_reader.hpp', src/'ncm_paged_content_meta_database_impl.hpp']:
            pins[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
for path in [fixture, Path(__file__)]+list(host.rglob('*.hpp')):
    pins[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
(root/'tests/nexus-integration/DB2R1-legacy-repro-receipt.json').write_text(json.dumps({
    'passed': True, 'hardware_tested': False,
    'scope': 'Actual frozen DB2 and DB2R1 service bodies and range readers with host IPC/backend/wire shims; exact known 96-byte legacy-forwarder fixture.',
    'source_pins': pins}, indent=2)+'\n')
