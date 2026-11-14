/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "ncm_content_storage_impl.hpp"
#include <stratosphere/ncm/ncm_usb_title_map.hpp>

namespace ams::ncm {
    class NativeSdContentStorage : public ContentStorageImpl {
        public:
            Result Register(PlaceHolderId placeholder, ContentId content) override {
                R_TRY(ContentStorageImpl::Register(placeholder, content));
                // Native registration replaces any older USB ownership proof,
                // including proof recorded earlier during this boot. Metadata
                // publication must not mistake a native reinstall for USB.
                R_RETURN(UnregisterUsbContentLocation(content));
            }
    };
    static_assert(IsIContentStorage<NativeSdContentStorage>);
}
