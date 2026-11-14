/* Exact identity checks shared by the USB control mount and host tests. */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::usb_storage::impl {

    inline bool MatchesUsbContentIdentity(u64 mapped_program_id, const ncm::ContentId &mapped_content_id,
                                         u64 requested_program_id, const ncm::ContentId &requested_content_id) {
        /* Zero is the FS API's unspecified program id. A nonstandard/nonzero
         * id is still an identity, not permission to substitute another title.
         * The exact requested archive must also match: being the only mapped
         * USB title, or having a matching title with a different archive, is
         * never sufficient evidence for a redirect. */
        return mapped_program_id != 0
            && mapped_content_id != ncm::InvalidContentId
            && mapped_content_id == requested_content_id
            && (requested_program_id == 0 || requested_program_id == mapped_program_id);
    }

    inline bool MatchesUsbControlIdentity(u64 mapped_program_id, const ncm::ContentId &mapped_content_id,
                                         u64 requested_program_id, const ncm::ContentId &requested_content_id) {
        return MatchesUsbContentIdentity(mapped_program_id, mapped_content_id, requested_program_id, requested_content_id);
    }

}
