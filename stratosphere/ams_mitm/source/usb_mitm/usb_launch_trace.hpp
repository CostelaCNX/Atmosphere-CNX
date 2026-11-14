/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2, as published
 * by the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <stratosphere.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include "usbmitm_module.hpp"

namespace ams::mitm::usb_storage {

    /* Memory-only in-flight diagnostic. The status thread publishes it;
     * forwarding threads never write logs to disk. */
    void BeginAppletForwardProgress(ncm::ProgramId program_id, u64 session,
        u32 command, u32 object, u32 input_size, u64 input0, bool decoded);
    void RecordAppletNativeReturn(u64 session, Result result);
    void RecordAppletObserverProgress(u64 session, bool completed);

    void BeginFsForwardProgress(ncm::ProgramId client, ncm::ProgramId program, u64 session,
        u32 command, u32 object, u32 input_size, u64 input0, bool decoded);
    void RecordFsNativeReturn(u64 session, Result result);
    void RecordFsObserverProgress(u64 session, bool completed);
    void RecordFsReplyResult(u64 session, Result result);
    void RecordFsDetailSuppressed(bool failed);

    /* R18 provider-local stages; the shared trace wire layout is unchanged. */
    inline constexpr auto FsControlObjectPublish = static_cast<ams::usb::content::LoaderProbeStage>(63);
    inline constexpr auto FsControlFileOpen = static_cast<ams::usb::content::LoaderProbeStage>(64);
    inline constexpr auto FsControlFileRead = static_cast<ams::usb::content::LoaderProbeStage>(65);
    inline constexpr auto FsControlDirectoryOpen = static_cast<ams::usb::content::LoaderProbeStage>(66);
    inline constexpr auto FsUsbRightsHeader = static_cast<ams::usb::content::LoaderProbeStage>(67);
    inline constexpr auto FsUsbProgramHeader = static_cast<ams::usb::content::LoaderProbeStage>(68);
    inline constexpr auto FsControlDataChecksum = static_cast<ams::usb::content::LoaderProbeStage>(69);
    inline constexpr auto NsAm2ObjectPublish = static_cast<ams::usb::content::LoaderProbeStage>(77);
    inline constexpr auto NsApplicationViewAttempt = static_cast<ams::usb::content::LoaderProbeStage>(78);
    inline constexpr auto NsApplicationViewResult = static_cast<ams::usb::content::LoaderProbeStage>(79);
    inline constexpr auto NsLaunchVersionAttempt = static_cast<ams::usb::content::LoaderProbeStage>(80);
    inline constexpr auto NsLaunchVersionResult = static_cast<ams::usb::content::LoaderProbeStage>(81);
    inline constexpr auto NsApplicationViewTail = static_cast<ams::usb::content::LoaderProbeStage>(82);
    inline constexpr auto NsApplicationViewPatch = static_cast<ams::usb::content::LoaderProbeStage>(83);
    inline constexpr auto AppletAeProxyResult = static_cast<ams::usb::content::LoaderProbeStage>(84);
    inline constexpr auto AppletApplicationCreatorResult = static_cast<ams::usb::content::LoaderProbeStage>(85);
    inline constexpr auto AppletCreateApplicationAttempt = static_cast<ams::usb::content::LoaderProbeStage>(86);
    inline constexpr auto AppletCreateApplicationResult = static_cast<ams::usb::content::LoaderProbeStage>(87);
    inline constexpr auto AppletApplicationStartAttempt = static_cast<ams::usb::content::LoaderProbeStage>(88);
    inline constexpr auto AppletApplicationStartResult = static_cast<ams::usb::content::LoaderProbeStage>(89);
    inline constexpr auto AppletAccessorCommandResult = static_cast<ams::usb::content::LoaderProbeStage>(90);
    inline constexpr auto AppletAeCommand100Rejected = static_cast<ams::usb::content::LoaderProbeStage>(91);
    inline constexpr auto AppletAeCandidateCommand = static_cast<ams::usb::content::LoaderProbeStage>(92);
    inline constexpr auto AppletAeResponseParseFailed = static_cast<ams::usb::content::LoaderProbeStage>(93);
    inline constexpr auto AppletApplicationGetResult = static_cast<ams::usb::content::LoaderProbeStage>(94);
    inline constexpr auto NsAm2ForwardCommand = static_cast<ams::usb::content::LoaderProbeStage>(95);
    inline constexpr auto NsAm2ForwardFailure = static_cast<ams::usb::content::LoaderProbeStage>(96);
    inline constexpr auto FsProgramFileSystemStock = static_cast<ams::usb::content::LoaderProbeStage>(97);
    inline constexpr auto FsProgramFileSystemUsb = static_cast<ams::usb::content::LoaderProbeStage>(98);
    inline constexpr auto FsProgramStorageStock = static_cast<ams::usb::content::LoaderProbeStage>(99);
    inline constexpr auto FsProgramStorageUsb = static_cast<ams::usb::content::LoaderProbeStage>(100);
    inline constexpr auto FsMappedProgramPathCache = static_cast<ams::usb::content::LoaderProbeStage>(101);
    inline constexpr auto FsAmOpenFileSystemWithId = static_cast<ams::usb::content::LoaderProbeStage>(102);
    inline constexpr auto FsForwardCommand = static_cast<ams::usb::content::LoaderProbeStage>(103);
    inline constexpr auto FsForwardFailure = static_cast<ams::usb::content::LoaderProbeStage>(104);
    inline constexpr auto ArpControlPropertyStock = static_cast<ams::usb::content::LoaderProbeStage>(105);
    inline constexpr auto ArpLaunchPropertyResult = static_cast<ams::usb::content::LoaderProbeStage>(106);
    inline constexpr auto ArpControlPropertyUsb = static_cast<ams::usb::content::LoaderProbeStage>(107);
    inline constexpr auto ArpWriterWorkerQueued = static_cast<ams::usb::content::LoaderProbeStage>(108);
    inline constexpr auto ArpReaderServiceOpen = static_cast<ams::usb::content::LoaderProbeStage>(109);
    inline constexpr auto ArpReaderListInstances = static_cast<ams::usb::content::LoaderProbeStage>(110);
    inline constexpr auto ArpReaderMatchInstance = static_cast<ams::usb::content::LoaderProbeStage>(111);
    inline constexpr auto ArpWriterReadControl = static_cast<ams::usb::content::LoaderProbeStage>(112);
    inline constexpr auto ArpWriterServiceOpen = static_cast<ams::usb::content::LoaderProbeStage>(113);
    inline constexpr auto ArpWriterRegisterControl = static_cast<ams::usb::content::LoaderProbeStage>(114);
    inline constexpr auto ArpWriterWorkerComplete = static_cast<ams::usb::content::LoaderProbeStage>(115);
    inline constexpr auto ArpWriterLaunchWait = static_cast<ams::usb::content::LoaderProbeStage>(116);
    inline constexpr auto FsControlEntryType = static_cast<ams::usb::content::LoaderProbeStage>(117);
    inline constexpr auto AppletAccessorRawCommand = static_cast<ams::usb::content::LoaderProbeStage>(118);
    inline constexpr auto FsControlFileGetSize = static_cast<ams::usb::content::LoaderProbeStage>(119);
    inline constexpr auto FsControlFileReadDetail = static_cast<ams::usb::content::LoaderProbeStage>(120);
    inline constexpr auto FsControlFileOperateRange = static_cast<ams::usb::content::LoaderProbeStage>(121);
    inline constexpr auto NcmLaunchTraceBegin = static_cast<ams::usb::content::LoaderProbeStage>(122);
    inline constexpr auto NcmLaunchTraceEnd = static_cast<ams::usb::content::LoaderProbeStage>(123);
    inline constexpr auto NcmMetadataContentInfo = static_cast<ams::usb::content::LoaderProbeStage>(124);
    inline constexpr auto NcmMetadataLatestKey = static_cast<ams::usb::content::LoaderProbeStage>(125);
    inline constexpr auto NcmOverlayHas = static_cast<ams::usb::content::LoaderProbeStage>(126);
    inline constexpr auto NcmOverlayPath = static_cast<ams::usb::content::LoaderProbeStage>(127);
    inline constexpr auto NcmNativeHas = static_cast<ams::usb::content::LoaderProbeStage>(128);
    inline constexpr auto NcmNativePath = static_cast<ams::usb::content::LoaderProbeStage>(129);
    inline constexpr auto NcmOpenStorage = static_cast<ams::usb::content::LoaderProbeStage>(130);
    inline constexpr auto NcmOpenMetadata = static_cast<ams::usb::content::LoaderProbeStage>(131);
    inline constexpr auto NcmLaunchTraceSummary = static_cast<ams::usb::content::LoaderProbeStage>(132);
    /* R103 provider-local stages. Keep the shared NCM/Loader trace ABI and
     * their already-built KIPs byte-for-byte unchanged. */
    inline constexpr auto NsApplicationRightsResult = static_cast<ams::usb::content::LoaderProbeStage>(141);
    inline constexpr auto NsApplicationUpdateResult = static_cast<ams::usb::content::LoaderProbeStage>(142);
    inline constexpr auto AppletAeActiveCommand = static_cast<ams::usb::content::LoaderProbeStage>(143);
    inline constexpr auto AppletAeActiveBuffer = static_cast<ams::usb::content::LoaderProbeStage>(144);
    inline constexpr auto FsHandoffCommand = static_cast<ams::usb::content::LoaderProbeStage>(145);
    inline constexpr auto FsHandoffBuffer = static_cast<ams::usb::content::LoaderProbeStage>(146);
    inline constexpr auto UsbLaunchTraceArmed = static_cast<ams::usb::content::LoaderProbeStage>(147);
    inline constexpr auto FsNativeControlForward = static_cast<ams::usb::content::LoaderProbeStage>(148);
    inline constexpr auto FsNativeIdentityCacheSeed = static_cast<ams::usb::content::LoaderProbeStage>(149);
    inline constexpr auto UsbRouteActivationDeferred = static_cast<ams::usb::content::LoaderProbeStage>(150);
    inline constexpr auto UsbRouteActivatedAtCreate = static_cast<ams::usb::content::LoaderProbeStage>(151);
    inline constexpr auto FsControlForwardSend = static_cast<ams::usb::content::LoaderProbeStage>(152);
    inline constexpr auto FsControlForwardReturn = static_cast<ams::usb::content::LoaderProbeStage>(153);
    inline constexpr auto FsControlForwardCmif = static_cast<ams::usb::content::LoaderProbeStage>(154);
    inline constexpr auto FsControlForwardDecodeFailure = static_cast<ams::usb::content::LoaderProbeStage>(155);
    inline constexpr auto AppletAeDispatchWorker = static_cast<ams::usb::content::LoaderProbeStage>(156);
    inline constexpr auto FsUsbStorageReservation = static_cast<ams::usb::content::LoaderProbeStage>(157);
    inline constexpr auto FsUsbStorageHandoff = static_cast<ams::usb::content::LoaderProbeStage>(158);
    inline constexpr auto FsUsbStorageFirstRead = static_cast<ams::usb::content::LoaderProbeStage>(159);
    inline constexpr auto FsUsbProcessBinding = static_cast<ams::usb::content::LoaderProbeStage>(160);
    inline constexpr auto FsNativeProcessResources = static_cast<ams::usb::content::LoaderProbeStage>(161);

    inline void RecordLaunchBoundaryTrace(ams::usb::content::LoaderProbeStage stage, ncm::ProgramId program_id, u8 content_type, Result result = ResultSuccess(), const char *path = nullptr, u8 location_storage = static_cast<u8>(ncm::StorageId::None), u8 content_storage = static_cast<u8>(ncm::StorageId::None)) {
        ams::usb::content::LoaderTraceInfo info{};
        info.program_id = program_id.value;
        info.stage = static_cast<u32>(stage);
        info.result = result.GetValue();
        info.location_storage = location_storage;
        info.metadata_storage = content_type;
        info.content_storage = content_storage;
        info.flags = static_cast<u32>(content_type);
        if (path != nullptr) {
            for (size_t i = 0; i + 1 < sizeof(info.path) && path[i] != '\0'; ++i) {
                info.path[i] = path[i];
            }
        }

        RecordLoaderTraceForDiagnostics(info);
    }

}
