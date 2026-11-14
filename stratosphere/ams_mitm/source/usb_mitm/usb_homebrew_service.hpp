/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 */
#pragma once

#include <stratosphere.hpp>
#include <stratosphere/usb/usb_homebrew.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include <stratosphere/usb/usb_content_bridge.hpp>
#include <stratosphere/usb/usb_volume.hpp>
#include <stratosphere/usb/usb_launch_content.hpp>
#include "nexus_vsd_protocol.h"
#include "nexus_sd_io_protocol.h"
#include <stratosphere/usb/usb_pause_protocol.h>
#include <stratosphere/usb/usb_completion_protocol.h>
#include <stratosphere/usb/usb_keep_alive_test_protocol.h>
#include <stratosphere/usb/usb_startup_timing_protocol.h>
#include <stratosphere/usb/usb_startup_io_protocol.h>
#include <stratosphere/usb/usb_startup_command_protocol.h>
#include <stratosphere/usb/usb_write_cache_protocol.h>
#include <stratosphere/usb/usb_write_transfer_protocol.h>
#include <stratosphere/usb/usb_completion_probe_protocol.h>
#include <stratosphere/usb/usb_host_write_protocol.h>

namespace ams::mitm::usb_storage {

    struct HomebrewServiceStatus {
        bool mounted;
        bool ready;
        u8 state;
        u8 reserved;
        u32 result;
        u64 media_generation;
    };

    struct HomebrewDirectoryEntry {
        char name[ams::usb::volume::MaxFileNameLength + 1];
        u64 size;
        u8 flags;
        u8 reserved[7];
    };

    static_assert(sizeof(HomebrewServiceStatus) == 0x10);
    static_assert(sizeof(HomebrewDirectoryEntry) == 0x110);

}

#define AMS_USB_HOMEBREW_INTERFACE_INFO(C, H)                                                                                         \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_OpenPinnedUsbContentFileSystem, Result, OpenPinnedUsbContentFileSystem, (sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, u64 generation), (out, generation)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_SD_IO_QUERY, Result, QuerySdIoCounters, (const sf::OutBuffer &out), (out)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_USB_PAUSE_QUERY, Result, QueryUsbPauses, (const sf::OutBuffer &out), (out)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_USB_COMPLETION_QUERY, Result, QueryUsbCompletion, (const sf::OutBuffer &out), (out)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_USB_KEEP_ALIVE_CONTROL, Result, ControlKeepAlive, (sf::Out<NexusUsbKeepAliveStatus> out, const sf::ClientProcessId &pid, NexusUsbKeepAliveRequest request), (out, pid, request)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_STARTUP_SUBMIT, Result, SubmitStartupTiming, (sf::Out<u64> out, const sf::ClientProcessId &pid, const sf::InBuffer &in), (out, pid, in)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_STARTUP_QUERY, Result, QueryStartupTiming, (const sf::OutBuffer &out, u32 index), (out, index)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_STARTUP_IO_QUERY, Result, QueryStartupIoTiming, (const sf::OutBuffer &out, u32 index), (out, index)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_STARTUP_COMMAND_QUERY, Result, QueryStartupCommandTiming, (const sf::OutBuffer &out, u32 index), (out, index)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_USB_WRITE_CACHE_QUERY, Result, QueryUsbWriteCache, (const sf::OutBuffer &out), (out)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_USB_WRITE_TRANSFER_CONTROL, Result, ControlWriteTransfer, (sf::Out<NexusUsbWriteTransferStatus> out, const sf::ClientProcessId &pid, NexusUsbWriteTransferRequest request), (out, pid, request)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_USB_COMPLETION_PROBE_CONTROL, Result, ControlCompletionProbe, (sf::Out<NexusUsbCompletionProbeStatus> out, const sf::ClientProcessId &pid, NexusUsbCompletionProbeRequest request), (out, pid, request)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_USB_HOST_WRITE_CONTROL, Result, ControlHostWrite, (sf::Out<NexusUsbHostWriteStatus> out, const sf::ClientProcessId &pid, NexusUsbHostWriteRequest request), (out, pid, request)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_VSD_QUERY, Result, QueryVirtualSd, (sf::Out<NexusVsdStatus> out, const sf::ClientProcessId &pid), (out, pid)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_VSD_ARM, Result, ArmVirtualSd, (const sf::ClientProcessId &pid, u64 generation, const sf::InBuffer &path), (pid, generation, path)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_VSD_BEGIN_LOAD, Result, BeginVirtualSdLoad, (sf::Out<NexusVsdStatus> out, const sf::ClientProcessId &pid, sf::CopyHandle &&process, const sf::InBuffer &path), (out, pid, std::move(process), path)) \
    AMS_SF_METHOD_INFO(C, H, NEXUS_VSD_CANCEL, Result, CancelVirtualSd, (const sf::ClientProcessId &pid), (pid)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_OpenNativeSdContentFileSystem, Result, OpenNativeSdContentFileSystem, (sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetLegacyStatus, Result, GetStatus,      (sf::Out<ams::mitm::usb_storage::HomebrewServiceStatus> out),                                           (out)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetNroInfo, Result, GetNroInfo,     (sf::Out<ams::usb::homebrew::NroInfo> out, const sf::InBuffer &path),                                    (out, path)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_ReadNro, Result, ReadNro,        (sf::Out<u64> out_read, const sf::OutBuffer &out, const sf::InBuffer &path, u64 offset, u64 media_generation), (out_read, out, path, offset, media_generation)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_ReadDirectoryLegacy, Result, ReadDirectory,  (sf::Out<u32> out_count, const sf::OutBuffer &out, const sf::InBuffer &path, u32 skip_entries),         (out_count, out, path, skip_entries)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_Start, Result, Start,          (),                                                                                                         ()) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetStorageStatus, Result, GetStorageStatus, (sf::Out<ams::usb::storage::Status> out),                                                                  (out)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetFileInfo, Result, GetFileInfo,      (sf::Out<ams::usb::volume::FileInfo> out, const sf::InBuffer &path, ams::usb::content::GenerationRequest request), (out, path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_ReadFile, Result, ReadFile,         (sf::Out<u64> out_read, const sf::OutBuffer &out, const sf::InBuffer &path, ams::usb::content::IoRequest request), (out_read, out, path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_ReadDirectory, Result, ReadContentDirectory, (sf::Out<u64> out_count, const sf::OutBuffer &out, const sf::InBuffer &path, ams::usb::content::DirectoryRequest request), (out_count, out, path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetSpaceSize, Result, GetSpaceSize,      (sf::Out<ams::usb::content::SpaceInfo> out),                                                              (out)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_CreateFile, Result, CreateFile,       (const sf::InBuffer &path, ams::usb::content::SizeRequest request),                                       (path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_ResizeFile, Result, ResizeFile,       (const sf::InBuffer &path, ams::usb::content::SizeRequest request),                                       (path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_DeleteFile, Result, DeleteFile,       (const sf::InBuffer &path, ams::usb::content::GenerationRequest request),                                 (path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_RenameFile, Result, RenameFile,       (const sf::InBuffer &old_path, const sf::InBuffer &new_path, ams::usb::content::GenerationRequest request), (old_path, new_path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_WriteFile, Result, WriteFile,        (sf::Out<u64> out_written, const sf::InBuffer &data, const sf::InBuffer &path, ams::usb::content::IoRequest request), (out_written, data, path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_CreateDirectory, Result, CreateDirectory,  (const sf::InBuffer &path, ams::usb::content::GenerationRequest request),                                 (path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_Flush, Result, Flush,            (ams::usb::content::GenerationRequest request),                                                          (request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_OpenCodeFileSystem, Result, OpenCodeFileSystem, (sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, const sf::OutBuffer &verification, const sf::InBuffer &path, ncm::ProgramId program_id), (out, verification, path, program_id)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_RecordLoaderProbe, Result, RecordLoaderProbe, (ams::usb::content::LoaderProbeInfo info), (info)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_RecordLoaderTrace, Result, RecordLoaderTrace, (ams::usb::content::LoaderTraceInfo info), (info)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetNcaRightsId, Result, GetNcaRightsId, (sf::Out<ams::fs::RightsId> out, const sf::InBuffer &path), (out, path)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetApplicationContentPath, Result, GetApplicationContentPath, (const sf::OutBuffer &out_path, ams::usb::content::ApplicationContentPathRequest request), (out_path, request)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_ResolveProgramIdAlias, Result, ResolveProgramIdAlias, (sf::Out<ncm::ProgramId> out, ncm::ProgramId requested_program_id), (out, requested_program_id)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetNcaProgramId, Result, GetNcaProgramId, (sf::Out<ncm::ProgramId> out, const sf::InBuffer &path), (out, path)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_OpenLaunchCodeFileSystem, Result, OpenLaunchCodeFileSystem, (sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, const sf::OutBuffer &verification, const sf::InBuffer &content, ncm::ProgramId program_id), (out, verification, content, program_id)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_OpenSdContentFileSystem, Result, OpenSdContentFileSystem, (sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_GetSdContentHeader, Result, GetSdContentHeader, (sf::Out<ams::usb::content::ContentHeaderInfo> out, const sf::InBuffer &path, u32 query), (out, path, query)) \
    AMS_SF_METHOD_INFO(C, H, ams::usb::content::CommandId_SetProcessContent, Result, SetProcessContent, (ams::usb::content::ProcessContentBinding binding), (binding))

/* Validate the actual dispatch table, not a separately maintained ID list. */
#define AMS_USB_COMMAND_VALUE(C, ID, ...) ID,
namespace ams::mitm::usb_storage {
    constexpr unsigned int HomebrewCommandIds[] = {
        AMS_USB_HOMEBREW_INTERFACE_INFO(Unused, AMS_USB_COMMAND_VALUE)
    };
    static_assert(ams::usb::content::AreCommandIdsUnique(HomebrewCommandIds),
                  "Duplicate ams:usb command ID");
}
#undef AMS_USB_COMMAND_VALUE

AMS_SF_DEFINE_INTERFACE(ams::mitm::usb_storage::impl, IHomebrewInterface, AMS_USB_HOMEBREW_INTERFACE_INFO, 0x00000000)

namespace ams::mitm::usb_storage {

    class HomebrewService {
        public:
            Result QuerySdIoCounters(const sf::OutBuffer &out);
            Result QueryUsbPauses(const sf::OutBuffer &out);
            Result QueryUsbCompletion(const sf::OutBuffer &out);
            Result ControlKeepAlive(sf::Out<NexusUsbKeepAliveStatus> out, const sf::ClientProcessId &pid, NexusUsbKeepAliveRequest request);
            Result SubmitStartupTiming(sf::Out<u64> out,const sf::ClientProcessId &pid,const sf::InBuffer &in);
            Result QueryStartupTiming(const sf::OutBuffer &out,u32 index);
            Result QueryStartupIoTiming(const sf::OutBuffer &out,u32 index);
            Result QueryStartupCommandTiming(const sf::OutBuffer &out,u32 index);
            Result QueryUsbWriteCache(const sf::OutBuffer &out);
            Result ControlWriteTransfer(sf::Out<NexusUsbWriteTransferStatus> out, const sf::ClientProcessId &pid, NexusUsbWriteTransferRequest request);
            Result ControlCompletionProbe(sf::Out<NexusUsbCompletionProbeStatus> out, const sf::ClientProcessId &pid, NexusUsbCompletionProbeRequest request);
            Result ControlHostWrite(sf::Out<NexusUsbHostWriteStatus> out, const sf::ClientProcessId &pid, NexusUsbHostWriteRequest request);
            Result QueryVirtualSd(sf::Out<NexusVsdStatus> out,const sf::ClientProcessId &pid);
            Result ArmVirtualSd(const sf::ClientProcessId &pid,u64 generation,const sf::InBuffer &path);
            Result BeginVirtualSdLoad(sf::Out<NexusVsdStatus> out,const sf::ClientProcessId &pid,sf::CopyHandle &&process,const sf::InBuffer &path);
            Result CancelVirtualSd(const sf::ClientProcessId &pid);
            Result Start();
            Result OpenPinnedUsbContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, u64 generation);
            Result GetStatus(sf::Out<HomebrewServiceStatus> out);
            Result GetNroInfo(sf::Out<ams::usb::homebrew::NroInfo> out, const sf::InBuffer &path);
            Result ReadNro(sf::Out<u64> out_read, const sf::OutBuffer &out, const sf::InBuffer &path, u64 offset, u64 media_generation);
            Result ReadDirectory(sf::Out<u32> out_count, const sf::OutBuffer &out, const sf::InBuffer &path, u32 skip_entries);
            Result GetStorageStatus(sf::Out<ams::usb::storage::Status> out);
            Result GetFileInfo(sf::Out<ams::usb::volume::FileInfo> out, const sf::InBuffer &path, ams::usb::content::GenerationRequest request);
            Result ReadFile(sf::Out<u64> out_read, const sf::OutBuffer &out, const sf::InBuffer &path, ams::usb::content::IoRequest request);
            Result ReadContentDirectory(sf::Out<u64> out_count, const sf::OutBuffer &out, const sf::InBuffer &path, ams::usb::content::DirectoryRequest request);
            Result GetSpaceSize(sf::Out<ams::usb::content::SpaceInfo> out);
            Result CreateFile(const sf::InBuffer &path, ams::usb::content::SizeRequest request);
            Result ResizeFile(const sf::InBuffer &path, ams::usb::content::SizeRequest request);
            Result DeleteFile(const sf::InBuffer &path, ams::usb::content::GenerationRequest request);
            Result RenameFile(const sf::InBuffer &old_path, const sf::InBuffer &new_path, ams::usb::content::GenerationRequest request);
            Result WriteFile(sf::Out<u64> out_written, const sf::InBuffer &data, const sf::InBuffer &path, ams::usb::content::IoRequest request);
            Result CreateDirectory(const sf::InBuffer &path, ams::usb::content::GenerationRequest request);
            Result Flush(ams::usb::content::GenerationRequest request);
            Result OpenCodeFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, const sf::OutBuffer &verification, const sf::InBuffer &path, ncm::ProgramId program_id);
            Result OpenLaunchCodeFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out, const sf::OutBuffer &verification, const sf::InBuffer &content, ncm::ProgramId program_id);
            Result RecordLoaderProbe(ams::usb::content::LoaderProbeInfo info);
            Result SetProcessContent(ams::usb::content::ProcessContentBinding binding);
            Result RecordLoaderTrace(ams::usb::content::LoaderTraceInfo info);
            Result GetNcaRightsId(sf::Out<ams::fs::RightsId> out, const sf::InBuffer &path);
            Result GetApplicationContentPath(const sf::OutBuffer &out_path, ams::usb::content::ApplicationContentPathRequest request);
            Result ResolveProgramIdAlias(sf::Out<ncm::ProgramId> out, ncm::ProgramId requested_program_id);
            Result GetNcaProgramId(sf::Out<ncm::ProgramId> out, const sf::InBuffer &path);
            Result OpenSdContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out);
            Result OpenNativeSdContentFileSystem(sf::Out<sf::SharedPointer<ams::fssrv::sf::IFileSystem>> out);
            Result GetSdContentHeader(sf::Out<ams::usb::content::ContentHeaderInfo> out, const sf::InBuffer &path, u32 query);
    };

    static_assert(impl::IsIHomebrewInterface<HomebrewService>);

}
