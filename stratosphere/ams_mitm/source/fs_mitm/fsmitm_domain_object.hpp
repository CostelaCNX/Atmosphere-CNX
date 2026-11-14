/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::fs::impl {

    /* A MITM domain shares IDs with the original service. A locally chosen ID
     * can shadow an un-intercepted object (including the client's SD log file).
     * Keep a native reservation alive for every local object, not just its root.
     * The backend seam lets host tests exercise the actual ownership logic. */
    template<typename Backend>
    class ScopedDomainObject {
        private:
            std::shared_ptr<Backend> m_backend;
            typename Backend::Object m_object{};
            bool m_initialized = false;
            bool m_open = false;
        public:
            explicit ScopedDomainObject(std::shared_ptr<Backend> backend) : m_backend(std::move(backend)) { }
            ScopedDomainObject(const ScopedDomainObject &) = delete;
            ScopedDomainObject &operator=(const ScopedDomainObject &) = delete;
            ~ScopedDomainObject() {
                if (m_open) m_backend->Close(std::addressof(m_object));
            }

            Result Initialize() {
                R_UNLESS(m_backend != nullptr && !m_initialized, ams::fs::ResultInvalidArgument());
                m_initialized = true;
                if (m_backend->IsDomain()) {
                    R_TRY(m_backend->Open(std::addressof(m_object)));
                    m_open = true;
                    R_UNLESS(m_backend->GetId(m_object) != 0, ams::fs::ResultInvalidArgument());
                }
                R_SUCCEED();
            }

            u32 GetId() const { return m_open ? m_backend->GetId(m_object) : 0; }
            const std::shared_ptr<Backend> &GetBackend() const { return m_backend; }
    };
}
