/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "usb_keep_alive_test_protocol.h"

namespace ams::usb::keep_alive_test {
    enum class Outcome { Success, InvalidParameter, Busy, Denied, MediaChanged };
    /* All methods are externally serialized by the storage lease mutex.
     * The worker also owns the volume mutex before consulting this state. */
    class Lease {
        private:
            uint64_t owner{}, token{}, generation{}, deadline{}, next_token{};
            uint64_t skipped{}, acquisitions{}, expirations{}, restores{}, generation_cancels{};
            static void Count(uint64_t &n) { if (n != UINT64_MAX) ++n; }
            void Clear() { owner = token = generation = deadline = 0; }
            void Observe(uint64_t now, uint64_t current_generation) {
                if (!owner) return;
                if (generation != current_generation) { Count(generation_cancels); Clear(); }
                else if (now >= deadline) { Count(expirations); Clear(); }
            }
        public:
            static bool ValidRequest(const NexusUsbKeepAliveRequest &r) {
                if (r.operation == NEXUS_USB_KEEP_ALIVE_SUSPEND)
                    return r.generation && !r.token && r.duration_ms && r.duration_ms <= NEXUS_USB_KEEP_ALIVE_MAX_MS;
                return (r.operation == NEXUS_USB_KEEP_ALIVE_QUERY || r.operation == NEXUS_USB_KEEP_ALIVE_RESTORE) && !r.duration_ms;
            }
            Outcome Control(uint64_t now, uint64_t current_generation, uint64_t caller,
                    const NexusUsbKeepAliveRequest &r, NexusUsbKeepAliveStatus *out) {
                if (!out || !caller || !ValidRequest(r)) return Outcome::InvalidParameter;
                Observe(now, current_generation);
                if (r.operation == NEXUS_USB_KEEP_ALIVE_SUSPEND) {
                    if (r.generation != current_generation) return Outcome::MediaChanged;
                    if (owner) return Outcome::Busy;
                    const uint64_t duration = uint64_t(r.duration_ms) * 1000000;
                    if (next_token == UINT64_MAX || duration > UINT64_MAX - now) return Outcome::InvalidParameter;
                    owner = caller; token = ++next_token; generation = current_generation;
                    deadline = now + duration; Count(acquisitions);
                } else if (r.operation == NEXUS_USB_KEEP_ALIVE_RESTORE && owner) {
                    if (caller != owner || r.token != token) return Outcome::Denied;
                    if (r.generation != generation) return Outcome::MediaChanged;
                    Count(restores); Clear();
                }
                Snapshot(now, current_generation, caller, out);
                return Outcome::Success;
            }
            void Snapshot(uint64_t now, uint64_t current_generation, uint64_t caller, NexusUsbKeepAliveStatus *out) {
                Observe(now, current_generation);
                *out = {};
                out->magic = NEXUS_USB_KEEP_ALIVE_MAGIC; out->version = NEXUS_USB_KEEP_ALIVE_VERSION;
                out->size = sizeof(*out); out->sampled_ns = now; out->media_generation = current_generation;
                out->flags = owner ? NEXUS_USB_KEEP_ALIVE_ACTIVE | (caller == owner ? NEXUS_USB_KEEP_ALIVE_OWNED : 0) : 0;
                out->owner_pid = owner; out->token = token; out->deadline_ns = deadline;
                out->skipped_polls = skipped; out->acquisitions = acquisitions; out->expirations = expirations;
                out->restores = restores; out->generation_cancels = generation_cancels;
            }
            bool ShouldSuppress(uint64_t now, uint64_t current_generation) {
                Observe(now, current_generation);
                if (!owner) return false;
                Count(skipped); return true;
            }
    };
}
