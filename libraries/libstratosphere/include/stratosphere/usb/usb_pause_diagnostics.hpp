/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stratosphere.hpp>
#include "usb_pause_core.hpp"

namespace ams::usb::pause_diagnostics {

    inline Recorder recorder;

    inline uint64_t NowNs() {
        return static_cast<uint64_t>(os::GetSystemTick().ToTimeSpan().GetNanoSeconds());
    }

    class Scope {
        private:
            Stage stage;
            uint64_t start_ns;
            uint64_t bytes;
            uint32_t opcode;
            bool finished{false};

            void End(uint32_t result) {
                if (finished) return;
                finished = true;
                const uint64_t now = NowNs();
                recorder.Record(stage, start_ns, now >= start_ns ? now - start_ns : 0, bytes, opcode, result);
            }

        public:
            explicit Scope(Stage stage, uint64_t bytes = 0, uint32_t opcode = 0)
                : stage(stage), start_ns(NowNs()), bytes(bytes), opcode(opcode) { }
            Scope(const Scope &) = delete;
            Scope &operator=(const Scope &) = delete;
            ~Scope() { this->End(NEXUS_USB_PAUSE_UNKNOWN_RESULT); }
            void Finish(Result result) { this->End(result.GetValue()); }
    };

    template<typename Operation>
    Result Measure(Stage stage, uint64_t bytes, uint32_t opcode, Operation operation) {
        Scope scope(stage, bytes, opcode);
        const Result result = operation();
        scope.Finish(result);
        return result;
    }

    inline bool Snapshot(NexusUsbPauseSnapshot *out, uint64_t generation) {
        if (!recorder.Snapshot(out, 0, generation)) return false;
        /* Timestamp after the coherent copy: every retained event ended
         * before sampled_ns, including a record racing this query. */
        out->sampled_ns = NowNs();
        return true;
    }

}
