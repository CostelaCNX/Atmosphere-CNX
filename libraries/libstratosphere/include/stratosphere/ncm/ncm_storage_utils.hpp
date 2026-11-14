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
#pragma once
#include <stratosphere/ncm/ncm_storage_id.hpp>
#include <stratosphere/ncm/ncm_content_meta_id.hpp>

namespace ams::ncm {

    class StorageList {
        public:
            static constexpr s32 MaxCount = 10;
        private:
            StorageId m_ids[MaxCount];
            s32 m_count;
        public:
            constexpr StorageList() : m_ids(), m_count() { /* ... */ }

            void Push(StorageId storage_id) {
                AMS_ABORT_UNLESS(m_count < MaxCount);

                for (s32 i = 0; i < MaxCount; i++) {
                    if (m_ids[i] == storage_id) {
                        return;
                    }
                }

                m_ids[m_count++] = storage_id;
            }

            s32 Count() const {
                return m_count;
            }

            StorageId operator[](s32 i) const {
                AMS_ABORT_UNLESS(i < m_count);
                return m_ids[i];
            }
    };

    constexpr StorageList GetStorageList(StorageId storage_id) {
        StorageList list;
        switch (storage_id) {
            case StorageId::BuiltInSystem:
            case StorageId::BuiltInUser:
            case StorageId::SdCard:
                list.Push(storage_id);
                break;
            case StorageId::Any:
                list.Push(StorageId::SdCard);
                list.Push(StorageId::BuiltInUser);
                break;
            AMS_UNREACHABLE_DEFAULT_CASE();
        }

        return list;
    }

    bool IsUsbContentEnabled();
    void SetUsbContentRuntimeEnabled(bool enabled);

    /* Install-only gate. This is deliberately separate from the ordinary
     * content-storage status path: NCM may consult the SD-side bridge here
     * only at the real install boundary, after Horizon has initialized. */
    bool IsUsbContentEnabledForInstall();
    bool IsUsbContentInstallTargetEnabled();
    void SetUsbContentInstallTargetEnabled(bool enabled);
    bool ShouldUseUsbContentForInstall(StorageId storage_id);
    bool IsUsbContentInstallRouteActive();
    void SetUsbContentInstallRouteActive(bool active);

    /* Expose the real USB mount in Host ContentStorage::GetPath when the
     * opt-in native-path test is enabled. */
    bool IsUsbContentNativePathEnabled();
    void SetUsbContentNativePathEnabledLocal(bool enabled);

    /* Diagnostic-only metadata route. This moves the active NCM metadata
     * interface to the in-memory Host database; it is not persistent across
     * reboot and therefore remains opt-in. */
    bool IsUsbContentMetadataRouteEnabled();
    void SetUsbContentMetadataRouteEnabledLocal(bool enabled);

    /* Installation-boundary diagnostics. */
    void RecordUsbContentRegistration(Result result);
    void RecordUsbTicketImport(Result result);

    /* Used by loader's opt-in USB title resolver.  This performs a bounded
     * readiness check only after the in-memory route permission is enabled. */
    bool IsUsbContentStorageReadyForRouting();

    /* A non-blocking snapshot of the USB Host content backend. This is kept
     * separate from usb::storage::Status so NCM's monitor ABI does not expose
     * the worker's internal geometry fields. */
    struct UsbContentStatus {
        u8 enabled;
        u8 running;
        u8 ready;
        u8 writable;
        u8 mounted;
        u8 registered;
        u8 filesystem_type;
        u8 state;
        u32 last_result;
        u32 last_io_result;
        u64 media_generation;
        u64 free_space;
        u64 total_space;
        char partition_name[37];
        char vendor[9];
        char product[17];
        char revision[5];
        u32 route_requests;
        u32 route_successes;
        u32 route_fallbacks;
        u8 last_requested_storage;
        u8 last_selected_storage;
        u16 reserved;
        u32 last_route_result;
        u32 probe_stage;
        u32 probe_result;
        u32 available_interface_count;
        u32 acquired_interface_count;
        u32 probe_attempts;
        u32 registration_attempts;
        u32 registration_successes;
        u32 registration_failures;
        u32 last_registration_result;
        u32 ticket_import_attempts;
        u32 ticket_import_successes;
        u32 ticket_import_failures;
        u32 last_ticket_import_result;
    };

    Result GetUsbContentStatus(UsbContentStatus *out);
    /* Query the route state from the NCM service.  Unlike the legacy
     * IsUsbContentEnabled()/IsUsbContentStorageReadyForRouting() pair, this
     * crosses the process boundary when called by loader or AMS MITM and
     * therefore observes the state owned by NCM rather than a process-local
     * copy. */
    bool IsUsbContentRouteReady();
    Result GetUsbContentStatusImpl(UsbContentStatus *out);
    void GetUsbContentRegistrationStats(UsbContentStatus *out);
    Result EnsureUsbContentServiceStarted();
    Result RefreshUsbContentStatusImpl();
    void GetUsbContentRouteStats(UsbContentStatus *out);
    Result SelectDownloadableStorage(StorageId *out_storage_id, StorageId storage_id, s64 required_size);
    Result SelectPatchStorage(StorageId *out_storage_id, StorageId storage_id, PatchId patch_id);
    const char *GetStorageIdString(StorageId storage_id);
    const char *GetStorageIdStringForPlayReport(StorageId storage_id);

}
