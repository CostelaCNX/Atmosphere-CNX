/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include "usb_pause_protocol.h"

namespace ams::usb::pause_diagnostics {

    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "USB pause telemetry must not introduce an atomic-library lock");

    enum class Stage : uint32_t {
        RequestRead, RequestWrite, RequestStatus,
        VolumeReadWait, VolumeWriteWait, VolumeSpaceWait,
        FileRead, FileWrite, SpaceQuery, Command,
        BotCbw, BotData, BotCsw, KeepAlive,
        UasCommand, UasData, UasStatus, StartupGate,
        HostSubmit, HostWait, HostReport,
    };
    static_assert(static_cast<uint32_t>(Stage::StartupGate) == 17);
    static_assert(static_cast<uint32_t>(Stage::HostReport) + 1 == NEXUS_USB_PAUSE_STAGES);

    constexpr uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
        return b > std::numeric_limits<uint64_t>::max() - a
            ? std::numeric_limits<uint64_t>::max() : a + b;
    }

    class Recorder {
        private:
            #if defined(NEXUS_USB_PAUSE_HOST_TEST)
            friend struct RecorderTestAccess;
            #endif
            std::atomic_flag busy = ATOMIC_FLAG_INIT;
            std::atomic<uint64_t> dropped{0};
            uint64_t sequence{0};
            NexusUsbPauseStage stages[NEXUS_USB_PAUSE_STAGES]{};
            NexusUsbPauseEvent events[NEXUS_USB_PAUSE_RING]{};
            uint32_t next{0};
            uint32_t count{0};

            struct Unlock {
                std::atomic_flag &flag;
                ~Unlock() { flag.clear(std::memory_order_release); }
            };

        public:
            Recorder() = default;
            Recorder(const Recorder &) = delete;
            Recorder &operator=(const Recorder &) = delete;

            bool Record(Stage stage, uint64_t start_ns, uint64_t duration_ns,
                        uint64_t bytes = 0, uint32_t opcode = 0,
                        uint32_t result = NEXUS_USB_PAUSE_UNKNOWN_RESULT) {
                const auto index = static_cast<uint32_t>(stage);
                if (index >= NEXUS_USB_PAUSE_STAGES) return false;
                if (busy.test_and_set(std::memory_order_acquire)) {
                    /* Unsigned atomic addition has defined modulo-2^64
                     * overflow. No retry loop on this diagnostic path. */
                    dropped.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                const Unlock unlock{busy};
                auto &stats = stages[index];
                stats.calls = SaturatingAdd(stats.calls, 1);
                stats.total_ns = SaturatingAdd(stats.total_ns, duration_ns);
                if (duration_ns > stats.max_ns) stats.max_ns = duration_ns;
                sequence = SaturatingAdd(sequence, 1);
                if (duration_ns >= NEXUS_USB_PAUSE_THRESHOLD_NS) {
                    stats.slow_calls = SaturatingAdd(stats.slow_calls, 1);
                    events[next] = {sequence, start_ns, duration_ns, bytes, index, opcode, result, 0};
                    next = (next + 1) % NEXUS_USB_PAUSE_RING;
                    if (count < NEXUS_USB_PAUSE_RING) ++count;
                }
                return true;
            }

            bool Snapshot(NexusUsbPauseSnapshot *out, uint64_t sampled_ns, uint64_t generation) {
                if (out == nullptr || busy.test_and_set(std::memory_order_acquire)) return false;
                const Unlock unlock{busy};
                *out = {};
                out->magic = NEXUS_USB_PAUSE_MAGIC;
                out->version = NEXUS_USB_PAUSE_VERSION;
                out->size = sizeof(*out);
                out->stage_count = NEXUS_USB_PAUSE_STAGES;
                out->sampled_ns = sampled_ns;
                out->media_generation = generation;
                out->sequence = sequence;
                out->dropped = dropped.load(std::memory_order_relaxed);
                out->threshold_ns = NEXUS_USB_PAUSE_THRESHOLD_NS;
                out->event_count = count;
                for (uint32_t i = 0; i < NEXUS_USB_PAUSE_STAGES; ++i) out->stages[i] = stages[i];
                const uint32_t first = (next + NEXUS_USB_PAUSE_RING - count) % NEXUS_USB_PAUSE_RING;
                for (uint32_t i = 0; i < count; ++i) out->events[i] = events[(first + i) % NEXUS_USB_PAUSE_RING];
                return true;
            }
    };

}
