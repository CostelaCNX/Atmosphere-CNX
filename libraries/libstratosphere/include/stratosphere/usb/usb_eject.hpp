// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace ams::usb::eject {
    enum class Phase : uint32_t { Online, Pending, Flushing, Safe, Failed };
    enum class Stage : uint32_t { None, Admission, FilesystemFlush, Unmount, DeviceCache, DeviceRelease, HostRelease, Complete };
    struct Status {
        Phase phase{};
        Stage stage{};
        uint32_t result{}, handles{};
        uint64_t request{}, generation{};
    };
    // All methods run under storage::Service's state mutex. No I/O or IPC.
    // Counting lifetime leases, rather than recent activity, prevents ejecting
    // between two reads/writes on an open file. Old-media releases are ignored.
    class Gate {
        Status value{};
    public:
        Status Get() const { return value; }
        bool Blocked() const { return value.phase != Phase::Online; }
        void NewGeneration(uint64_t generation) { value.generation=generation; value.handles=0; }
        bool Acquire(uint64_t generation) {
            if (Blocked() || generation==0 || generation!=value.generation || value.handles==UINT32_MAX) return false;
            ++value.handles; return true;
        }
        void Release(uint64_t generation) {
            if (generation==value.generation && value.handles) --value.handles;
        }
        bool Request(uint64_t request, uint64_t generation) {
            if (!request || Blocked() || value.handles || !generation || generation!=value.generation) return false;
            value.request=request; value.result=0; value.stage=Stage::Admission; value.phase=Phase::Pending; return true;
        }
        void At(Stage stage) { value.stage=stage; value.phase=Phase::Flushing; }
        void Fail(uint32_t result) { value.result=result; value.phase=Phase::Failed; }
        void Complete() { value.result=0; value.stage=Stage::Complete; value.phase=Phase::Safe; }
        bool Resume() {
            if(value.phase!=Phase::Safe) return false;
            value.phase=Phase::Online; value.stage=Stage::None; value.result=0; value.request=0; return true;
        }
    };
}
