/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
namespace ams::usb::sleep_diagnostics {
    enum PrepareStage : std::uint32_t {
        NoPrepare, CheckVolume, CaptureInterface, FlushVolume, CaptureSerial, WriteMarker,
        SyncDevice, VerifyMarker, ReadFingerprint, UseCheckpoint, Parked
    };
    inline std::atomic<std::uint32_t> prepare_stage{}, checkpoint_result{}, checkpoint_used{};
    inline std::atomic<std::uint32_t> phase{}, prepares{}, resumes{}, failures{}, last_result{};
    inline std::atomic<std::uint64_t> generation_before{}, generation_after{}, last_prepare_ns{}, last_resume_ns{};
    inline std::atomic<std::uint32_t> reset_attentions{}, ready_retries{}, identity_failures{};
    inline std::atomic<std::uint32_t> marker_armed{}, rebind_attempts{}, rebind_successes{}, rebind_stage{}, rebind_result{}, rebind_candidates{};
    inline std::atomic<std::uint32_t> rebind_queries{}, rebind_query_result{}, rebind_visible_max{}, rebind_exact_max{};
}
