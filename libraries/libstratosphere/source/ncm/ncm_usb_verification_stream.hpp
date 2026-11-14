/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>

namespace ams::ncm::usb_verification {
    enum class Step { Ignored, Progress, Complete, Aborted };

    // Observes only successful, contiguous reads of one known-length file.
    // Never authorizes a read, changes its data/result, or clears OS flags.
    template<class Hasher> class Stream {
        Hasher m_hash;
        uint64_t m_total{}, m_done{};
        bool m_active{};
    public:
        bool Active() const { return m_active; }
        uint64_t Done() const { return m_done; }
        uint64_t Total() const { return m_total; }
        void Cancel() { m_active = false; }
        void Begin(uint64_t total) {
            m_total = total; m_done = 0; m_active = total != 0;
            if (m_active) m_hash.Initialize();
        }
        Step Append(uint64_t offset, const void *data, size_t size, bool success,
                    void *digest, size_t digest_size) {
            if (!m_active) return Step::Ignored;
            if (!success || offset != m_done || !data || size == 0
                || size > m_total - m_done || !digest || digest_size != 32) {
                Cancel(); return Step::Aborted;
            }
            m_hash.Update(data, size);
            m_done += size;
            if (m_done != m_total) return Step::Progress;
            m_hash.GetHash(digest, digest_size);
            Cancel(); return Step::Complete;
        }
    };
}
