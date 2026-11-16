// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace ams::mitm::usb_storage::system_power {

// A separate gate for the custom monitor/publisher. Never share or nest this
// with the storage gate: a monitor already admitted here can call storage.
class ActivityGate {
    static constexpr std::uint32_t ClosedBit = 0x80000000u;
    std::atomic<std::uint32_t> m_state;
public:
    explicit ActivityGate(bool closed = false) : m_state(closed ? ClosedBit : 0) {}
    bool TryEnter() {
        auto state = m_state.load(std::memory_order_acquire);
        while (!(state & ClosedBit)) {
            if ((state & ~ClosedBit) == ClosedBit - 1) return false;
            if (m_state.compare_exchange_weak(state, state + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        return false;
    }
    void Leave() { m_state.fetch_sub(1, std::memory_order_acq_rel); }
    void Close() { m_state.fetch_or(ClosedBit, std::memory_order_acq_rel); }
    void Open() { m_state.fetch_and(~ClosedBit, std::memory_order_release); }
    bool Closed() const { return (m_state.load(std::memory_order_acquire) & ClosedBit) != 0; }
    std::uint32_t Active() const { return m_state.load(std::memory_order_acquire) & ~ClosedBit; }
};

enum class Phase : std::uint32_t { Awake, Preparing, Asleep, Resuming, Failed };
struct Outcome { std::uint32_t result; bool ack_attempted; };

class Coordinator {
    Phase m_phase{Phase::Awake};
    std::uint32_t m_failure{};
public:
    Phase GetPhase() const { return m_phase; }
    std::uint32_t Failure() const { return m_failure; }
    template<class Backend> void Fail(std::uint32_t result, Backend &backend) {
        backend.CloseBackground();
        if (!m_failure) m_failure = result;
        m_phase = Phase::Failed;
        backend.FailStorage(result);
    }
    // Only the PSC watcher changes the coordinator. The recovery worker returns
    // its result through a mailbox; it never opens the background activity gate.
    // The storage worker may leave its failed state once it was allowed at
    // FullAwake. Track that here so the next sleep drains storage normally
    // instead of acknowledging PSC while USB I/O is live again.
    template<class Backend> void AdoptRecoveredStorage(Backend &backend) {
        if (m_phase == Phase::Failed && backend.IsStorageRecovered()) {
            m_phase = Phase::Awake;
            m_failure = 0;
        }
    }
    template<class Backend> void PollWake(Backend &backend) {
        AdoptRecoveredStorage(backend);
        if (m_phase != Phase::Resuming) return;
        std::uint32_t result{};
        if (!backend.TakeWakeResult(result)) return;
        if (result) {
            Fail(result, backend);
            // FullAwake was acknowledged before recovery started, so the
            // worker may rediscover the drive as a fresh hot-plug.
            backend.AllowStorageRecovery();
        } else {
            m_phase = Phase::Awake;
        }
        // The awake request was already acknowledged before this job started.
        // Failed recovery permits only the existing SD-only diagnostic path.
        backend.OpenBackground();
    }
    // Called only after GetRequest succeeds. Pending activity means NO ACK:
    // retry the same request after a bounded wait, never pretend it is quiet.
    // On descent, start quiescing at MinimumAwake while USB may still be
    // powered. On ascent the volume is already parked; only FullAwake opens it.
    template<class Backend> Outcome Handle(std::uint32_t state, Backend &backend) {
        PollWake(backend);
        std::uint32_t result = m_phase == Phase::Failed ? m_failure : 0;
        bool open_after_ack = false;
        bool start_after_ack = false;
        if (m_phase == Phase::Failed) {
            if (state == 0) {
                // SD-only failure reporting may restart after a known awake
                // request. USB is not retried here; after the ACK the storage
                // worker is allowed to rediscover the drive as a new device.
                open_after_ack = true;
            } else {
                backend.DenyStorageRecovery();
                backend.CloseBackground();
                const auto drain = backend.DrainBackground();
                if (drain) return {drain, false};
            }
        } else {
            if (state == 0) {
                if (m_phase == Phase::Asleep) {
                    result = backend.ReserveWake();
                    if (!result) {
                        m_phase = Phase::Resuming;
                        start_after_ack = true;
                    } else {
                        open_after_ack = true;
                    }
                } else if (m_phase != Phase::Awake && m_phase != Phase::Resuming) {
                    // Runtime retries one pending request. A different wake
                    // during partial prepare is not a completed suspension.
                    result = backend.InvalidStateResult();
                }
            } else if (state >= 1 && state <= 5) {
                // If sleep is requested again, the host dependency stays awake
                // until this recovery finishes and the new suspend drains I/O.
                if (m_phase == Phase::Resuming) return {backend.BusyResult(), false};
                if (m_phase != Phase::Asleep) {
                    m_phase = Phase::Preparing;
                    backend.CloseBackground();
                    result = backend.DrainBackground();
                    if (result) return {result, false};
                    if (!result) result = backend.PrepareStorage();
                    if (result && !backend.IsStorageQuiescent()) return {result, false};
                    if (!result) m_phase = Phase::Asleep;
                }
            } else {
                result = backend.InvalidStateResult();
            }
            if (result) Fail(result, backend);
        }
        if (m_phase == Phase::Failed &&
            (!backend.IsBackgroundQuiescent() || !backend.IsStorageQuiescent())) {
            return {m_failure, false};
        }
        const auto ack_result = backend.Acknowledge(state);
        if (ack_result) {
            Fail(ack_result, backend);
            if (!result) result = ack_result;
        } else if (start_after_ack) {
            backend.StartRecovery();
        } else if (open_after_ack) {
            if (m_phase != Phase::Failed) m_phase = Phase::Awake;
            else if (state == 0) backend.AllowStorageRecovery();
            backend.OpenBackground();
        }
        return {result, true};
    }
};

}
