/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>

// Explicitly included by the transport and status writer, not the public PCH.
// Cumulative observations; successful recovery does not erase failure evidence.
namespace ams::usb::runtime_diagnostics {
    inline std::atomic<std::uint64_t> empty_reports{0}, invalid_reports{0};
    inline std::atomic<std::uint64_t> ready_retries{0}, ready_recovered{0};
    inline std::atomic<std::uint32_t> last_stage{0}, last_opcode{0}, last_result{0};
    inline std::atomic<std::uint32_t> last_sense_action{0};
    // 1=foreground I/O, 2=keep-alive, 3=host query error, 4=interface absent.
    inline std::atomic<std::uint32_t> disconnect_reason{0};
}
