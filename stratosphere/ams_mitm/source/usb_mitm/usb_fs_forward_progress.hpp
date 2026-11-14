#pragma once
#include <cstddef>
#include "usb_applet_forward_progress.hpp"

namespace ams::mitm::usb_storage::impl {
    /* FS has multiple dispatch threads. Keep independent session slots and
     * never evict a pending call. Callers synchronize this fixed-size state;
     * it owns no client pointers and performs no allocation, IPC or I/O. */
    struct FsForwardProgress {
        static constexpr std::size_t Capacity = 16;
        struct Entry {
            AppletForwardProgress progress{};
            std::uint64_t client{};
            std::uint32_t cmif_result{};
            bool used{}, cmif_known{};
        };
        Entry entries[Capacity]{};
        std::uint64_t revision{}, calls{}, tracking_drops{};
        std::uint64_t success_details_suppressed{}, failure_details_suppressed{};

        Entry *Find(std::uint64_t session) {
            for (auto &entry : entries) {
                if (entry.used && entry.progress.session == session) return &entry;
            }
            return nullptr;
        }

        bool Begin(std::uint64_t client, std::uint64_t program, std::uint64_t session,
                   std::uint32_t command, std::uint32_t object, std::uint32_t size,
                   std::uint64_t input, bool decoded, std::int64_t now) {
            ++revision;
            ++calls;
            Entry *target = Find(session);
            if (target != nullptr && target->progress.Pending()) {
                ++tracking_drops;
                return false;
            }
            if (target == nullptr) {
                for (auto &entry : entries) {
                    if (!entry.used) { target = &entry; break; }
                    if (!entry.progress.Pending()
                            && (target == nullptr || entry.progress.call < target->progress.call)) {
                        target = &entry;
                    }
                }
            }
            if (target == nullptr) { ++tracking_drops; return false; }
            *target = {};
            target->used = true;
            target->client = client;
            target->progress.Begin(program, session, command, object, size, input, decoded, now);
            target->progress.call = calls;
            return true;
        }

        bool NativeReturn(std::uint64_t session, std::uint32_t result, std::int64_t now) {
            auto *entry = Find(session);
            if (entry == nullptr || !entry->progress.NativeReturn(session, result, now)) return false;
            ++revision;
            return true;
        }

        bool Observer(std::uint64_t session, bool completed, std::int64_t now) {
            auto *entry = Find(session);
            if (entry == nullptr || !entry->progress.Observer(session, completed, now)) return false;
            ++revision;
            return true;
        }

        bool Reply(std::uint64_t session, std::uint32_t result) {
            auto *entry = Find(session);
            if (entry == nullptr || entry->progress.phase != AppletForwardProgress::ObserverEntered) return false;
            entry->cmif_result = result;
            entry->cmif_known = true;
            ++revision;
            return true;
        }

        void SuppressDetail(bool failed) {
            if (failed) ++failure_details_suppressed;
            else ++success_details_suppressed;
            ++revision;
        }

        bool Pending() const {
            for (const auto &entry : entries) {
                if (entry.used && entry.progress.Pending()) return true;
            }
            return false;
        }
    };
}
