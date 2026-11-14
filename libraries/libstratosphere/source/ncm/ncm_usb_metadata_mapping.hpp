/* USB metadata validation shared with the host regression tests. */
#pragma once
#include <stratosphere.hpp>
#include <stratosphere/ncm/ncm_usb_title_map.hpp>

namespace ams::ncm::impl {

    inline bool GetRegisteredUsbApplicationIds(ContentId *out_meta, ContentId *out_control, ContentId *out_program,
                                         const void *data, size_t size, bool require_registered_locations = true,
                                         bool require_registration_this_boot = true) {
        /* Check the wire layout before reading caller-supplied content
         * info. In particular, an extended-header/count mismatch must
         * never become an out-of-bounds scan in the NCM service. */
        if (data == nullptr || size < sizeof(ContentMetaHeader)) {
            return false;
        }
        ContentMetaHeader header{};
        std::memcpy(std::addressof(header), data, sizeof(header));
        size_t offset = sizeof(header);
        if (header.extended_header_size > size - offset) {
            return false;
        }
        offset += header.extended_header_size;
        if (header.content_count == 0 || header.content_count > (size - offset) / sizeof(ContentInfo)) {
            return false;
        }
        const size_t infos_end = offset + header.content_count * sizeof(ContentInfo);
        if (header.content_meta_count > (size - infos_end) / sizeof(ContentMetaInfo)) {
            return false;
        }

        *out_meta = *out_control = *out_program = InvalidContentId;
        u16 control_offset = 256;
        u16 program_offset = 256;
        bool registered_this_boot = false;
        for (u16 i = 0; i < header.content_count; ++i) {
            ContentInfo info{};
            std::memcpy(std::addressof(info), static_cast<const u8 *>(data) + offset + i * sizeof(info), sizeof(info));
            if (info.GetId() == InvalidContentId
                || (require_registered_locations && !IsUsbContentLocationRegistered(info.GetId()))) {
                return false;
            }
            if (require_registered_locations) {
                registered_this_boot |= WasUsbContentRegisteredThisBoot(info.GetId());
            }
            switch (info.GetType()) {
                case ContentType::Meta:
                    *out_meta = info.GetId();
                    break;
                case ContentType::Control:
                    if (info.GetIdOffset() < control_offset) {
                        control_offset = info.GetIdOffset();
                        *out_control = info.GetId();
                    }
                    break;
                case ContentType::Program:
                    if (info.GetIdOffset() < program_offset) {
                        program_offset = info.GetIdOffset();
                        *out_program = info.GetId();
                    }
                    break;
                default:
                    break;
            }
        }
        return (!require_registered_locations || !require_registration_this_boot || registered_this_boot) && *out_meta != InvalidContentId
            && *out_control != InvalidContentId && *out_program != InvalidContentId;
    }

    inline bool HasSameRegisteredUsbApplicationMap(ProgramId title, const void *data, size_t size) {
        ContentId meta{}, control{}, program{}, old_meta{}, old_control{}, old_program{};
        // This is preservation evidence only. It must never create a new map.
        return IsUsbTitleMapped(title)
            && GetRegisteredUsbApplicationIds(std::addressof(meta), std::addressof(control),
                std::addressof(program), data, size, true, false)
            && R_SUCCEEDED(GetUsbTitleContentIds(title, std::addressof(old_meta),
                std::addressof(old_control), std::addressof(old_program)))
            && meta == old_meta && control == old_control && program == old_program;
    }


}
