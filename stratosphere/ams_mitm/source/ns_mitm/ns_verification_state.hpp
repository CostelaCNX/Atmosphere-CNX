/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <cstdint>

namespace ams::mitm::ns::verification {
    /* Caller supplies a monotonic clock and serializes access. Peek supplies
     * the unambiguous verification scope; logging counters must never decide
     * whether a filesystem can open. This state is not archive validation. */
    class Window {
        struct Entry {
            std::uint64_t token{}, title{}, started{};
            unsigned successes{}, failures{}, opens{};
        } entries[4];
        std::uint64_t sequence{}, overflow_started{};
        bool overflow{};
        static bool Active(const Entry &entry, std::uint64_t now) {
            return entry.token != 0 && now >= entry.started && now - entry.started < LifetimeMs;
        }
    public:
        static constexpr std::uint64_t LifetimeMs = 20 * 60 * 1000;
        bool Active(std::uint64_t now) const {
            for (const auto &entry : entries) if (Active(entry, now)) return true;
            return false;
        }
        std::uint64_t Begin(std::uint64_t id, std::uint64_t now) {
            if (id == 0) return 0;
            for (auto &entry : entries) {
                if (Active(entry, now)) continue;
                if (++sequence == 0) ++sequence;
                entry = {sequence, id, now, 0, 0};
                return sequence;
            }
            overflow = true; overflow_started = now;
            return 0;
        }
        void End(std::uint64_t owner) {
            for (auto &entry : entries) if (owner != 0 && owner == entry.token) entry.token = 0;
        }
        bool Peek(std::uint64_t now, std::uint64_t &id, std::uint64_t &owner) const {
            if (overflow && (now < overflow_started || now - overflow_started < LifetimeMs)) return false;
            const Entry *selected = nullptr;
            for (const auto &entry : entries) {
                if (!Active(entry, now)) continue;
                if (selected != nullptr) return false;
                selected = &entry;
            }
            if (selected == nullptr) return false;
            id = selected->title; owner = selected->token;
            return true;
        }
        bool TakeOpen(std::uint64_t now) {
            std::uint64_t id{}, owner{};
            if (!Peek(now, id, owner)) return false;
            for (auto &entry : entries) {
                if (entry.token != owner) continue;
                if (entry.opens >= 32) return false;
                ++entry.opens;
                return true;
            }
            return false;
        }
        bool Take(std::uint64_t now, bool failed, std::uint64_t &id, std::uint64_t &owner) {
            if (overflow && (now < overflow_started || now - overflow_started < LifetimeMs)) return false;
            Entry *selected = nullptr;
            for (auto &entry : entries) {
                if (!Active(entry, now)) continue;
                if (selected != nullptr) return false; // Concurrent checks cannot be attributed safely.
                selected = &entry;
            }
            if (selected == nullptr) return false;
            unsigned &count = failed ? selected->failures : selected->successes;
            if (count >= 32) return false;
            ++count; id = selected->title; owner = selected->token;
            return true;
        }
    };

    /* The optional detail read is a separate, read-only native command. Its
     * answer must never replace Get's result, even if the two disagree. */
    template<class Dispatch, class Record>
    std::uint32_t ReadResult(std::uint32_t command, bool &detail_read,
                            Dispatch dispatch, Record record) {
        const auto result = dispatch(command);
        record(command, result, false);
        if (command == 3) detail_read = true;
        if (command == 0 && !detail_read) {
            detail_read = true;
            const auto detail = dispatch(3);
            record(3, detail, true);
        }
        return result;
    }
}
