/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>
#include "fs_shim.h"
#include "fsmitm_domain_object.hpp"

namespace ams::mitm::fs::impl {
    /* R157: a game may mount its RomFS without permission to browse the SD.
     * Reserve an empty native service object instead of mounting an SD FS.
     * The object belongs to the SAME forwarded domain and lives as long as
     * our storage. Never call Add/Commit on it, forward storage methods to it,
     * or invent a local ID on failure. No save or filesystem is opened here. */
    class StorageDomainBackend {
        private:
            std::shared_ptr<::Service> m_forward;
            const bool m_is_domain;
        public:
            using Object = ::Service;
            explicit StorageDomainBackend(std::shared_ptr<::Service> forward)
                : m_forward(std::move(forward)), m_is_domain(serviceIsDomain(m_forward.get())) { }
            bool IsDomain() const { return m_is_domain; }
            Result Open(Object *out) {
                R_RETURN(fsOpenMultiCommitManagerFwd(m_forward.get(), out));
            }
            u32 GetId(const Object &object) const { return object.object_id; }
            void Close(Object *object) { serviceClose(object); }
    };
    using StorageDomainObject = ScopedDomainObject<StorageDomainBackend>;
}
