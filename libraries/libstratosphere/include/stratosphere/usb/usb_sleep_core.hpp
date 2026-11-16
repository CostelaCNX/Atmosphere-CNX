/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ams::usb::sleep {
    constexpr std::uint64_t DrainBudgetNs = 5'000'000'000ULL;
    constexpr std::uint64_t ProbeBudgetNs = 5'000'000'000ULL;
    enum class Phase : std::uint32_t { Awake, Draining, Suspended, Resuming, Failed, Stopped };
    enum class Admission { Entered, Wait, Rejected };
    // All calls are serialized by Service's separate admission mutex. No
    // filesystem/host call takes that mutex, including the sleep callback.
    class Gate {
        Phase phase{Phase::Awake};
        std::uint32_t active{};
    public:
        Phase GetPhase() const { return phase; }
        std::uint32_t Active() const { return active; }
        bool Paused() const { return phase != Phase::Awake; }
        Admission Enter() {
            if (phase == Phase::Failed || phase == Phase::Stopped) return Admission::Rejected;
            if (phase != Phase::Awake) return Admission::Wait;
            if (active == std::numeric_limits<std::uint32_t>::max()) return Admission::Rejected;
            ++active; return Admission::Entered;
        }
        bool Leave() { if (!active) return false; --active; return true; }
        bool BeginSleep() {
            if (phase != Phase::Awake) return false;
            phase = Phase::Draining; return true;
        }
        bool Park() {
            if (phase != Phase::Draining || active) return false;
            phase = Phase::Suspended; return true;
        }
        bool BeginWake() {
            if (phase != Phase::Suspended || active) return false;
            phase = Phase::Resuming; return true;
        }
        bool CompleteWake() {
            if (phase != Phase::Resuming || active) return false;
            phase = Phase::Awake; return true;
        }
        void Fail() { if (phase != Phase::Stopped) phase = Phase::Failed; }
        void Stop() { phase = Phase::Stopped; }
        bool Restart() {
            if (phase != Phase::Stopped || active) return false;
            phase = Phase::Awake; return true;
        }
        /* Leave a failed wake only when nothing is admitted. The caller must
         * have quarantined the old device and requested a full reconnect, so
         * this behaves like a fresh hot-plug rather than a retried wake. */
        bool Recover() {
            if (phase != Phase::Failed || active) return false;
            phase = Phase::Awake; return true;
        }
    };
    constexpr std::uint64_t Deadline(std::uint64_t now, std::uint64_t budget) {
        return now > UINT64_MAX - budget ? UINT64_MAX : now + budget;
    }
    constexpr bool IsPowerResetAttention(const std::uint8_t *p, std::size_t n) {
        if (!p || n < 8 || n < 8u + p[7]) return false;
        if ((p[0] & 0x7f) == 0x70)
            return n >= 14 && p[7] >= 6 && (p[2] & 15) == 6 && p[12] == 0x29 && p[13] <= 4;
        if ((p[0] & 0x7f) == 0x72)
            return (p[1] & 15) == 6 && p[2] == 0x29 && p[3] <= 4;
        return false; // Deferred sense and medium-change are never wake retries.
    }
    struct ReadRetry {
        bool clean_resume{};
        bool reset_consumed{};
        std::uint32_t ready_retries{};
        bool fresh_session{};
        bool insertion_consumed{};
        bool RetryFreshInsertion(const std::uint8_t *p, std::size_t n) {
            if (!fresh_session || insertion_consumed || !p || n < 8 || n < 8u + p[7]) return false;
            const auto format = p[0] & 0x7f;
            const bool current = (format == 0x70 && n >= 14 && p[7] >= 6 &&
                                  (p[2] & 15) == 6 && p[12] == 0x28 && p[13] == 0) ||
                                 (format == 0x72 && (p[1] & 15) == 6 && p[2] == 0x28 && p[3] == 0);
            if (current) insertion_consumed = true;
            return current;
        }
        bool RetryReset(const std::uint8_t *p, std::size_t n) {
            if (!clean_resume || reset_consumed || !IsPowerResetAttention(p,n)) return false;
            reset_consumed = true; return true;
        }
        bool RetryReady() { if (ready_retries >= (fresh_session ? 80U : 20U)) return false; ++ready_retries; return true; }
    };
}
