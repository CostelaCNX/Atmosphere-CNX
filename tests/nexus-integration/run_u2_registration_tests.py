"""Exercise actual U1 overlay dispatch with instrumented native/USB backends."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, subprocess, tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='g++')
parser.add_argument('--negative-control', type=Path)
args = parser.parse_args()
compiler = shutil.which(args.cxx)
if not compiler:
    parser.error('A host C++20 compiler is required')
source = root/'stratosphere/ams_mitm/source/usb_mitm/usb_content_filesystem.cpp'
fixture = root/'tests/nexus-integration/u2_registration_tests.cpp'
if args.negative_control: source=args.negative_control
original = source.read_bytes()
text = original.decode()
helpers = text[text.index('        constexpr const char RegisteredDirectory'):text.index('        class UsbContentFile final')]
def extract_class(name):
    start = text.index('        class '+name+' final')
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end+1]
body = extract_class('UsbContentOverlayFileSystem')
# Expose virtual dispatch bodies for direct calls. Routing expressions/bodies
# remain verbatim; file/directory objects are backend tags, not real media I/O.
body = 'class UsbContentOverlayFileSystem ' + body[body.index('{'):]
body = body.replace('private:', 'public:').replace('override final', '')
directory = extract_class('UsbContentOverlayDirectory')
directory = 'class UsbContentOverlayDirectory : public fs::fsa::IDirectory ' + directory[directory.index('{'):]
directory = directory.replace('private:', 'public:').replace('override final', '').replace('override', '')
with tempfile.TemporaryDirectory(prefix='nexus-u1-routing-') as folder:
    tmp = Path(folder)
    (tmp/'u1_helpers.inc').write_text(helpers)
    (tmp/'u1_overlay.inc').write_text(body)
    (tmp/'u1_directory.inc').write_text(directory)
    runs = []
    for flags in (['-O0'], ['-O2', '-fsanitize=undefined', '-fno-sanitize-recover=all']):
        exe = tmp/'routing-tests'
        cmd = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags, *(['-DU1_NEGATIVE'] if args.negative_control else []), '-I'+str(tmp), str(fixture), '-o', str(exe)]
        subprocess.run(cmd, check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
        print(result.stdout, end=''); print(result.stderr, end='')
        result.check_returncode()
        runs.append({'flags': flags, 'output': result.stdout})
assert source.read_bytes() == original, 'Production source changed during test; rerun against final source'
receipt = {'passed': True, 'hardware_tested': False,
    'scope': 'Actual overlay dispatch, merged directory implementation and path/parent/delete helpers; instrumented native/USB backends and tagged file objects. Does not simulate Horizon IPC or physical media.',
    'runs': runs,
    'source_pins': {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, fixture, Path(__file__))}}
(root/('tests/nexus-integration/U2-negative-test-receipt.json' if args.negative_control else 'tests/nexus-integration/U2-registration-test-receipt.json')).write_text(json.dumps(receipt, indent=2)+'\n')
