"""Test real legacy/paged NCM metadata routing against explicit USB evidence."""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, tempfile

root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx',default='g++')
args=parser.parse_args()
compiler=shutil.which(args.cxx)
if not compiler: parser.error('A host C++20 compiler is required')
src=root/'libraries/libstratosphere/source/ncm'
host=root/'tests/nexus-integration/db2_metadata_host'
fixture=root/'tests/nexus-integration/u1_metadata_routing_tests.cpp'
production=['ncm_paged_content_meta_database_impl.cpp','ncm_paged_content_meta_database_impl.hpp',
 'ncm_content_meta_range_reader.hpp','ncm_content_meta_database_impl.cpp',
 'ncm_content_meta_database_impl.hpp','ncm_content_meta_database_impl_base.hpp','ncm_usb_metadata_mapping.hpp']
files=[src/name for name in production]+list(host.rglob('*.hpp'))+[Path(__file__),fixture]
pins={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
with tempfile.TemporaryDirectory(prefix='nexus-u1-metadata-') as folder:
    tmp=Path(folder)
    shim=tmp/'host'
    shutil.copytree(host,shim)
    path=shim/'stratosphere.hpp'
    text=path.read_text()
    before='inline bool IsUsbContentLocationRegistered(ContentId){return true;}inline bool WasUsbContentRegisteredThisBoot(ContentId){return true;}'
    assert before in text, 'Host fixture shape changed; inspect shim before updating'
    text=text.replace(before,'''inline std::array<bool,256> registered{},registered_this_boot{};
inline bool IsUsbContentLocationRegistered(ContentId id){return registered[id.data[0]];}
inline bool WasUsbContentRegisteredThisBoot(ContentId id){return registered_this_boot[id.data[0]];}
inline bool existing_title_mapped=false;
inline u64 existing_title=0;
inline ContentId existing_meta{},existing_control{},existing_program{};
inline Result existing_map_result{};
inline bool IsUsbTitleMapped(ProgramId id){return existing_title_mapped&&id.value==existing_title;}
inline Result GetUsbTitleContentIds(ProgramId id,ContentId*m,ContentId*c,ContentId*p){if(existing_map_result)return existing_map_result;if(!IsUsbTitleMapped(id))return ResultContentNotFound();*m=existing_meta;*c=existing_control;*p=existing_program;return {};}''')
    path.write_text(text)
    shutil.copy2(host/'ncm_lookup_trace.hpp',tmp/'ncm_lookup_trace.hpp')
    for name in ('ncm_paged_content_meta_database_impl.cpp','ncm_content_meta_database_impl.cpp'):
        shutil.copy2(src/name,tmp/name)
    runs=[]
    for flags in (['-O0'],['-O2','-fsanitize=undefined','-fno-sanitize-recover=all']):
        exe=tmp/'metadata-tests'
        cmd=[compiler,'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I'+str(shim),'-I'+str(src),str(fixture),str(tmp/'ncm_paged_content_meta_database_impl.cpp'),str(tmp/'ncm_content_meta_database_impl.cpp'),'-o',str(exe)]
        subprocess.run(cmd,check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
        print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()
        runs.append({'flags':flags,'output':result.stdout})
for p in files:
    assert hashlib.sha256(p.read_bytes()).hexdigest()==pins[p.relative_to(root).as_posix()], 'Source changed during test; rerun'
receipt={'passed':True,'hardware_tested':False,'scope':'Unchanged production legacy/paged NCM metadata service bodies and USB mapping validator; host storage/IPC shims instrument per-content registered/current-boot evidence.','runs':runs,'source_pins':pins}
(root/'tests/nexus-integration/U1-metadata-routing-test-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
