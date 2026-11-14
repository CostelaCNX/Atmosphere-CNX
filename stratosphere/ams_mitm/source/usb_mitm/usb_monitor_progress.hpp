#pragma once
#include <cstdint>

namespace ams::mitm::usb_storage::impl {
    /* The monitor writes this under the trace mutex. The independent trace
     * worker reads it under that mutex; neither holds it across any IPC. */
    struct MonitorProgress {
        enum Phase { Unseen, Starting, RouteCommand, RefreshCommand, StatusQuery,
            RouteReconcile, StatusPublish, LastReadyPublish, NcmTracePublish,
            Notification, CacheRefresh, Sleeping };
        Phase phase{Unseen};
        std::uint64_t revision{};
        std::int64_t changed_ms{};
        std::uint32_t last_status_result{};
        bool status_result_known{};

        void SetPhase(Phase next, std::int64_t now) {
            phase = next; changed_ms = now; ++revision;
        }
        void StatusReturned(std::uint32_t result) {
            last_status_result = result; status_result_known = true; ++revision;
        }
        bool Pending() const { return phase != Unseen && phase != Sleeping; }
        const char *PhaseName() const {
            switch (phase) {
                case Starting: return "starting";
                case RouteCommand: return "route_command";
                case RefreshCommand: return "refresh_command";
                case StatusQuery: return "ncm_status_query";
                case RouteReconcile: return "route_reconcile";
                case StatusPublish: return "status_file_publish";
                case LastReadyPublish: return "last_ready_publish";
                case NcmTracePublish: return "ncm_trace_publish";
                case Notification: return "notification";
                case CacheRefresh: return "mapped_title_cache_refresh";
                case Sleeping: return "sleeping";
                default: return "unseen";
            }
        }
    };
}
