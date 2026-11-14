#pragma once
#include <cstdint>

namespace ams::mitm::usb_storage::impl {
    /* One appletAE dispatch thread, including its cloned sessions. Callers
     * synchronize access. No allocations, IPC, file access or borrowed data. */
    struct AppletForwardProgress {
        enum Phase { Unseen, BeforeNativeSend, NativeReturned, ObserverEntered, ObserverReturned };
        std::uint64_t revision{}, call{}, program{}, session{}, input0{};
        std::int64_t changed_ms{};
        std::uint32_t command{}, object{}, input_size{}, native_result{};
        Phase phase{Unseen};
        bool decoded{}, native_result_known{};

        void Begin(std::uint64_t pid, std::uint64_t key, std::uint32_t cmd,
                   std::uint32_t obj, std::uint32_t size, std::uint64_t input,
                   bool parsed, std::int64_t now) {
            ++revision; ++call;
            program = pid; session = key; command = cmd; object = obj;
            input_size = size; input0 = input; decoded = parsed;
            native_result = 0; native_result_known = false;
            phase = BeforeNativeSend; changed_ms = now;
        }
        bool NativeReturn(std::uint64_t key, std::uint32_t result, std::int64_t now) {
            if (phase != BeforeNativeSend || key != session) return false;
            native_result = result; native_result_known = true;
            phase = NativeReturned; changed_ms = now; ++revision;
            return true;
        }
        bool Observer(std::uint64_t key, bool completed, std::int64_t now) {
            if (key != session || !native_result_known || native_result != 0) return false;
            if (phase != (completed ? ObserverEntered : NativeReturned)) return false;
            phase = completed ? ObserverReturned : ObserverEntered;
            changed_ms = now; ++revision;
            return true;
        }
        bool Pending() const {
            return phase == BeforeNativeSend || phase == ObserverEntered
                || (phase == NativeReturned && native_result == 0);
        }
        const char *PhaseName() const {
            switch (phase) {
                case BeforeNativeSend: return "before_native_send";
                case NativeReturned: return "native_returned";
                case ObserverEntered: return "observer_entered";
                case ObserverReturned: return "observer_returned";
                default: return "unseen";
            }
        }
    };
}
