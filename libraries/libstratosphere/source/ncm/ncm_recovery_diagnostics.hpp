/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Private R20 diagnostic protocol. No public NCM interface or USB wire ABI changes.
namespace usb_recovery {
    inline constexpr std::uint32_t TraceMagic = 0x3944434e; // NCD9
    inline constexpr std::uint32_t TraceVersion = 1;
    inline constexpr std::uint32_t BeginTraceCommand = 65000;
    inline constexpr std::uint32_t EndTraceCommand = 65001;
    inline constexpr std::size_t TraceCapacity = 64;
    inline constexpr std::uint64_t TraceTimeoutNs = 10'000'000'000ULL;

    enum class LookupOperation : std::uint32_t {
        MetadataContentInfo = 1, MetadataLatestKey = 2,
        OverlayHas = 3, OverlayPath = 4, NativeHas = 5, NativePath = 6,
        OpenStorage = 7, OpenMetadata = 8,
        OverlaySize = 9, OverlayRead = 10, OverlayRights = 11,
        OverlayProgramId = 12, OverlayCount = 13, OverlayList = 14,
    };
    enum RecordFlags : std::uint8_t { OutputValid = 1, OffsetSpecified = 2, Present = 4 };
    struct LookupRecord {
        std::uint64_t sequence;
        std::uint64_t title_id; // Zero means storage operation, not a guessed title.
        std::uint32_t operation;
        std::uint32_t result;
        std::uint32_t metadata_version;
        std::uint8_t storage_id;
        std::uint8_t content_type;
        std::uint8_t id_offset;
        std::uint8_t flags;
        std::uint8_t content_id[16];
        char detail[48];
    };
    static_assert(sizeof(LookupRecord) == 96 && offsetof(LookupRecord, content_id) == 32);
    struct LookupSnapshot {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t record_size;
        std::uint32_t count;
        std::uint32_t dropped;
        std::uint32_t flags; // Bit 0: expired. Bit 1: ended by the client.
        std::uint64_t session;
        std::uint64_t title_id;
        LookupRecord records[TraceCapacity];
    };
    static_assert(offsetof(LookupSnapshot, records) == 40);
    static_assert(sizeof(LookupSnapshot) == 6184 && std::is_trivially_copyable_v<LookupSnapshot>);
    inline bool IsValidSnapshot(const LookupSnapshot &snapshot, std::uint64_t session, std::uint64_t title_id) {
        return snapshot.magic == TraceMagic && snapshot.version == TraceVersion &&
            snapshot.record_size == sizeof(LookupRecord) && snapshot.count <= TraceCapacity &&
            snapshot.session == session && snapshot.title_id == title_id;
    }

    // Caller provides synchronization. No allocation, filesystem, IPC or clock calls.
    class LookupCapture {
        LookupSnapshot m_snapshot{};
        std::uint64_t m_next_session{};
        std::uint64_t m_started{};
        bool m_active{};
      public:
        std::uint64_t Begin(std::uint64_t title_id, std::uint64_t now) {
            std::memset(&m_snapshot, 0, sizeof(m_snapshot));
            if (++m_next_session == 0) ++m_next_session;
            m_snapshot.magic = TraceMagic;
            m_snapshot.version = TraceVersion;
            m_snapshot.record_size = sizeof(LookupRecord);
            m_snapshot.session = m_next_session;
            m_snapshot.title_id = title_id;
            m_started = now;
            m_active = title_id != 0;
            return m_next_session;
        }
        bool Active(std::uint64_t now) {
            if (m_active && (now < m_started || now - m_started >= TraceTimeoutNs)) {
                m_active = false;
                m_snapshot.flags |= 1;
            }
            return m_active;
        }
        void Append(LookupRecord record, std::uint64_t now) {
            if (!Active(now)) return;
            if (record.title_id != 0 && record.title_id != m_snapshot.title_id &&
                !(m_snapshot.title_id <= UINT64_MAX - 0x800 && record.title_id == m_snapshot.title_id + 0x800)) return;
            record.sequence = static_cast<std::uint64_t>(m_snapshot.count) + m_snapshot.dropped + 1;
            record.detail[sizeof(record.detail) - 1] = 0;
            if (m_snapshot.count == TraceCapacity) { ++m_snapshot.dropped; return; }
            m_snapshot.records[m_snapshot.count++] = record;
        }
        bool End(std::uint64_t session, std::uint64_t now) {
            if (session == 0 || session != m_snapshot.session) return false;
            Active(now); m_active = false; m_snapshot.flags |= 2;
            return true;
        }
        const LookupSnapshot &Snapshot() const { return m_snapshot; }
    };

    // CRC-32/ISO-HDLC. Diagnostic comparison only, not authentication.
    inline std::uint32_t ControlDataCrc32(const void *data, std::size_t size) {
        auto bytes = static_cast<const std::uint8_t *>(data);
        std::uint32_t crc = 0xffffffff;
        for (std::size_t i = 0; i < size; ++i) {
            crc ^= bytes[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }
}
