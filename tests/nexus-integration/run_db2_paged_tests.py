#!/usr/bin/env python3
"""Run production DB2 store with a deterministic transactional filesystem model."""
from pathlib import Path
import hashlib,json,os,subprocess,tempfile,shutil
root=Path(__file__).resolve().parents[2]
tests=root/'tests/nexus-integration'
lib=root/'libraries/libstratosphere'
compiler=shutil.which(os.environ.get('CXX','g++'))
if not compiler: raise SystemExit('A host C++20 compiler is required')
env=dict(os.environ)
with tempfile.TemporaryDirectory(prefix='nexus-db2-paged-') as d:
    exe=Path(d)/'db2-tests.exe'
    command=[compiler,'-std=c++20','-O1','-Wall','-Wextra','-Werror',
        '-I'+str(tests/'db2_paged_host'),'-I'+str(lib/'include'),
        str(tests/'db2_paged_tests.cpp'),str(lib/'source/ncm/ncm_paged_content_meta_store.cpp'),'-o',str(exe)]
    built=subprocess.run(command,env=env,capture_output=True,text=True)
    (tests/'DB2-paged-compile.log').write_text(built.stdout+built.stderr)
    if built.returncode:print(built.stdout+built.stderr);built.check_returncode()
    result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=240)
    (tests/'DB2-paged-tests.log').write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr,end='');result.check_returncode()
    pins=[lib/'include/stratosphere/ncm/ncm_paged_content_meta_store.hpp',lib/'source/ncm/ncm_paged_content_meta_store.cpp',tests/'db2_paged_tests.cpp',Path(__file__)]+[p for p in (tests/'db2_paged_host').rglob('*') if p.is_file()]
    receipt={'passed':True,'hardware_tested':False,'drive_writes':False,
        'scope':'Actual production PagedContentMetaStore; mocked atomic save-data filesystem, handles, quotas and extension. Restart model discards uncommitted filesystem changes and closes process handles; ordinary Result failures require implementation cleanup. This does not prove physical power-loss behavior or Horizon filesystem semantics.',
        'source_pins':{str(p.relative_to(root)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for p in pins}}
    (tests/'DB2-paged-test-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
