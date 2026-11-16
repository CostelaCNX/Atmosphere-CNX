/* Installed-content selection, shared by all USB applications. GPL-2.0-only. */
#pragma once
#include <stratosphere/usb/usb_launch_content.hpp>

namespace ams::ldr {
    /* MP1: program IDs inside a collection are not independent CNMT keys.
     * Use this same lookup before Host.Has/GetPath AND when binding code.
     * Only a missing exact key permits a parent lookup; never hide IO errors,
     * change an existing key's owner, or substitute the menu's Program NCA. */
    template<class Database>
    Result ResolveUsbProgramContent(ncm::ContentMetaKey *out_key, ncm::ContentId *out_content,
                                    Database &db, ncm::ProgramId program_id) {
        *out_key = {};
        *out_content = ncm::InvalidContentId;
        ncm::ContentMetaKey key{};
        Result result = db.GetLatest(std::addressof(key), program_id.value);
        if (ncm::ResultContentMetaNotFound::Includes(result) && (program_id.value & 0xff) != 0) {
            result = db.GetLatest(std::addressof(key), program_id.value & ~u64{0xff});
        }
        R_TRY(result);
        R_UNLESS(key.type == ncm::ContentMetaType::Application
            && program_id.value >= key.id && program_id.value - key.id <= 255,
            ncm::ResultContentMetaNotFound());
        ncm::ContentId content{};
        R_TRY(db.GetContentIdByTypeAndIdOffset(std::addressof(content), key,
            ncm::ContentType::Program, static_cast<u8>(program_id.value - key.id)));
        R_UNLESS(content != ncm::InvalidContentId, ncm::ResultContentNotFound());
        *out_key = key;
        *out_content = content;
        R_SUCCEED();
    }

    /* Template allows the exact production selection logic to run against a
     * deterministic metadata/Host fake in host regression tests. No FS hook
     * calls this: Loader is an external NCM client. */
    template<class Database, class Host>
    Result SelectUsbLaunchContent(usb::content::LaunchContent *out, Database &db, Host &host,
                                  ncm::ProgramId program_id, bool *out_is_usb) {
        *out = {};
        *out_is_usb = false;
        ncm::ContentMetaKey base_key{};
        R_TRY(ResolveUsbProgramContent(std::addressof(base_key), std::addressof(out->base), db, program_id));
        const u8 id_offset = static_cast<u8>(program_id.value - base_key.id);
        bool has_base = false;
        R_TRY(host.Has(std::addressof(has_base), out->base));
        R_SUCCEED_IF(!has_base); // ordinary SD title: leave completely native
        *out_is_usb = true;
        out->version = usb::content::LaunchContentVersion;
        out->size = sizeof(*out);
        out->program_id = program_id.value;
        out->application_id = base_key.id;

        ncm::PatchId patch_id{};
        R_TRY(db.GetPatchId(std::addressof(patch_id), base_key));
        if (patch_id.value != 0) {
            ncm::ContentMetaKey patch_key{};
            const Result patch_result = db.GetLatest(std::addressof(patch_key), patch_id.value);
            if (!ncm::ResultContentMetaNotFound::Includes(patch_result)) {
                R_TRY(patch_result);
                R_UNLESS(patch_key.type == ncm::ContentMetaType::Patch, ncm::ResultInvalidContentMetaKey());
                /* Confirm the patch owner from the native metadata index. */
                ncm::ContentMetaKey owned[1]{};
                typename Database::ListCount count{};
                R_TRY(db.ListContentMetaResult(std::addressof(count), owned, 1, ncm::ContentMetaType::Patch,
                    ncm::ApplicationId{base_key.id}, patch_id.value, patch_id.value));
                R_UNLESS(count.written != 0, ncm::ResultInvalidContentMetaKey());
                R_TRY(db.GetContentIdByTypeAndIdOffset(std::addressof(out->patch), patch_key, ncm::ContentType::Program, id_offset));
                bool has_patch = false;
                R_TRY(host.Has(std::addressof(has_patch), out->patch));
                R_UNLESS(has_patch, ncm::ResultContentNotFound()); // never silently boot base instead
                out->patch_id = patch_key.id;
                out->patch_version = patch_key.version;
            }
        }

        /* Iterate by content-meta ID, not by fixed total count. GetLatest
         * collapses multiple versions; the native owner filter prevents a
         * different application's add-ons from entering this snapshot. */
        u64 next_id = 0;
        for (;;) {
            ncm::ContentMetaKey keys[8]{};
            typename Database::ListCount count{};
            R_TRY(db.ListContentMetaResult(std::addressof(count), keys, util::size(keys),
                ncm::ContentMetaType::AddOnContent, ncm::ApplicationId{base_key.id}, next_id, UINT64_MAX));
            R_UNLESS(count.written >= 0 && static_cast<size_t>(count.written) <= util::size(keys), ncm::ResultInvalidContentMetaKey());
            if (count.written == 0) break;
            for (s32 i = 0; i < count.written; ++i) {
                if (keys[i].id < next_id) continue; // another version of an already visited ID
                ncm::ContentMetaKey key{};
                R_TRY(db.GetLatest(std::addressof(key), keys[i].id));
                R_UNLESS(key.type == ncm::ContentMetaType::AddOnContent, ncm::ResultInvalidContentMetaKey());
                ncm::ContentId data{};
                R_TRY(db.GetContentIdByType(std::addressof(data), key, ncm::ContentType::Data));
                bool present = false;
                R_TRY(host.Has(std::addressof(present), data));
                if (present) {
                    /* Newer DLC can itself have a DataPatch. Do not silently
                     * open the unpatched Data archive in that case. Legacy
                     * extended headers legitimately have no DataPatch field. */
                    ncm::DataPatchId data_patch{};
                    const Result data_patch_result = db.GetDataPatchId(std::addressof(data_patch), key);
                    if (!ncm::ResultInvalidAddOnContentMetaExtendedHeader::Includes(data_patch_result)) {
                        R_TRY(data_patch_result);
                        if (data_patch.value != 0) {
                            ncm::ContentMetaKey data_patch_key{};
                            const Result installed = db.GetLatest(std::addressof(data_patch_key), data_patch.value);
                            if (!ncm::ResultContentMetaNotFound::Includes(installed)) {
                                R_TRY(installed);
                                R_THROW(fs::ResultUnsupportedOperation());
                            }
                        }
                    }
                    R_UNLESS(out->add_on_count < usb::content::MaxLaunchAddOns, ncm::ResultBufferInsufficient());
                    out->add_ons[out->add_on_count++] = {key.id, data, key.version, static_cast<u32>(ncm::StorageId::SdCard)};
                }
                R_UNLESS(key.id != UINT64_MAX, ncm::ResultInvalidContentMetaKey());
                next_id = key.id + 1;
            }
        }
        R_UNLESS(usb::content::IsValidLaunchContent(*out), ncm::ResultInvalidContentMetaKey());
        R_SUCCEED();
    }
}
