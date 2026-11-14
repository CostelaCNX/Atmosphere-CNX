"""Test actual native Register and targeted title-map cleanup bodies."""
from pathlib import Path
import hashlib,json,shutil,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
src=root/'libraries/libstratosphere/source/ncm'
fixture=root/'tests/nexus-integration/u1_ownership_tests.cpp'
def extract(text,signature):
    start=text.index(signature);opening=text.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
native=src/'ncm_native_sd_content_storage.hpp'
mapping=src/'ncm_usb_title_map.cpp'
files=[native,mapping,fixture,Path(__file__)]
pins={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
with tempfile.TemporaryDirectory(prefix='nexus-u1-ownership-') as folder:
    tmp=Path(folder)
    (tmp/'u1_native_registration.inc').write_text(extract(native.read_text(),'    class NativeSdContentStorage')+';\n')
    (tmp/'u1_title_unmap.inc').write_text(extract(mapping.read_text(),'    Result UnregisterUsbTitle(')+'\n')
    runs=[]
    for flags in (['-O0'],['-O2','-fsanitize=undefined','-fno-sanitize-recover=all']):
        exe=tmp/'ownership-tests'
        subprocess.run([shutil.which('g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I'+str(tmp),str(fixture),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()
        runs.append({'flags':flags,'output':result.stdout})
for p in files:assert hashlib.sha256(p.read_bytes()).hexdigest()==pins[p.relative_to(root).as_posix()],'Source changed during test; rerun'
(root/'tests/nexus-integration/U1-ownership-test-receipt.json').write_text(json.dumps({'passed':True,'hardware_tested':False,'scope':'Actual NativeSdContentStorage class and UnregisterUsbTitle body; storage register, durable map I/O and cache functions are instrumented host shims.','runs':runs,'source_pins':pins},indent=2)+'\n')
