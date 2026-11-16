/*
 * Copyright (c) Horizon OC Contributors
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stratosphere/rocrt/rocrt.hpp>

namespace ams::ldr::hoc::pcv {

    constexpr size_t PcvDataArenaSize    = 0x1000;
    constexpr size_t PcvCodeArenaSize    = 0x1000;
    constexpr size_t PcvRuntimeArenaSize = 0x1000;
    constexpr size_t PcvArenaSize = PcvDataArenaSize + PcvCodeArenaSize + PcvRuntimeArenaSize;
    constexpr size_t PcvHookPayloadOffset = 0x100;
    constexpr size_t PcvHookPayloadCapacity = PcvCodeArenaSize - PcvHookPayloadOffset;

    /* PCV's startup runtime enumerates RX Code mappings as modules. A bare
     * hook page is rejected before PCV starts. Describe it as an empty module,
     * with hook instructions after the metadata and runtime-owned BSS in the
     * following RW page. Never share that BSS with loader-initialized hook
     * data: module discovery clears BSS before registering the module.
     *
     * There are no imports, exports, relocations, TLS, or constructors. The
     * empty SysV hash table is still required by runtime symbol lookups. */
    struct HookModuleMetadata {
        rocrt::ModuleHeaderLocation location;
        u32 reserved;
        rocrt::ModuleHeader header;
        u8 header_padding[0x0C];
        struct DynamicEntry { u64 tag; u64 value; } dynamic[6];
        u32 hash[4];
        char strings[8];
        u8 null_symbol[24];
        u8 padding[0x20];
    };
    static_assert(sizeof(HookModuleMetadata) == PcvHookPayloadOffset);
    static_assert(offsetof(HookModuleMetadata, header) == 0x10);
    static_assert(offsetof(HookModuleMetadata, dynamic) == 0x50);
    static_assert(offsetof(HookModuleMetadata, hash) == 0xB0);
    static_assert(offsetof(HookModuleMetadata, null_symbol) == 0xC8);
    static_assert(PcvHookPayloadOffset % 8 == 0);

    void InitializeHookModule(uintptr_t code_arena);

}
