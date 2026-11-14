/* SPDX-License-Identifier: GPL-2.0-only */
#include "usb_game_wake_trace.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "actual_game_wake_format.hpp"

using namespace ams::mitm::usb_storage::game_wake;

int main() {
    auto *game = Bind(0x1234);
    assert(game && Bind(0x1234) == game);
    auto long_read = Begin(game, Op::Read, 0, 64, 1024, 10);
    BeginWake();
    BeginWake();
    char report[1600]{};
    auto size = Format(report, sizeof(report));
    assert(size > 0 && size < int(sizeof(report)));
    assert(std::strstr(report, "usb_game_io_inflight=1\n"));
    assert(std::strstr(report, "usb_game_io_pending=1,0,0,0,10,0,64,1024,0xFFFFFFFF"));
    assert(std::strstr(report, "usb_game_io_started=0,0,0,0,0,0"));
    Finish(long_read, 0, 97);
    const auto range = static_cast<unsigned>(Op::Range);
    auto query = Begin(game, Op::Range, 5, 32, 96, 105);
    Finish(query, 0x177202, 120);
    assert(game->crossed.Read(2) == 1);
    assert(game->finished[0].Read(2) == 1 && game->began[0].Read(2) == 0);
    assert(game->errors.Read(2) == 1 && game->began[range].Read(2) == 1);
    assert(Format(report, sizeof(report)) > 0);
    assert(std::strstr(report, "usb_game_io_last_error=1,2,2,5,105,120,32,96,0x00177202"));

    BeginWake();
    auto *other = Bind(0x5678);
    auto b = Begin(other, Op::Flush, 0, 0, 0, 300);
    auto a = Begin(game, Op::GetSize, 0, 0, 0, 301);
    assert(Format(report, sizeof(report)) > 0);
    assert(std::strstr(report, "usb_game_io_title=0000000000005678\n"));
    assert(std::strstr(report, "usb_game_io_started=0,0,0,0,1,0\n"));
    Finish(b, 0, 302);
    Finish(a, 0, 303);
    std::vector<Ticket> live;
    for (unsigned i = 0; i != 12; ++i) live.push_back(Begin(game, Op::Read, 0, i, i + 16, 315 + i));
    assert(game->active.load() == 12 && game->drops.Read(3) == 4);
    for (auto &ticket : live) Finish(ticket, 0, 600);
    assert(game->active.load() == 0 && game->finished[0].Read(3) == 12);
    std::puts("PASS wake-crossing read, current epoch totals, title separation, error result, bounded pending slot overflow");

    // Force multiple producers onto the same live and completed records.
    // Every sampled field is derived from a different writer's unique start
    // time; a partially overwritten sample fails the consistency checks.
    BeginWake();
    std::atomic<unsigned> clock{1000}, ready{}, finished{};
    std::atomic<bool> go{};
    auto *test = Bind(0x8765);
    std::vector<std::thread> workers;
    for (unsigned w = 0; w != 12; ++w) workers.emplace_back([&]() {
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();
        for (unsigned k = 0; k < 4000; ++k) {
            const auto t = clock.fetch_add(1);
            auto call = Begin(test, Op::Range, t + 1, t + 2, t + 3, t);
            Finish(call, t + 4, t + 5);
        }
        finished.fetch_add(1);
    });
    while (ready.load() != workers.size()) std::this_thread::yield();
    go.store(true);
    unsigned observed = 0;
    const auto check = [&](const Sample &slot, bool running) {
        Call c{};
        if (!slot.Read(c) || c.title != 0x8765) return;
        assert(c.epoch == 4 && c.op == range && c.range == c.start + 1);
        assert(c.offset == std::int64_t(c.start + 2) && c.size == std::int64_t(c.start + 3));
        if (running) assert(c.result == UINT32_MAX && !c.end);
        else assert(c.result == c.start + 4 && c.end == c.start + 5);
        ++observed;
    };
    do {
        for (auto &slot : pending) check(slot, true);
        check(test->last, false);
        check(test->failure, false);
    } while (finished.load() != workers.size());
    for (auto &worker : workers) worker.join();
    check(test->failure, false);
    assert(observed && !test->active.load());
    assert(test->began[range].Read(4) == 48000 && test->finished[range].Read(4) == 48000);
    assert(test->errors.Read(4) == 48000 && !test->crossed.Read(4));
    char tiny[8];
    assert(Format(tiny, sizeof(tiny)) >= int(sizeof(tiny)));
    std::printf("PASS 48000 concurrent calls, %u coherent samples, exact totals and isolated results\n", observed);
}
