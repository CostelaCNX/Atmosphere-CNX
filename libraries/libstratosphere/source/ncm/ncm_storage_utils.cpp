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
#include <stratosphere/usb/usb_content_proxy.hpp>

namespace ams::ncm {

    namespace {

        constinit os::SdkMutex g_usb_runtime_mutex;
        bool g_usb_runtime_enabled;
        bool g_usb_install_target_enabled;
        bool g_usb_install_route_active;
        bool g_usb_install_route_selected;
        bool g_usb_native_path_enabled;
        bool g_usb_metadata_route_enabled;
        u32 g_usb_registration_attempts;
        u32 g_usb_registration_successes;
        u32 g_usb_registration_failures;
        u32 g_usb_registration_last_result{ResultSuccess().GetValue()};
        u32 g_usb_ticket_import_attempts;
        u32 g_usb_ticket_import_successes;
        u32 g_usb_ticket_import_failures;
        u32 g_usb_ticket_import_last_result{ResultSuccess().GetValue()};

        StorageList GetDownloadStorageList(StorageId storage_id) {
            /* Do not expose Host as a candidate storage id. Host is only the
             * private implementation behind the stock SdCard content-storage
             * interface; title metadata and installation records must remain
             * associated with the normal Horizon storage id. */
            return storage_id == StorageId::Host ? GetStorageList(StorageId::Any) : GetStorageList(storage_id);
        }

    }

    bool IsUsbContentEnabled() {
        /* NCM starts with routing disabled on every boot. A persistent SD file
         * may be stale before Horizon is initialized, and reading set:sys here
         * previously put USB work into NCM's boot path. AMS explicitly arms
         * this in-memory flag after WaitInitialized() instead. */
        std::scoped_lock lk(g_usb_runtime_mutex);
        return g_usb_runtime_enabled;
    }

    bool IsUsbContentRouteReady() {
        UsbContentStatus status{};
        if (R_FAILED(GetUsbContentStatus(std::addressof(status)))) {
            return false;
        }

        return status.enabled != 0
            && status.running != 0
            && status.ready != 0
            && status.writable != 0
            && status.mounted != 0;
    }

    void SetUsbContentRuntimeEnabled(bool enabled) {
        std::scoped_lock lk(g_usb_runtime_mutex);
        g_usb_runtime_enabled = enabled;
        if (!enabled) {
            g_usb_install_target_enabled = false;
        }
    }

    bool IsUsbContentEnabledForInstall() {
        if constexpr (usb::content::UseFileSystemContentRoute) {
            /* R97 leaves NCM on StorageId::SdCard. The command-110
             * filesystem wrapper chooses the physical byte store, so Host
             * selection and USB metadata maps must remain disabled. */
            return false;
        }
        /* Reading already-mapped USB content and selecting USB for a new
         * install are independent decisions. This distinction lets a caller
         * create ordinary SD content (for example, a forwarder) without
         * making mapped USB titles disappear from Horizon. */
        std::scoped_lock lk(g_usb_runtime_mutex);
        return g_usb_runtime_enabled && g_usb_install_target_enabled;
    }

    bool IsUsbContentInstallTargetEnabled() {
        std::scoped_lock lk(g_usb_runtime_mutex);
        return g_usb_install_target_enabled;
    }

    void SetUsbContentInstallTargetEnabled(bool enabled) {
        std::scoped_lock lk(g_usb_runtime_mutex);
        g_usb_install_target_enabled = enabled;
    }

    bool ShouldUseUsbContentForInstall(StorageId storage_id) {
        if constexpr (usb::content::UseFileSystemContentRoute) {
            AMS_UNUSED(storage_id);
            return false;
        }
        if (storage_id != StorageId::SdCard) {
            return false;
        }

        /* Select USB from the explicit route permission, not from one
         * instantaneous worker-status sample.  The worker starts
         * asynchronously and can still report Starting/WaitingForDevice at
         * the exact moment NCM prepares a task, even though the medium becomes
         * ready immediately afterwards.  Treating that sample as a reason to
         * return SdCard was the cause of silent SD-only installs.
         *
         * The actual Host content operations perform the bounded readiness
         * wait and fail if the medium cannot become usable.  This preserves a
         * safe SD fallback only when the USB route is not enabled at all; an
         * enabled USB install can no longer be redirected silently. */
        return IsUsbContentEnabledForInstall();
    }

    bool IsUsbContentInstallRouteActive() {
        std::scoped_lock lk(g_usb_runtime_mutex);
        return g_usb_install_route_active && g_usb_install_route_selected;
    }

    void SetUsbContentInstallRouteActive(bool active) {
        std::scoped_lock lk(g_usb_runtime_mutex);
        g_usb_install_route_active = active;
        g_usb_install_route_selected = active;
    }

    bool IsUsbContentNativePathEnabled() {
        std::scoped_lock lk(g_usb_runtime_mutex);
        return g_usb_native_path_enabled;
    }

    void SetUsbContentNativePathEnabledLocal(bool enabled) {
        std::scoped_lock lk(g_usb_runtime_mutex);
        g_usb_native_path_enabled = enabled;
    }

    bool IsUsbContentMetadataRouteEnabled() {
        std::scoped_lock lk(g_usb_runtime_mutex);
        return g_usb_metadata_route_enabled;
    }

    void SetUsbContentMetadataRouteEnabledLocal(bool enabled) {
        std::scoped_lock lk(g_usb_runtime_mutex);
        g_usb_metadata_route_enabled = enabled;
    }

    void RecordUsbContentRegistration(Result result) {
        std::scoped_lock lk(g_usb_runtime_mutex);
        ++g_usb_registration_attempts;
        g_usb_registration_last_result = result.GetValue();
        if (R_SUCCEEDED(result)) {
            ++g_usb_registration_successes;
        } else {
            ++g_usb_registration_failures;
        }
    }

    void RecordUsbTicketImport(Result result) {
        std::scoped_lock lk(g_usb_runtime_mutex);
        ++g_usb_ticket_import_attempts;
        g_usb_ticket_import_last_result = result.GetValue();
        if (R_SUCCEEDED(result)) {
            ++g_usb_ticket_import_successes;
        } else {
            ++g_usb_ticket_import_failures;
        }
    }

    void GetUsbContentRegistrationStats(UsbContentStatus *out) {
        AMS_ASSERT(out != nullptr);
        std::scoped_lock lk(g_usb_runtime_mutex);
        out->registration_attempts = g_usb_registration_attempts;
        out->registration_successes = g_usb_registration_successes;
        out->registration_failures = g_usb_registration_failures;
        out->last_registration_result = g_usb_registration_last_result;
        out->ticket_import_attempts = g_usb_ticket_import_attempts;
        out->ticket_import_successes = g_usb_ticket_import_successes;
        out->ticket_import_failures = g_usb_ticket_import_failures;
        out->last_ticket_import_result = g_usb_ticket_import_last_result;
    }

    Result SelectDownloadableStorage(StorageId *out_storage_id, StorageId storage_id, s64 required_size) {
        const auto list = GetDownloadStorageList(storage_id);
        for (s32 i = 0; i < list.Count(); i++) {
            auto candidate = list[i];

            /* Open the content meta database. NOTE: This is unused. */
            ContentMetaDatabase content_meta_database;
            if (R_FAILED(ncm::OpenContentMetaDatabase(std::addressof(content_meta_database), candidate))) {
                continue;
            }

            /* Open the content storage. */
            ContentStorage content_storage;
            if (R_FAILED(ncm::OpenContentStorage(std::addressof(content_storage), candidate))) {
                continue;
            }

            /* Get the free space on this storage. */
            s64 free_space_size;
            if (R_FAILED(content_storage.GetFreeSpaceSize(std::addressof(free_space_size)))) {
                continue;
            }

            /* There must be more free space than is required. */
            if (free_space_size <= required_size) {
                continue;
            }

            /* Output the storage id. */
            *out_storage_id = candidate;
            R_SUCCEED();
        }

        R_THROW(ncm::ResultNotEnoughInstallSpace());
    }

    Result SelectPatchStorage(StorageId *out_storage_id, StorageId storage_id, PatchId patch_id) {
        const auto list = GetDownloadStorageList(storage_id);
        u32 version = 0;
        *out_storage_id = storage_id;

        for (s32 i = 0; i < list.Count(); i++) {
            auto candidate = list[i];

            /* Open the content meta database. */
            ContentMetaDatabase content_meta_database;
            if (R_FAILED(ncm::OpenContentMetaDatabase(std::addressof(content_meta_database), candidate))) {
                continue;
            }

            /* Get the latest key. */
            ContentMetaKey key;
            R_TRY_CATCH(content_meta_database.GetLatest(std::addressof(key), patch_id.value)) {
                R_CATCH(ncm::ResultContentMetaNotFound) { continue; }
            } R_END_TRY_CATCH;

            if (key.version > version) {
                version = key.version;
                *out_storage_id = candidate;
            }
        }

        R_SUCCEED();
    }

    const char *GetStorageIdString(StorageId storage_id) {
        switch (storage_id) {
            case StorageId::None:          return "None";
            case StorageId::Host:          return "Host";
            case StorageId::GameCard:      return "GameCard";
            case StorageId::BuiltInSystem: return "BuiltInSystem";
            case StorageId::BuiltInUser:   return "BuiltInUser";
            default:                       return "(unknown)";
        }
    }

    const char *GetStorageIdStringForPlayReport(StorageId storage_id) {
        switch (storage_id) {
            case StorageId::None:          return "None";
            case StorageId::Host:          return "Host";
            case StorageId::Card:          return "Card";
            case StorageId::BuildInSystem: return "BuildInSystem";
            case StorageId::BuildInUser:   return "BuildInUser";
            default:                       return "(unknown)";
        }
    }

}
