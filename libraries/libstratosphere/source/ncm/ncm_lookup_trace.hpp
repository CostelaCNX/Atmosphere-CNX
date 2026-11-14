/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>
#include "ncm_recovery_diagnostics.hpp"

namespace ams::ncm::lookup_trace {
    inline constinit os::SdkMutex g_mutex;
    inline usb_recovery::LookupCapture g_capture;
    inline std::atomic_bool g_armed{false};

    inline u64 Now() { return static_cast<u64>(os::GetSystemTick().ToTimeSpan().GetNanoSeconds()); }
    inline u64 Begin(u64 title_id) {
        std::scoped_lock lock(g_mutex);
        const u64 session = g_capture.Begin(title_id, Now());
        g_armed.store(title_id != 0, std::memory_order_release);
        return session;
    }
    inline Result End(void *out, size_t size, u64 session) {
        R_UNLESS(out != nullptr && size >= sizeof(usb_recovery::LookupSnapshot), ncm::ResultBufferInsufficient());
        std::scoped_lock lock(g_mutex);
        R_UNLESS(g_capture.End(session, Now()), fs::ResultInvalidArgument());
        g_armed.store(false, std::memory_order_release);
        std::memcpy(out, std::addressof(g_capture.Snapshot()), sizeof(usb_recovery::LookupSnapshot));
        R_SUCCEED();
    }
    inline void Append(usb_recovery::LookupRecord record) {
        if (!g_armed.load(std::memory_order_acquire)) return;
        std::scoped_lock lock(g_mutex);
        const u64 now = Now();
        g_capture.Append(record, now);
        if (!g_capture.Active(now)) g_armed.store(false, std::memory_order_release);
    }
    inline void Detail(usb_recovery::LookupRecord &record, const char *text) {
        if (text != nullptr) {
            for (size_t i = 0; i + 1 < sizeof(record.detail) && text[i] != 0; ++i) record.detail[i] = text[i];
        }
    }
    inline void Metadata(usb_recovery::LookupOperation op, StorageId storage, u64 title_id, u32 version,
                         ContentType type, util::optional<u8> offset, Result result,
                         const ContentId *content_id = nullptr, const char *detail = nullptr) {
        if (!g_armed.load(std::memory_order_acquire)) return;
        usb_recovery::LookupRecord record{};
        record.operation = static_cast<u32>(op); record.result = result.GetValue();
        record.title_id = title_id; record.metadata_version = version;
        record.storage_id = static_cast<u8>(storage); record.content_type = static_cast<u8>(type);
        record.id_offset = offset ? *offset : 0;
        if (offset) record.flags |= usb_recovery::OffsetSpecified;
        if (R_SUCCEEDED(result) && content_id != nullptr) {
            record.flags |= usb_recovery::OutputValid;
            static_assert(sizeof(*content_id) == sizeof(record.content_id));
            std::memcpy(record.content_id, content_id, sizeof(record.content_id));
        }
        Detail(record, detail); Append(record);
    }
    inline void Storage(usb_recovery::LookupOperation op, StorageId storage, ContentId id, Result result,
                        bool output_valid, bool present, const char *detail) {
        if (!g_armed.load(std::memory_order_acquire)) return;
        usb_recovery::LookupRecord record{};
        record.operation = static_cast<u32>(op); record.result = result.GetValue();
        record.storage_id = static_cast<u8>(storage);
        std::memcpy(record.content_id, std::addressof(id), sizeof(record.content_id));
        if (output_valid && R_SUCCEEDED(result)) record.flags |= usb_recovery::OutputValid;
        if (present && R_SUCCEEDED(result)) record.flags |= usb_recovery::Present;
        Detail(record, detail); Append(record);
    }
}
