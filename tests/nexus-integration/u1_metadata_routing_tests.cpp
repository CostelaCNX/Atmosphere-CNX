#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include "ncm_paged_content_meta_database_impl.hpp"
#include "ncm_content_meta_database_impl.hpp"
using namespace ams;
using namespace ams::ncm;
namespace {
size_t checks{},cases{};
void Check(bool value,const char*what){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",what);std::abort();}}
ContentId Id(u8 n){ContentId id{};id.data[0]=n;return id;}
std::vector<u8> Metadata(){
    ContentMetaHeader header{sizeof(ApplicationMetaExtendedHeader),3,0,0,ContentMetaPlatform::Nx};
    std::vector<u8>bytes(sizeof(header)+header.extended_header_size+3*sizeof(ContentInfo));
    ContentInfo contents[3]{{Id(1),100,0,ContentType::Meta,0},{Id(2),100,0,ContentType::Control,0},{Id(3),100,0,ContentType::Program,0}};
    std::memcpy(bytes.data(),&header,sizeof(header));
    std::memcpy(bytes.data()+sizeof(header)+header.extended_header_size,contents,sizeof(contents));
    return bytes;
}
struct Database {
    kvdb::MemoryKeyValueStore<ContentMetaKey> memory;
    ContentMetaDatabaseImpl legacy;
    PagedContentMetaDatabaseImpl paged;
    bool use_paged;
    Database(bool p,StorageId storage):legacy(&memory,"db",storage),paged(storage),use_paged(p){Check(paged.Initialize("db:/","db",fs::SaveDataSpaceId::SdSystem,1)==0,"initialize");}
    Result Set(const ContentMetaKey&key,const std::vector<u8>&bytes){return use_paged?paged.Set(key,{bytes.data(),bytes.size()}):legacy.Set(key,{bytes.data(),bytes.size()});}
    Result Commit(){return use_paged?paged.Commit():legacy.Commit();}
};
void Reset(){usb_enabled=true;usb_ready=true;usb_install=true;registered.fill(false);registered_this_boot.fill(false);mappings=unmaps=0;fs::fail_commit=false;mapping_failed=false;existing_title_mapped=false;existing_map_result={};PagedContentMetaStore::ResetFaults();}
void Evidence(unsigned mask,unsigned boot){for(unsigned i=1;i<=3;++i){registered[i]=(mask&(1u<<(i-1)))!=0;registered_this_boot[i]=(boot&(1u<<(i-1)))!=0;}}
const ContentMetaKey key{0x05004e584d470000,1,ContentMetaType::Application,ContentInstallType::Full,{}};
void DestinationCases(){
    for(bool paged:{false,true})for(bool mounted:{false,true})for(bool install:{false,true})for(unsigned mask:{0u,1u,3u,7u})for(unsigned boot:{0u,1u,7u}){
        Reset();usb_ready=mounted;usb_install=install;Evidence(mask,boot);Database db(paged,StorageId::SdCard);auto bytes=Metadata();++cases;
        Check(db.Set(key,bytes)==0,"metadata Set succeeds");
        Check(mappings==0&&unmaps==0,"Set never publishes routing");
        Check(db.Commit()==0,"metadata Commit succeeds");
        const bool expected_usb=mask==7&&boot!=0;
        Check(mappings==(expected_usb?1:0),"only all registered content plus current-boot proof maps USB; mounted/install flags alone cannot");
        Check(unmaps==(expected_usb?0:1),"native metadata removes prior title route even while USB mounted");
        if(expected_usb)Check(mapped_title==key.id&&mapped_meta==Id(1)&&mapped_control==Id(2)&&mapped_program==Id(3),"exact registered content IDs published");
        Check(db.Commit()==0&&mappings==(expected_usb?1:0)&&unmaps==(expected_usb?0:1),"queues drain after success");
    }
}
void DurabilityCases(){
    for(bool paged:{false,true})for(bool usb_destination:{false,true}){
        Reset();Evidence(usb_destination?7:0,usb_destination?1:0);Database db(paged,StorageId::SdCard);++cases;
        Check(db.Set(key,Metadata())==0,"queue durable operation");fs::fail_commit=true;
        Check(db.Commit()==900&&mappings==0&&unmaps==0,"failed durable metadata commit cannot change title routing");
        fs::fail_commit=false;Check(db.Commit()==0,"retry durable commit");
        Check(mappings==(usb_destination?1:0)&&unmaps==(usb_destination?0:1),"retry publishes only intended destination");
    }
    for(bool paged:{false,true}){
        Reset();Evidence(7,1);Database db(paged,StorageId::SdCard);++cases;
        Check(db.Set(key,Metadata())==0,"queue USB with proof");registered_this_boot.fill(false);
        Check(db.Commit()!=0&&mappings==0,"USB proof revoked before commit prevents stale publication");
    }
}
void NativeNandAndNonApplicationCases(){
    for(bool paged:{false,true}){
        Reset();Evidence(7,7);Database db(paged,StorageId::BuiltInUser);++cases;
        Check(db.Set(key,Metadata())==0&&db.Commit()==0,"NAND metadata commits normally");
        Check(mappings==0&&unmaps==0,"NAND metadata never touches USB routing");
    }
    for(bool paged:{false,true}){
        Reset();Evidence(7,7);Database db(paged,StorageId::SdCard);auto patch=key;patch.type=ContentMetaType::Patch;++cases;
        Check(db.Set(patch,Metadata())==0&&db.Commit()==0,"nonapplication metadata commits");
        Check(mappings==0&&unmaps==0,"nonapplication metadata never creates an application USB route");
    }
}
void LatestSetWins(){
    for(bool paged:{false,true})for(bool final_usb:{false,true}){
        Reset();Evidence(final_usb?0:7,final_usb?0:1);Database db(paged,StorageId::SdCard);++cases;
        Check(db.Set(key,Metadata())==0,"initial pending destination");
        Evidence(final_usb?7:0,final_usb?1:0);
        Check(db.Set(key,Metadata())==0,"replacement pending destination");
        Check(db.Commit()==0,"changed destination commits");
        Check(mappings==(final_usb?1:0)&&unmaps==(final_usb?0:1),"latest successful Set replaces opposite pending mapping");
    }
}
void ExistingUsbPreservation(){
    for(bool paged:{false,true})for(int variation=0;variation<7;++variation){
        Reset();Database db(paged,StorageId::SdCard);auto bytes=Metadata();Evidence(7,1);
        if(variation!=1)Check(db.Set(key,bytes)==0&&db.Commit()==0,"seed existing metadata record");
        mappings=unmaps=0;registered_this_boot.fill(false);
        existing_title_mapped=variation!=2;existing_title=key.id;existing_meta=Id(1);existing_control=Id(2);existing_program=Id(3);
        if(variation==3)existing_program=Id(9);
        if(variation==4)registered[2]=false;
        if(variation==5)bytes[6]^=1; // Same content IDs, different actual metadata payload.
        auto candidate=key;if(variation==6)candidate.version=2;
        ++cases;Check(db.Set(candidate,bytes)==0&&db.Commit()==0,"post-reboot metadata operation commits");
        Check(mappings==0,"preservation never creates or refreshes a USB map without fresh registration");
        Check(unmaps==(variation==0?0:1),"only same existing key, bytes, title IDs and all locations preserve previous USB map");
    }
    for(bool paged:{false,true})for(bool queued_usb:{false,true}){
        Reset();Database db(paged,StorageId::SdCard);auto bytes=Metadata();Evidence(7,1);
        Check(db.Set(key,bytes)==0&&db.Commit()==0,"seed preserved record");mappings=unmaps=0;
        existing_title_mapped=true;existing_title=key.id;existing_meta=Id(1);existing_control=Id(2);existing_program=Id(3);
        Evidence(queued_usb?7:0,queued_usb?1:0);Check(db.Set(key,bytes)==0,"queue earlier destination");
        Evidence(7,0);Check(db.Set(key,bytes)==0&&db.Commit()==0,"latest operation preserves unchanged prior mapping");++cases;
        Check(mappings==0&&unmaps==0,"preserve decision cancels earlier USB or native pending work");
    }
    {
        Reset();Database db(true,StorageId::SdCard);auto bytes=Metadata();Evidence(7,1);Check(db.Set(key,bytes)==0&&db.Commit()==0,"seed record before read fault");
        mappings=unmaps=0;Evidence(7,0);existing_title_mapped=true;existing_title=key.id;existing_meta=Id(1);existing_control=Id(2);existing_program=Id(3);
        PagedContentMetaStore::ResetFaults();PagedContentMetaStore::fail_read_at=0;++cases;
        Check(db.Set(key,bytes)==905,"old metadata comparison I/O error returned before Set");PagedContentMetaStore::ResetFaults();
        Check(db.Commit()==0&&mappings==0&&unmaps==0,"failed preservation read queues no destination mutation");
    }
}
}
int main(){DestinationCases();DurabilityCases();NativeNandAndNonApplicationCases();LatestSetWins();ExistingUsbPreservation();std::printf("PASS U1 metadata routing: %zu cases, %zu assertions (legacy and paged production bodies)\n",cases,checks);}
