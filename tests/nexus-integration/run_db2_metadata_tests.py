"""Compile unchanged DB2 and legacy metadata service bodies with host IPC/store shims."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, tempfile

root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx',default='g++')
args=parser.parse_args()
compiler=shutil.which(args.cxx)
if not compiler: parser.error('A host C++20 compiler is required')
src=root/'libraries/libstratosphere/source/ncm'
testdir=root/'tests/nexus-integration'
host=testdir/'db2_metadata_host'
production=['ncm_paged_content_meta_database_impl.cpp','ncm_paged_content_meta_database_impl.hpp',
 'ncm_content_meta_range_reader.hpp','ncm_content_meta_database_impl.cpp',
 'ncm_content_meta_database_impl.hpp','ncm_content_meta_database_impl_base.hpp','ncm_usb_metadata_mapping.hpp']
with tempfile.TemporaryDirectory(prefix='nexus-db2-metadata-') as tmpdir:
    tmp=Path(tmpdir)
    # Local quote include shadows tracing only. Service function bodies are copied
    # byte-for-byte; the real production headers and USB metadata validator compile.
    shutil.copy2(host/'ncm_lookup_trace.hpp',tmp/'ncm_lookup_trace.hpp')
    for name in ('ncm_paged_content_meta_database_impl.cpp','ncm_content_meta_database_impl.cpp'):
        shutil.copy2(src/name,tmp/name)
    exe=tmp/('metadata-tests.exe' if os.name=='nt' else 'metadata-tests')
    command=[compiler,'-std=c++20','-O1','-Wall','-Wextra','-Werror','-I'+str(host),'-I'+str(src),
      str(testdir/'db2_metadata_tests.cpp'),str(tmp/'ncm_paged_content_meta_database_impl.cpp'),
      str(tmp/'ncm_content_meta_database_impl.cpp'),'-o',str(exe)]
    subprocess.run(command,check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
    print(result.stdout,end='');print(result.stderr,end='')
    (testdir/'DB2-metadata-tests.log').write_text(result.stdout+result.stderr)
    result.check_returncode()
    files=[src/name for name in production]+list(host.rglob('*.hpp'))+[Path(__file__),testdir/'db2_metadata_tests.cpp']
    receipt={'passed':True,'hardware_tested':False,'scope':'Production DB2 and legacy service function bodies, range reader and USB mapping validator, with host wire/IPC/backend/trace shims.',
      'source_pins':{p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (testdir/'DB2-metadata-test-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
