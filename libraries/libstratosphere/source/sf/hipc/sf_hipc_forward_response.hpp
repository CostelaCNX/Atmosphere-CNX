/*
 * Copyright (c) Atmosphère-NX
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <cstddef>
#include <cstring>

namespace ams::sf::hipc::impl {

    /* Observers run after SendSyncRequest, before the original reply is sent.
     * A nested client IPC uses the same TLS buffer even when the observer
     * does not explicitly write to the response. Keep a stable, aligned view
     * for parsing, then restore the complete wire reply before handle parsing.
     * This owns only bytes: it must not close/duplicate reply handles or domain
     * objects. Buffer descriptors continue to refer to the original mappings. */
    template<std::size_t Size>
    class ScopedForwardResponse final {
        private:
            void *const m_message;
            alignas(0x10) unsigned char m_response[Size];
        public:
            explicit ScopedForwardResponse(void *message) noexcept : m_message(message) {
                static_assert(Size > 0);
                std::memcpy(m_response, m_message, Size);
            }

            ScopedForwardResponse(const ScopedForwardResponse &) = delete;
            ScopedForwardResponse &operator=(const ScopedForwardResponse &) = delete;

            ~ScopedForwardResponse() noexcept {
                std::memcpy(m_message, m_response, Size);
            }

            const void *GetPointer() const noexcept {
                return m_response;
            }
    };

}
