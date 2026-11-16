/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>
#include "ldr_patcher.hpp"

namespace ams::ldr {

    namespace {

        constexpr const char *NsoPatchesDirectory = "exefs_patches";

        /* Exefs patches want to prevent modification of header, */
        /* and also want to adjust offset relative to mapped location. */
        constexpr size_t NsoPatchesProtectedSize   = sizeof(NsoHeader);
        constexpr size_t NsoPatchesProtectedOffset = sizeof(NsoHeader);

        constexpr const char * const LoaderSdMountName = "#amsldr-sdpatch";
        static_assert(sizeof(LoaderSdMountName) <= fs::MountNameLengthMax);

        constexpr const char *AmPatchDiagnosticBuildId = "usb-content-v178-recovery-r59-20260830";
        constexpr const char *AmPatchDiagnosticPath = "#amsldr-sdpatch:/atmosphere/logs/usb_am_patch_status_r59.txt";
        /* R143: the diagnostic instruction tables describe only HOS 22.5.
         * FW23 retains its native results; kernel IPC tracing observes them
         * without borrowing instruction offsets from a different firmware. */
        constexpr bool EnableAmExposeUsbLaunchResultPatches = false;
        constexpr bool EnableNsPrePglStageTagPatches = false;
        constexpr bool EnableAmPatchDiagnosticWriter = false;
        constexpr size_t AmPatchDiagnosticBufferSize = 0x800;

        constinit os::SdkMutex g_ldr_sd_lock;
        constinit bool g_mounted_sd;

        constinit os::SdkMutex g_embedded_patch_lock;
        constinit bool g_got_embedded_patch_settings;
        constinit bool g_force_enable_usb30;

        constinit os::SdkMutex g_ns_pre_pgl_patch_diagnostic_lock;
        constinit NsPrePglPatchDiagnostic g_ns_pre_pgl_patch_diagnostic{};

        constinit os::SdkMutex g_am_patch_diagnostic_lock;
        constinit char g_am_patch_diagnostic_buffer[AmPatchDiagnosticBufferSize];
        constinit size_t g_am_patch_diagnostic_size;
        constinit bool g_am_patch_diagnostic_pending;

        bool EnsureSdCardMounted() {
            std::scoped_lock lk(g_ldr_sd_lock);

            if (g_mounted_sd) {
                return true;
            }

            if (!cfg::IsSdCardInitialized()) {
                return false;
            }

            if (R_FAILED(fs::MountSdCard(LoaderSdMountName))) {
                return false;
            }

            return (g_mounted_sd = true);
        }

        bool IsUsb30ForceEnabled() {
            std::scoped_lock lk(g_embedded_patch_lock);

            if (!g_got_embedded_patch_settings) {
                g_force_enable_usb30 = spl::IsUsb30ForceEnabled();
                g_got_embedded_patch_settings = true;
            }

            return g_force_enable_usb30;
        }

        consteval u8 ParseNybble(char c) {
            AMS_ASSUME(('0' <= c && c <= '9') || ('A' <= c && c <= 'F') || ('a' <= c && c <= 'f'));
            if ('0' <= c && c <= '9') {
                return c - '0' + 0x0;
            } else if ('A' <= c && c <= 'F') {
                return c - 'A' + 0xA;
            } else /* if ('a' <= c && c <= 'f') */ {
                return c - 'a' + 0xa;
            }
        }

        consteval ro::ModuleId ParseModuleId(const char *str) {
            /* Parse a static module id. */
            ro::ModuleId module_id = {};

            size_t ofs = 0;
            while (str[0] != 0) {
                AMS_ASSUME(ofs < sizeof(module_id));
                AMS_ASSUME(str[1] != 0);

                module_id.data[ofs] = (ParseNybble(str[0]) << 4) | (ParseNybble(str[1]) << 0);

                str += 2;
                ofs++;
            }

            return module_id;
        }

        u32 ReadPatchWord(const void *address) {
            u32 value;
            std::memcpy(std::addressof(value), address, sizeof(value));
            return value;
        }

        void StoreNsPrePglPatchDiagnostic(const NsPrePglPatchDiagnostic &diagnostic) {
            std::scoped_lock lk(g_ns_pre_pgl_patch_diagnostic_lock);
            g_ns_pre_pgl_patch_diagnostic = diagnostic;
        }

        struct EmbeddedPatchEntry {
            uintptr_t offset;
            const void * const data;
            size_t size;
        };

        struct EmbeddedPatch {
            ro::ModuleId module_id;
            size_t num_entries;
            const EmbeddedPatchEntry *entries;
        };

        #include "ldr_embedded_usb_patches.inc"
        #include "ldr_embedded_am_patches.inc"
        #include "ldr_embedded_web_patches.inc"
        #include "ldr_embedded_ns_patches.inc"

        void AppendAmPatchDiagnostic(size_t *offset, const char *format, ...) {
            if (*offset >= AmPatchDiagnosticBufferSize - 1) {
                return;
            }

            std::va_list vl;
            va_start(vl, format);
            const int length = util::VSNPrintf(g_am_patch_diagnostic_buffer + *offset, AmPatchDiagnosticBufferSize - *offset, format, vl);
            va_end(vl);

            if (length > 0) {
                *offset = std::min(*offset + static_cast<size_t>(length), AmPatchDiagnosticBufferSize - 1);
            }
        }

        bool TryFlushAmPatchDiagnosticLocked() {
            if (!g_am_patch_diagnostic_pending) {
                return true;
            }

            /* AM is loaded before Loader can necessarily mount the SD card.
             * Keep the completed report in Loader's process memory and retry
             * it when a later NSO is loaded instead of silently losing it. */
            if (!EnsureSdCardMounted()) {
                return false;
            }

            static_cast<void>(fs::CreateDirectory("#amsldr-sdpatch:/atmosphere/logs"));
            static_cast<void>(fs::DeleteFile(AmPatchDiagnosticPath));
            if (R_FAILED(fs::CreateFile(AmPatchDiagnosticPath, static_cast<s64>(g_am_patch_diagnostic_size)))) {
                return false;
            }

            fs::FileHandle file;
            if (R_FAILED(fs::OpenFile(std::addressof(file), AmPatchDiagnosticPath, fs::OpenMode_Write))) {
                return false;
            }
            ON_SCOPE_EXIT { fs::CloseFile(file); };

            if (R_FAILED(fs::WriteFile(file, 0, g_am_patch_diagnostic_buffer, g_am_patch_diagnostic_size, fs::WriteOption::Flush))) {
                return false;
            }

            g_am_patch_diagnostic_pending = false;
            return true;
        }

        void TryFlushAmPatchDiagnostic() {
            std::scoped_lock lk(g_am_patch_diagnostic_lock);
            static_cast<void>(TryFlushAmPatchDiagnosticLocked());
        }

        void WriteAmPatchDiagnostic(const ro::ModuleId &module_id, uintptr_t mapped_nso, size_t mapped_size, const EmbeddedPatch &patch) {
            std::scoped_lock lk(g_am_patch_diagnostic_lock);

            g_am_patch_diagnostic_size = 0;
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "build_id=%s\n", AmPatchDiagnosticBuildId);
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "status=final-mapped-bytes\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "program_id=%016lX\n", static_cast<unsigned long>(ncm::SystemProgramId::Am.value));
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "nso=main\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "exact_match=1\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "module_id=");
            for (size_t i = 0; i < sizeof(module_id.data); ++i) {
                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "%02X", module_id.data[i]);
            }
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "mapped_size=0x%lX\n", static_cast<unsigned long>(mapped_size));
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "entry_count=%lu\n", static_cast<unsigned long>(patch.num_entries));
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "sd_ips_already_applied=1\n");

            bool all_replacements = true;
            for (size_t i = 0; i < patch.num_entries; ++i) {
                const auto &entry = patch.entries[i];
                const bool in_bounds = entry.size == 4 && entry.offset + entry.size <= mapped_size;
                const u8 *final_bytes = in_bounds ? reinterpret_cast<const u8 *>(mapped_nso + entry.offset) : nullptr;
                const u8 *replacement = static_cast<const u8 *>(entry.data);
                const u8 *original = AmExposeUsbLaunchResultOriginals_22_5_0[i];
                const bool is_replacement = in_bounds && std::memcmp(final_bytes, replacement, 4) == 0;
                const bool is_original = in_bounds && std::memcmp(final_bytes, original, 4) == 0;
                all_replacements &= is_replacement;

                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "entry%lu_offset=0x%lX bounds=%u expected=", static_cast<unsigned long>(i), static_cast<unsigned long>(entry.offset), in_bounds ? 1u : 0u);
                for (size_t byte = 0; byte < 4; ++byte) {
                    AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "%02X", original[byte]);
                }
                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), " replacement=");
                for (size_t byte = 0; byte < 4; ++byte) {
                    AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "%02X", replacement[byte]);
                }
                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), " final=");
                if (in_bounds) {
                    for (size_t byte = 0; byte < 4; ++byte) {
                        AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "%02X", final_bytes[byte]);
                    }
                } else {
                    AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "unavailable");
                }
                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), " state=%s\n", is_replacement ? "replacement" : (is_original ? "original" : "other"));
            }
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "all_replacements=%u\n", all_replacements ? 1u : 0u);

            g_am_patch_diagnostic_pending = true;
            static_cast<void>(TryFlushAmPatchDiagnosticLocked());
        }

        void WriteAmPatchMismatchDiagnostic(const ro::ModuleId &module_id, size_t mapped_size) {
            std::scoped_lock lk(g_am_patch_diagnostic_lock);

            g_am_patch_diagnostic_size = 0;
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "build_id=%s\n", AmPatchDiagnosticBuildId);
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "status=unknown-am-main-module\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "program_id=%016lX\n", static_cast<unsigned long>(ncm::SystemProgramId::Am.value));
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "nso=main\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "exact_match=0\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "patch_applied=0\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "module_id=");
            for (size_t i = 0; i < sizeof(module_id.data); ++i) {
                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "%02X", module_id.data[i]);
            }
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "\n");
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "mapped_size=0x%lX\n", static_cast<unsigned long>(mapped_size));
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "expected_module_count=%lu\n", static_cast<unsigned long>(util::size(AmExposeUsbLaunchResultPatches)));
            for (size_t patch_index = 0; patch_index < util::size(AmExposeUsbLaunchResultPatches); ++patch_index) {
                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "expected_module%lu=", static_cast<unsigned long>(patch_index));
                for (size_t i = 0; i < sizeof(AmExposeUsbLaunchResultPatches[patch_index].module_id.data); ++i) {
                    AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "%02X", AmExposeUsbLaunchResultPatches[patch_index].module_id.data[i]);
                }
                AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "\n");
            }
            AppendAmPatchDiagnostic(std::addressof(g_am_patch_diagnostic_size), "safety=unknown-build-left-untouched\n");

            g_am_patch_diagnostic_pending = true;
            static_cast<void>(TryFlushAmPatchDiagnosticLocked());
        }

    }

    /* Apply IPS patches. */
    void LocateAndApplyIpsPatchesToModule(const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size) {
        if (!EnsureSdCardMounted()) {
            return;
        }

        ro::ModuleId module_id;
        std::memcpy(std::addressof(module_id.data), module_id_data, sizeof(module_id.data));
        ams::patcher::LocateAndApplyIpsPatchesToModule(LoaderSdMountName, NsoPatchesDirectory, NsoPatchesProtectedSize, NsoPatchesProtectedOffset, std::addressof(module_id), reinterpret_cast<u8 *>(mapped_nso), mapped_size);
    }

    /* Apply embedded patches. */
    void ApplyEmbeddedPatchesToModule(const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size) {
        /* Make module id. */
        ro::ModuleId module_id;
        std::memcpy(std::addressof(module_id.data), module_id_data, sizeof(module_id.data));

        if (IsUsb30ForceEnabled()) {
            for (const auto &patch : Usb30ForceEnablePatches) {
                if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) == 0) {
                    for (size_t i = 0; i < patch.num_entries; ++i) {
                        const auto &entry = patch.entries[i];
                        if (entry.offset + entry.size <= mapped_size) {
                            std::memcpy(reinterpret_cast<void *>(mapped_nso + entry.offset), entry.data, entry.size);
                        }
                    }
                }
            }
        }

        /* TODO: Remove this if/when a cleaner solution is implemented by hbmenu/libnx. */
        for (const auto &patch : AmDisableTeardownPatches) {
            if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) == 0) {
                for (size_t i = 0; i < patch.num_entries; ++i) {
                    const auto &entry = patch.entries[i];
                    if (entry.offset + entry.size <= mapped_size) {
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + entry.offset), entry.data, entry.size);
                    }
                }
            }
        }
        
        /* TODO: Remove this if/when a cleaner solution is figured out. */
        for (const auto &patch : WebMemoryPatches) {
            if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) == 0) {
                for (size_t i = 0; i < patch.num_entries; ++i) {
                    const auto &entry = patch.entries[i];
                    if (entry.offset + entry.size <= mapped_size) {
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + entry.offset), entry.data, entry.size);
                    }
                }
            }
        }

        /* Preserve AM's underlying application-start failure across each
         * known result-conversion boundary for the passive USB launch trace.
         * This remains failure-only diagnostics and is restricted to the
         * exact firmware module IDs listed in the table. */
        if constexpr (EnableAmExposeUsbLaunchResultPatches) {
            for (const auto &patch : AmExposeUsbLaunchResultPatches) {
                if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) == 0) {
                    for (size_t i = 0; i < patch.num_entries; ++i) {
                        const auto &entry = patch.entries[i];
                        if (entry.offset + entry.size <= mapped_size) {
                            std::memcpy(reinterpret_cast<void *>(mapped_nso + entry.offset), entry.data, entry.size);
                        }
                    }
                }
            }
        }

        /* Identify which exact NS 22.5.0 pre-PGL call returns the native
         * launch failure. This is an atomic, original-byte-verified diagnostic:
         * if either site is out of bounds or differs from the inspected
         * firmware, neither site is changed. */
        if constexpr (EnableNsPrePglStageTagPatches) {
            for (const auto &patch : NsPrePglStageTagPatches) {
                if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) != 0) {
                    continue;
                }

                NsPrePglPatchDiagnostic diagnostic{};
                diagnostic.apply_status = NsPrePglPatchApplyStatus::ExactPatchApplied;
                diagnostic.pre_patch_words[0] = diagnostic.pre_patch_words[1] = UINT32_MAX;
                diagnostic.final_words[0] = diagnostic.final_words[1] = UINT32_MAX;
                diagnostic.mapped_size = mapped_size;
                std::memcpy(diagnostic.module_id_words, module_id.data, sizeof(diagnostic.module_id_words));

                bool can_apply = true;
                if (patch.num_entries != util::size(NsPrePglStageTagOriginals_22_5_0)) {
                    diagnostic.apply_status = NsPrePglPatchApplyStatus::InvalidEntryCount;
                    can_apply = false;
                }
                for (size_t i = 0; i < std::min(patch.num_entries, util::size(NsPrePglStageTagOriginals_22_5_0)); ++i) {
                    const auto &entry = patch.entries[i];
                    const bool in_bounds = entry.size == sizeof(NsPrePglStageTagOriginals_22_5_0[i]) &&
                                           entry.offset <= mapped_size &&
                                           entry.size <= mapped_size - entry.offset;
                    if (!in_bounds) {
                        diagnostic.apply_status = NsPrePglPatchApplyStatus::EntryOutOfBounds;
                        can_apply = false;
                        continue;
                    }

                    diagnostic.pre_patch_words[i] = ReadPatchWord(reinterpret_cast<const void *>(mapped_nso + entry.offset));
                    if (std::memcmp(reinterpret_cast<const void *>(mapped_nso + entry.offset), NsPrePglStageTagOriginals_22_5_0[i], entry.size) != 0) {
                        if (diagnostic.apply_status != NsPrePglPatchApplyStatus::EntryOutOfBounds) {
                            diagnostic.apply_status = NsPrePglPatchApplyStatus::OriginalByteMismatch;
                        }
                        can_apply = false;
                    }
                }

                if (can_apply) {
                    for (size_t i = 0; i < patch.num_entries; ++i) {
                        const auto &entry = patch.entries[i];
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + entry.offset), entry.data, entry.size);
                    }
                }
                StoreNsPrePglPatchDiagnostic(diagnostic);
            }
        }
    }

    void FinalizeEmbeddedPatchDiagnostics(ncm::ProgramId program_id, bool is_main_nso, const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size) {
        if constexpr (EnableNsPrePglStageTagPatches) {
            if (program_id == ncm::SystemProgramId::Ns && is_main_nso) {
                ro::ModuleId module_id;
                std::memcpy(std::addressof(module_id.data), module_id_data, sizeof(module_id.data));

                bool exact_module = false;
                for (const auto &patch : NsPrePglStageTagPatches) {
                    if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) != 0) {
                        continue;
                    }

                    exact_module = true;
                    NsPrePglPatchDiagnostic diagnostic = GetNsPrePglPatchDiagnostic();
                    diagnostic.final_words[0] = diagnostic.final_words[1] = UINT32_MAX;
                    diagnostic.mapped_size = mapped_size;
                    std::memcpy(diagnostic.module_id_words, module_id.data, sizeof(diagnostic.module_id_words));

                    bool all_replacements = patch.num_entries == util::size(NsPrePglStageTagOriginals_22_5_0);
                    bool all_originals = all_replacements;
                    bool all_in_bounds = all_replacements;
                    for (size_t i = 0; i < std::min(patch.num_entries, util::size(NsPrePglStageTagOriginals_22_5_0)); ++i) {
                        const auto &entry = patch.entries[i];
                        const bool in_bounds = entry.size == sizeof(NsPrePglStageTagOriginals_22_5_0[i]) &&
                                               entry.offset <= mapped_size &&
                                               entry.size <= mapped_size - entry.offset;
                        all_in_bounds &= in_bounds;
                        if (!in_bounds) {
                            all_replacements = false;
                            all_originals = false;
                            continue;
                        }

                        const void *final_bytes = reinterpret_cast<const void *>(mapped_nso + entry.offset);
                        diagnostic.final_words[i] = ReadPatchWord(final_bytes);
                        all_replacements &= std::memcmp(final_bytes, entry.data, entry.size) == 0;
                        all_originals &= std::memcmp(final_bytes, NsPrePglStageTagOriginals_22_5_0[i], entry.size) == 0;
                    }

                    diagnostic.final_status = !all_in_bounds ? NsPrePglPatchFinalStatus::EntryOutOfBounds
                                            : all_replacements ? NsPrePglPatchFinalStatus::ReplacementsVerified
                                            : all_originals ? NsPrePglPatchFinalStatus::OriginalsPresent
                                            : NsPrePglPatchFinalStatus::MixedOrOtherBytes;
                    StoreNsPrePglPatchDiagnostic(diagnostic);
                    break;
                }

                if (!exact_module) {
                    NsPrePglPatchDiagnostic diagnostic{};
                    diagnostic.apply_status = NsPrePglPatchApplyStatus::UnknownNsMainModule;
                    diagnostic.final_status = NsPrePglPatchFinalStatus::UnknownNsMainModule;
                    diagnostic.pre_patch_words[0] = diagnostic.pre_patch_words[1] = UINT32_MAX;
                    diagnostic.final_words[0] = diagnostic.final_words[1] = UINT32_MAX;
                    diagnostic.mapped_size = mapped_size;
                    std::memcpy(diagnostic.module_id_words, module_id.data, sizeof(diagnostic.module_id_words));
                    StoreNsPrePglPatchDiagnostic(diagnostic);
                }
            }
        }

        /* R59 enables only the exact-build in-memory result-preservation
         * patch. Keep Loader's SD diagnostic writer entirely out of the
         * runtime path so AM loading cannot acquire an early filesystem
         * dependency. The passive ams_mitm trace records the preserved
         * launch Result after Home Menu requests the title. */
        if constexpr (!EnableAmExposeUsbLaunchResultPatches || !EnableAmPatchDiagnosticWriter) {
            AMS_UNUSED(program_id, is_main_nso, module_id_data, mapped_nso, mapped_size);
            return;
        }

        /* A prior target may have been inspected before SD initialization.
         * Every later NSO load is a safe opportunity to persist that report. */
        TryFlushAmPatchDiagnostic();

        /* R55 only recognized AM after its exact build ID matched, which made
         * an unknown running AM indistinguishable from Loader not executing.
         * Identify the process and its main NSO independently, then report an
         * unknown module without changing a byte. */
        if (program_id != ncm::SystemProgramId::Am || !is_main_nso) {
            return;
        }

        ro::ModuleId module_id;
        std::memcpy(std::addressof(module_id.data), module_id_data, sizeof(module_id.data));

        for (const auto &patch : AmExposeUsbLaunchResultPatches) {
            if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) == 0) {
                WriteAmPatchDiagnostic(module_id, mapped_nso, mapped_size, patch);
                return;
            }
        }

        WriteAmPatchMismatchDiagnostic(module_id, mapped_size);
    }

    NsPrePglPatchDiagnostic GetNsPrePglPatchDiagnostic() {
        std::scoped_lock lk(g_ns_pre_pgl_patch_diagnostic_lock);
        return g_ns_pre_pgl_patch_diagnostic;
    }

}
