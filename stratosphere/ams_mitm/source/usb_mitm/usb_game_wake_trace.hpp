/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ams::mitm::usb_storage::game_wake {
    enum class Op : std::uint32_t { Read, GetSize, Range, Write, Flush, SetSize, Count };
    constexpr auto OpCount = static_cast<std::size_t>(Op::Count);

    // Count events according to the epoch at the event, so a read that spans a
    // full wake can finish in its new epoch. A delayed old event cannot reset it.
    struct EpochCount {
        std::atomic<std::uint64_t> value{};
        void Add(std::uint32_t epoch) {
            auto old = value.load(std::memory_order_relaxed);
            for (;;) {
                const auto old_epoch = static_cast<std::uint32_t>(old >> 32);
                if (old_epoch > epoch) return;
                auto count = old_epoch == epoch ? static_cast<std::uint32_t>(old) : 0u;
                if (count == UINT32_MAX) return;
                const auto next = (std::uint64_t(epoch) << 32) | (count + 1u);
                if (value.compare_exchange_weak(old, next, std::memory_order_relaxed)) return;
            }
        }
        std::uint32_t Read(std::uint32_t epoch) const {
            const auto v = value.load(std::memory_order_relaxed);
            return (v >> 32) == epoch ? static_cast<std::uint32_t>(v) : 0;
        }
    };

    struct Call {
        std::uint64_t title{}, start{}, end{};
        std::int64_t offset{}, size{};
        std::uint32_t epoch{}, op{}, range{}, result{};
    };

    // Two-bit state: 0 free, 1 writing, 2 published. The upper bits change
    // with every claim. A reader racing a writer drops the whole sample.
    class Sample {
        std::atomic<std::uint64_t> stamp{}, title{}, start{}, end{};
        std::atomic<std::int64_t> offset{}, size{};
        std::atomic<std::uint32_t> epoch{}, op{}, range{}, result{};
    public:
        bool Publish(const Call &call, bool live) {
            auto old = stamp.load(std::memory_order_relaxed);
            if ((old & 1) || (live && (old & 2))) return false;
            const auto next = (old & ~std::uint64_t{3}) + 4;
            if (!stamp.compare_exchange_strong(old, next + 1, std::memory_order_acq_rel)) return false;
            std::atomic_thread_fence(std::memory_order_release);
            if (live || end.load(std::memory_order_relaxed) <= call.end) {
                title.store(call.title, std::memory_order_relaxed);
                start.store(call.start, std::memory_order_relaxed);
                end.store(call.end, std::memory_order_relaxed);
                offset.store(call.offset, std::memory_order_relaxed);
                size.store(call.size, std::memory_order_relaxed);
                epoch.store(call.epoch, std::memory_order_relaxed);
                op.store(call.op, std::memory_order_relaxed);
                range.store(call.range, std::memory_order_relaxed);
                result.store(call.result, std::memory_order_relaxed);
            }
            stamp.store(next + 2, std::memory_order_release);
            return true;
        }
        void ReleaseLive() { stamp.fetch_add(2, std::memory_order_release); }
        // The report reader is cold. Share one snapshot routine across the
        // pending, completed and failed samples in the fixed-size boot module.
        [[gnu::noinline]] bool Read(Call &out) const {
            const auto before = stamp.load(std::memory_order_acquire);
            if ((before & 3) != 2) return false;
            Call found{title.load(std::memory_order_relaxed), start.load(std::memory_order_relaxed),
                end.load(std::memory_order_relaxed), offset.load(std::memory_order_relaxed), size.load(std::memory_order_relaxed),
                epoch.load(std::memory_order_relaxed), op.load(std::memory_order_relaxed),
                range.load(std::memory_order_relaxed), result.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (stamp.load(std::memory_order_relaxed) != before) return false;
            out = found;
            return true;
        }
    };

    struct Title {
        std::atomic<std::uint64_t> id{};
        std::atomic<std::uint32_t> active{};
        EpochCount began[OpCount]{}, finished[OpCount]{}, crossed{}, errors{}, drops{};
        Sample last{}, failure{};
    };
    inline std::atomic<std::uint32_t> wake_epoch{}, title_overflow{};
    inline Title titles[16]{};
    inline std::atomic<Title *> recent{};
    inline Sample pending[8]{};

    inline Title *Bind(std::uint64_t id) {
        if (!id) return nullptr;
        for (auto &title : titles) {
            auto current = title.id.load(std::memory_order_relaxed);
            if (!current && title.id.compare_exchange_strong(current, id, std::memory_order_relaxed)) return &title;
            if (current == id) return &title;
        }
        title_overflow.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    inline void BeginWake() { wake_epoch.fetch_add(1, std::memory_order_acq_rel); }
    struct Ticket { Title *title; Sample *slot; Call call; };
    inline Ticket Begin(Title *title, Op op, std::uint32_t range, std::int64_t offset, std::int64_t size, std::uint64_t now) {
        Ticket ticket{title, nullptr, {}};
        if (!title) return ticket;
        const auto epoch = wake_epoch.load(std::memory_order_acquire);
        const auto index = static_cast<std::uint32_t>(op);
        ticket.call = {title->id.load(std::memory_order_relaxed), now, 0, offset, size, epoch, index, range, UINT32_MAX};
        title->active.fetch_add(1, std::memory_order_relaxed);
        title->began[index].Add(epoch);
        if (recent.load(std::memory_order_relaxed) != title) recent.store(title, std::memory_order_relaxed);
        for (auto &slot : pending) {
            if (slot.Publish(ticket.call, true)) { ticket.slot = &slot; break; }
        }
        if (!ticket.slot) title->drops.Add(epoch);
        return ticket;
    }
    inline void Finish(Ticket &ticket, std::uint32_t result, std::uint64_t now) {
        if (!ticket.title) return;
        auto &title = *ticket.title;
        const auto epoch = wake_epoch.load(std::memory_order_acquire);
        ticket.call.result = result;
        ticket.call.end = now;
        if (!title.last.Publish(ticket.call, false)) title.drops.Add(epoch);
        if (result) {
            title.errors.Add(epoch);
            if (!title.failure.Publish(ticket.call, false)) title.drops.Add(epoch);
        }
        if (ticket.slot) ticket.slot->ReleaseLive();
        title.finished[ticket.call.op].Add(epoch);
        if (ticket.call.epoch != epoch) title.crossed.Add(epoch);
        title.active.fetch_sub(1, std::memory_order_relaxed);
    }
    int Format(char *out, std::size_t capacity);
}
