/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#pragma once

#include <vapours.hpp>
#include <stratosphere/usb/usb_content_commands.hpp>
#include <stratosphere/usb/usb_volume.hpp>

namespace ams::usb::content {

    constexpr inline const char *ServiceName = "ams:usb";

    /* R110 returns to the filesystem route that preserved Horizon's normal
     * SdCard launch handoff. NCM metadata, tickets and ProgramLocation keep
     * their stock SD identity; fsp-srv command 110 redirects only the
     * byte-bearing /registered and /placehld directories while USB is ready.
     * Keep this compile-time gate shared so NCM and AMS MITM cannot select
     * conflicting storage architectures during one boot. */
    constexpr inline bool UseFileSystemContentRoute = true;

    enum class LoaderProbeStage : u32 {
        None                 = 0,
        Entered              = 1,
        IneligibleLocation   = 2,
        MetadataNotFound     = 3,
        MetadataFound        = 4,
        HostOpenFailed       = 5,
        HostContentMissing   = 6,
        HostPathInvalid      = 7,
        HostPathReady        = 8,
        StockCodeMountFailed = 9,
        UsbCodeMountRetry    = 10,
        MetadataDatabaseOpen = 11,
        MetadataLookup       = 12,
        HostOpenAttempt      = 13,
        HostContentCheck     = 14,
        HostPathLookup       = 15,
        StockCodeMountAttempt = 16,
        StockCodeMountResult = 17,
        UsbCodeMountAttempt  = 18,
        UsbCodeMountResult   = 19,
        RuntimeCodeOpenAttempt = 20,
        RuntimeCodeOpenResult  = 21,
        ProgramPathResolveAttempt = 22,
        ProgramPathResolveResult  = 23,
        LoaderCreateProcessAttempt = 24,
        LoaderCreateProcessPathResult = 25,
        LoaderGetProgramInfoAttempt = 26,
        LoaderGetProgramInfoPathResult = 27,
        PglLaunchAttempt         = 28,
        PglLaunchResult          = 29,
        PmLaunchAttempt          = 30,
        PmGetProgramInfoResult   = 31,
        PmPinProgramResult       = 32,
        PmCreateProcessResult    = 33,
        PmLaunchResult            = 34,
        NsGetContentPathAttempt   = 35,
        NsGetContentPathResult    = 36,
        NsResolveContentPathAttempt = 37,
        NsResolveContentPathResult  = 38,
        NsWebGetContentPathAttempt  = 39,
        NsWebGetContentPathResult   = 40,
        FsCurrentStorageAttempt     = 41,
        FsCurrentStorageResult      = 42,
        FsDataIdStorageAttempt      = 43,
        FsDataIdStorageResult       = 44,
        PmShellLaunchAttempt        = 45,
        PmShellLaunchResult         = 46,
        NsWebGetContentPath2Attempt = 47,
        NsWebGetContentPath2Result  = 48,
        LrResolveProgramPathAttempt = 49,
        LrResolveProgramPathResult  = 50,
        UsbTitleMapMiss             = 51,
        UsbTitleMapHit              = 52,
        ExternalCodeBindAttempt     = 53,
        ExternalCodeBindResult      = 54,
        FsControlMountAttempt       = 55,
        FsControlMountResult        = 56,
        FsControlScanAttempt        = 57,
        FsControlScanResult         = 58,
        PglAliasResolveAttempt      = 59,
        PglAliasResolveResult       = 60,
        LoaderAliasFallbackAttempt  = 61,
        LoaderAliasFallbackResult   = 62,
        UsbBoundSdMountAttempt      = 70,
        UsbBoundSdMountResult       = 71,
        UsbBoundBaseMountAttempt    = 72,
        UsbBoundBaseMountResult     = 73,
        FsStockControlMountResult   = 74,
        FsStockRightsHeaderResult   = 75,
        FsStockProgramHeaderResult  = 76,
        NsPrePglPatchDiagnostic     = 133,
        PglIngressAttempt           = 134,
        PglExactMapResult           = 135,
        MappedTitleContentCacheSeed = 136,
        FsUsbHeaderIdentityResult   = 137,
        NsControlDataObjectPublish  = 138,
        NsControlDataForwardCommand = 139,
        NsControlDataForwardFailure = 140,
    };

    struct LoaderProbeInfo {
        u64 program_id;
        u32 stage;
        u32 result;
        u8 location_storage;
        u8 metadata_storage;
        u8 reserved[6];
    };

    enum LoaderTraceFlags : u32 {
        LoaderTraceFlag_PathTruncated = (1u << 0),
    };

    struct LoaderTraceInfo {
        u64 program_id;
        u8 content_id[0x10];
        u32 stage;
        u32 result;
        u8 location_storage;
        u8 metadata_storage;
        u8 content_storage;
        u8 reserved;
        u32 flags;
        u64 media_generation;
        char path[0x80];
    };

    struct GenerationRequest {
        u64 media_generation;
    };

    struct IoRequest {
        u64 offset;
        u64 media_generation;
    };

    struct SizeRequest {
        u64 size;
        u64 media_generation;
    };

    struct DirectoryRequest {
        u64 skip_entries;
        u64 media_generation;
        u8 include_directories;
        u8 include_files;
        u8 reserved[6];
    };

    struct SpaceInfo {
        u64 free_size;
        u64 total_size;
    };

    struct ApplicationContentPathRequest {
        u64 program_id;
        u8 content_type;
        u8 reserved[7];
    };

    static_assert(sizeof(GenerationRequest) == 0x8);
    static_assert(sizeof(IoRequest) == 0x10);
    static_assert(sizeof(SizeRequest) == 0x10);
    static_assert(sizeof(DirectoryRequest) == 0x18);
    static_assert(sizeof(SpaceInfo) == 0x10);
    static_assert(sizeof(ApplicationContentPathRequest) == 0x10);
    static_assert(sizeof(LoaderProbeInfo) == 0x18);
    static_assert(sizeof(LoaderTraceInfo) == 0xB0);

}
