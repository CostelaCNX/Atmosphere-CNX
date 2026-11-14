// SPDX-License-Identifier: GPL-2.0-or-later
// Included in usbmitm_module.cpp's private namespace. The existing monitor is
// the only caller. Never run PM/NS/Loader IPC from the storage worker.
constexpr const char* EjectCommandPath="/logs/usb_eject_command.bin";
constexpr const char* EjectStatusPath="/logs/usb_eject_status.txt";
constexpr const char* EjectTempPath="/logs/usb_eject_status.txt.tmp";
u64 g_eject_boot{},g_eject_ack{};
u32 g_eject_ack_result{};

bool UsbLaunchInUse() {
    // Do not use the diagnostic "last active title" atomic as ownership:
    // the disabled applet observer does not clear it on every close.
    std::unique_lock lock(g_process_content_mutex,std::try_to_lock);
    if(!lock.owns_lock()) return true;
    for(const auto& e:g_process_contents) if(e.pin_id && e.snapshot) return true;
    lock.unlock();
    VirtualSdDiagnostics vsd{};
    if(!GetVirtualSdDiagnostics(&vsd) || vsd.watched) return true;
    return false;
}

bool ApplicationOrAlbumInUse() {
    // Existing pm:dmnt client, queried only for a user eject request and with
    // no storage/FS/launch lock held. Unknown/failed queries refuse removal.
    u64 pid{};
    const auto app=::pmdmntGetApplicationProcessId(&pid);
    if(app!=MAKERESULT(15,1)) return true;
    const auto album=::pmdmntGetProcessId(&pid,ncm::SystemAppletId::PhotoViewer.value);
    return album!=MAKERESULT(15,1);
}

void ApplyEjectCommand() {
    const u64 now=os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
    if(!g_eject_boot) {
        // A request left by a previous boot must never eject today's drive.
        static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(EjectCommandPath));
        g_eject_boot=static_cast<u64>(::armGetSystemTick());
        if(!g_eject_boot) g_eject_boot=1;
        return;
    }
    FsFile file{};
    if(R_FAILED(mitm::fs::OpenAtmosphereSdFile(&file,EjectCommandPath,ams::fs::OpenMode_Read))) return;
    nexus_eject::Request request{};u64 read{};s64 size{};
    const Result size_rc=fsFileGetSize(&file,&size);
    const Result rc=fsFileRead(&file,0,&request,sizeof(request),FsReadOption_None,&read);
    fsFileClose(&file);
    const auto deleted=mitm::fs::DeleteAtmosphereSdFile(EjectCommandPath);
    if(R_FAILED(deleted) || R_FAILED(size_rc) || R_FAILED(rc) || size!=sizeof(request) || read!=sizeof(request)) return;
    if(!nexus_eject::Valid(request,g_eject_boot,now) || request.nonce==g_eject_ack) return;
    g_eject_ack=request.nonce;
    if(request.action==nexus_eject::Resume) {
        const auto snapshot=g_storage_service.GetEjectStatus();
        g_eject_ack_result=request.generation==snapshot.generation
            ? g_storage_service.ResumeAfterEject().GetValue() : usb::ResultMediaChanged().GetValue();
        return;
    }
    // The panel also refuses running application/Album processes. Backend
    // ownership checks independently protect USB launch/virtual-SD lifetimes.
    g_eject_ack_result=(UsbLaunchInUse() || ApplicationOrAlbumInUse()) ? usb::ResultResourceBusy().GetValue()
        : g_storage_service.RequestEject(request.nonce,request.generation).GetValue();
}

Result PublishEjectStatus() {
    if(!g_eject_boot) R_THROW(usb::ResultNotInitialized());
    const auto e=g_storage_service.GetEjectStatus();
    usb::storage::Status s{};R_TRY(g_storage_service.GetStatus(&s));
    const u64 now=os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
    char text[512]{};
    const int n=util::SNPrintf(text,sizeof(text),"NXEJ1 %llu %llu %llu %llu %llu %u %u %u %u %u %u\n",
        static_cast<unsigned long long>(g_eject_boot),static_cast<unsigned long long>(now),
        static_cast<unsigned long long>(e.generation),static_cast<unsigned long long>(e.request),
        static_cast<unsigned long long>(g_eject_ack),g_eject_ack_result,static_cast<u32>(e.phase),
        static_cast<u32>(e.stage),e.result,e.handles,s.state==usb::storage::State::Ready?1u:0u);
    R_UNLESS(n>0 && static_cast<size_t>(n)<sizeof(text),usb::ResultInvalidParameter());
    static_cast<void>(mitm::fs::DeleteAtmosphereSdFile(EjectTempPath));
    R_TRY(mitm::fs::CreateAtmosphereSdFile(EjectTempPath,0,ams::fs::CreateOption_None));
    {
        FsFile f{};R_TRY(mitm::fs::OpenAtmosphereSdFile(&f,EjectTempPath,ams::fs::OpenMode_ReadWrite));
        ON_SCOPE_EXIT {fsFileClose(&f);};
        R_TRY(fsFileSetSize(&f,n));R_TRY(fsFileWrite(&f,0,text,n,FsWriteOption_Flush));
    }
    char from[ams::fs::EntryNameLengthMax+1]{},to[ams::fs::EntryNameLengthMax+1]{};
    util::SNPrintf(from,sizeof(from),"%s:/atmosphere%s",ams::fs::impl::SdCardFileSystemMountName,EjectTempPath);
    util::SNPrintf(to,sizeof(to),"%s:/atmosphere%s",ams::fs::impl::SdCardFileSystemMountName,EjectStatusPath);
    static_cast<void>(ams::fs::DeleteFile(to));
    R_RETURN(ams::fs::RenameFile(from,to));
}
