/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "fsmitm_domain_filesystem.hpp"
#include "fsmitm_domain_object.hpp"
#include "fsmitm_storage_domain.hpp"
#include "fs_shim.h"
#include "../usb_mitm/usb_launch_trace.hpp"
#include "../usb_mitm/usb_game_wake_trace.hpp"
#include "../../../../libraries/libstratosphere/source/ncm/ncm_recovery_diagnostics.hpp"
#include <cstdio>

namespace ams::mitm::usb_storage::game_wake {
    [[gnu::noinline]] static bool AppendCall(char *out, std::size_t capacity, int &used,
                                              const char *label, const Call &sample) {
        const auto space = capacity - static_cast<std::size_t>(used);
        const int bytes = std::snprintf(out + used, space, "usb_game_io_%s=%u,%u,%u,%u,%llu,%llu,%lld,%lld,0x%08X\n",
            label, sample.title ? 1u : 0u, sample.epoch, sample.op, sample.range,
            static_cast<unsigned long long>(sample.start), static_cast<unsigned long long>(sample.end),
            static_cast<long long>(sample.offset), static_cast<long long>(sample.size), sample.result);
        if (bytes < 0 || static_cast<std::size_t>(bytes) >= space) return false;
        used += bytes;
        return true;
    }

    int Format(char *out, std::size_t capacity) {
        const auto epoch = wake_epoch.load(std::memory_order_acquire);
        Title *title = recent.load(std::memory_order_relaxed);
        Call active{}, done{}, error{};
        for (auto &slot : pending) {
            Call sample{};
            if (slot.Read(sample) && (!active.title || sample.start < active.start)) active = sample;
        }
        if (active.title) title = Bind(active.title);
        std::uint32_t started[OpCount]{}, completed[OpCount]{};
        if (title) {
            title->last.Read(done);
            title->failure.Read(error);
            for (std::size_t i = 0; i < OpCount; ++i) {
                started[i] = title->began[i].Read(epoch);
                completed[i] = title->finished[i].Read(epoch);
            }
        }
        const int header = std::snprintf(out, capacity,
            "usb_game_io_epoch=%u\nusb_game_io_title=%016llX\nusb_game_io_ops=read,get_size,range,write,flush,set_size\n"
            "usb_game_io_started=%u,%u,%u,%u,%u,%u\nusb_game_io_completed=%u,%u,%u,%u,%u,%u\n"
            "usb_game_io_inflight=%u\nusb_game_io_crossed=%u\nusb_game_io_errors=%u\nusb_game_io_sample_drops=%u\n"
            "usb_game_io_untracked_opens=%u\n"
            "usb_game_io_fields=valid,start_epoch,op,range,start_ns,end_ns,offset,size,result\n",
            epoch, title ? static_cast<unsigned long long>(title->id.load(std::memory_order_relaxed)) : 0ULL,
            started[0],started[1],started[2],started[3],started[4],started[5],
            completed[0],completed[1],completed[2],completed[3],completed[4],completed[5],
            title ? title->active.load(std::memory_order_relaxed) : 0,
            title ? title->crossed.Read(epoch) : 0, title ? title->errors.Read(epoch) : 0,
            title ? title->drops.Read(epoch) : 0, title_overflow.load(std::memory_order_relaxed));
        if (header < 0 || static_cast<std::size_t>(header) >= capacity) return header;
        int used = header;
        if (!AppendCall(out, capacity, used, "pending", active) ||
            !AppendCall(out, capacity, used, "last", done) ||
            !AppendCall(out, capacity, used, "last_error", error)) return -1;
        return used;
    }
}

namespace ams::mitm::fs {
    namespace {
        namespace afs = ::ams::fs;

        class NativeDomainBackend {
            private:
                std::shared_ptr<::Service> m_forward;
                const bool m_is_domain;
            public:
                using Object = ::FsFileSystem;
                explicit NativeDomainBackend(std::shared_ptr<::Service> forward)
                    : m_forward(std::move(forward)), m_is_domain(serviceIsDomain(m_forward.get())) { }
                bool IsDomain() const { return m_is_domain; }
                Result Open(Object *out) {
                    /* Reserve on THIS client's native domain, not ams_mitm's
                     * global FS connection. This opens a service object only:
                     * no SD file/directory is read, created or written. */
                    R_RETURN(fsOpenSdCardFileSystemFwd(m_forward.get(), out));
                }
                u32 GetId(const Object &object) const { return object.s.object_id; }
                void Close(Object *object) { serviceClose(&object->s); }
        };
        using DomainObject = impl::ScopedDomainObject<NativeDomainBackend>;

        Result OpenDomainObject(std::unique_ptr<DomainObject> *out, const std::shared_ptr<NativeDomainBackend> &backend) {
            auto object = std::make_unique<DomainObject>(backend);
            R_UNLESS(object != nullptr, afs::ResultAllocationMemoryFailed());
            R_TRY(object->Initialize());
            *out = std::move(object);
            R_SUCCEED();
        }

        class DomainFile final : public afs::fsa::IFile {
            private:
                std::unique_ptr<DomainObject> m_object;
                std::unique_ptr<afs::fsa::IFile> m_file;
                ncm::ProgramId m_program_id;
                bool m_trace_control;
                bool m_is_control_nacp;
                std::atomic_bool m_read_traced{false};
            public:
                DomainFile(std::unique_ptr<DomainObject> object, std::unique_ptr<afs::fsa::IFile> file,
                    ncm::ProgramId id, bool trace_control, bool is_control_nacp)
                    : m_object(std::move(object)), m_file(std::move(file)), m_program_id(id),
                      m_trace_control(trace_control), m_is_control_nacp(is_control_nacp) { }
                sf::cmif::DomainObjectId GetDomainObjectId() const override { return {m_object->GetId()}; }
            private:
                Result DoRead(size_t *out, s64 offset, void *buffer, size_t size, const afs::ReadOption &option) override {
                    const Result result = m_file->Read(out, offset, buffer, size, option);
                    if (m_trace_control) {
                        const size_t actual = R_SUCCEEDED(result) && out != nullptr ? *out : 0;
                        char detail[160]{};
                        std::snprintf(detail, sizeof(detail), "Control file read: id=%u nacp=%u off=%lld request=%zu actual=%zu",
                            m_object->GetId(), m_is_control_nacp ? 1u : 0u,
                            static_cast<long long>(offset), size, actual);
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlFileReadDetail, m_program_id,
                            static_cast<u8>(ncm::ContentType::Control), result, detail);
                    }
                    if (m_trace_control && !m_read_traced.exchange(true, std::memory_order_relaxed)) {
                        if (m_is_control_nacp && R_SUCCEEDED(result) && out != nullptr && *out <= size) {
                            const size_t hashed = std::min<size_t>(*out, 0x4000);
                            const u32 crc = usb_recovery::ControlDataCrc32(buffer, hashed);
                            char prefix[33]{};
                            const auto *bytes = static_cast<const u8 *>(buffer);
                            for (size_t i = 0; i < std::min<size_t>(hashed, 16); ++i)
                                std::snprintf(prefix + 2 * i, sizeof(prefix) - 2 * i, "%02x", bytes[i]);
                            char detail[192]{};
                            std::snprintf(detail, sizeof(detail), "NACP id=%u off=%lld read=%zu hashed=%zu crc32=%08x first16=%s",
                                m_object->GetId(), static_cast<long long>(offset), *out, hashed, crc, prefix);
                            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlDataChecksum, m_program_id,
                                static_cast<u8>(ncm::ContentType::Control), result, detail);
                        }
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlFileRead, m_program_id,
                            static_cast<u8>(ncm::ContentType::Control), result, "Control file: first read");
                    }
                    R_RETURN(result);
                }
                Result DoGetSize(s64 *out) override {
                    const Result result = m_file->GetSize(out);
                    if (m_trace_control) {
                        const s64 actual = R_SUCCEEDED(result) && out != nullptr ? *out : -1;
                        char detail[128]{};
                        std::snprintf(detail, sizeof(detail), "Control file size: id=%u nacp=%u size=%lld",
                            m_object->GetId(), m_is_control_nacp ? 1u : 0u, static_cast<long long>(actual));
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlFileGetSize, m_program_id,
                            static_cast<u8>(ncm::ContentType::Control), result, detail);
                    }
                    R_RETURN(result);
                }
                Result DoFlush() override { R_SUCCEED(); }
                Result DoWrite(s64 offset, const void *buffer, size_t size, const afs::WriteOption &option) override {
                    AMS_UNUSED(offset, buffer, size, option);
                    R_THROW(afs::ResultUnsupportedWriteForReadOnlyFile());
                }
                Result DoSetSize(s64 size) override {
                    AMS_UNUSED(size);
                    R_THROW(afs::ResultUnsupportedWriteForReadOnlyFile());
                }
                Result DoOperateRange(void *dst, size_t dst_size, afs::OperationId op, s64 offset, s64 size, const void *src, size_t src_size) override {
                    R_UNLESS(op == afs::OperationId::Invalidate || op == afs::OperationId::QueryRange,
                        afs::ResultUnsupportedOperateRangeForReadOnlyFile());
                    const Result result = m_file->OperateRange(dst, dst_size, op, offset, size, src, src_size);
                    if (m_trace_control) {
                        char detail[160]{};
                        std::snprintf(detail, sizeof(detail), "Control file range: id=%u nacp=%u op=%u off=%lld size=%lld dst=%zu src=%zu",
                            m_object->GetId(), m_is_control_nacp ? 1u : 0u, static_cast<u32>(op),
                            static_cast<long long>(offset), static_cast<long long>(size), dst_size, src_size);
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlFileOperateRange, m_program_id,
                            static_cast<u8>(ncm::ContentType::Control), result, detail);
                    }
                    R_RETURN(result);
                }
        };

        class DomainDirectory final : public afs::fsa::IDirectory {
            private:
                std::unique_ptr<DomainObject> m_object;
                std::unique_ptr<afs::fsa::IDirectory> m_directory;
            public:
                DomainDirectory(std::unique_ptr<DomainObject> object, std::unique_ptr<afs::fsa::IDirectory> directory)
                    : m_object(std::move(object)), m_directory(std::move(directory)) { }
                sf::cmif::DomainObjectId GetDomainObjectId() const override { return {m_object->GetId()}; }
            private:
                Result DoRead(s64 *out, afs::DirectoryEntry *entries, s64 count) override { R_RETURN(m_directory->Read(out, entries, count)); }
                Result DoGetEntryCount(s64 *out) override { R_RETURN(m_directory->GetEntryCount(out)); }
        };

        class DomainFileSystem final : public afs::fsa::IFileSystem {
            private:
                std::unique_ptr<DomainObject> m_object;
                std::shared_ptr<afs::fsa::IFileSystem> m_base;
                ncm::ProgramId m_program_id;
                bool m_trace_control;
            public:
                DomainFileSystem(std::unique_ptr<DomainObject> object, std::shared_ptr<afs::fsa::IFileSystem> base,
                    ncm::ProgramId id, bool trace_control)
                    : m_object(std::move(object)), m_base(std::move(base)), m_program_id(id), m_trace_control(trace_control) { }
            private:
                Result DoOpenFile(std::unique_ptr<afs::fsa::IFile> *out, const afs::Path &path, afs::OpenMode mode) override {
                    const Result result = [&]() -> Result {
                        R_UNLESS(mode == afs::OpenMode_Read, afs::ResultInvalidOpenMode());
                        std::unique_ptr<afs::fsa::IFile> file;
                        R_TRY(m_base->OpenFile(std::addressof(file), path, mode));
                        std::unique_ptr<DomainObject> object;
                        R_TRY(OpenDomainObject(std::addressof(object), m_object->GetBackend()));
                        auto wrapped = std::make_unique<DomainFile>(std::move(object), std::move(file), m_program_id,
                            m_trace_control, m_trace_control && std::strcmp(path.GetString(), "/control.nacp") == 0);
                        R_UNLESS(wrapped != nullptr, afs::ResultAllocationMemoryFailed());
                        *out = std::move(wrapped);
                        R_SUCCEED();
                    }();
                    if (m_trace_control) {
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlFileOpen, m_program_id,
                            static_cast<u8>(ncm::ContentType::Control), result, path.GetString());
                    }
                    R_RETURN(result);
                }
                Result DoOpenDirectory(std::unique_ptr<afs::fsa::IDirectory> *out, const afs::Path &path, afs::OpenDirectoryMode mode) override {
                    const Result result = [&]() -> Result {
                        std::unique_ptr<afs::fsa::IDirectory> directory;
                        R_TRY(m_base->OpenDirectory(std::addressof(directory), path, mode));
                        std::unique_ptr<DomainObject> object;
                        R_TRY(OpenDomainObject(std::addressof(object), m_object->GetBackend()));
                        auto wrapped = std::make_unique<DomainDirectory>(std::move(object), std::move(directory));
                        R_UNLESS(wrapped != nullptr, afs::ResultAllocationMemoryFailed());
                        *out = std::move(wrapped);
                        R_SUCCEED();
                    }();
                    if (m_trace_control) {
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlDirectoryOpen, m_program_id,
                            static_cast<u8>(ncm::ContentType::Control), result, path.GetString());
                    }
                    R_RETURN(result);
                }
                Result DoGetEntryType(afs::DirectoryEntryType *out, const afs::Path &path) override {
                    const Result result = m_base->GetEntryType(out, path);
                    if (m_trace_control) {
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlEntryType, m_program_id,
                            static_cast<u8>(ncm::ContentType::Control), result, path.GetString());
                    }
                    R_RETURN(result);
                }
                Result DoCommit() override { R_SUCCEED(); }
                Result DoCreateFile(const afs::Path &path, s64 size, int flags) override {
                    AMS_UNUSED(path, size, flags); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
                Result DoDeleteFile(const afs::Path &path) override {
                    AMS_UNUSED(path); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
                Result DoCreateDirectory(const afs::Path &path) override {
                    AMS_UNUSED(path); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
                Result DoDeleteDirectory(const afs::Path &path) override {
                    AMS_UNUSED(path); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
                Result DoDeleteDirectoryRecursively(const afs::Path &path) override {
                    AMS_UNUSED(path); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
                Result DoRenameFile(const afs::Path &a, const afs::Path &b) override {
                    AMS_UNUSED(a, b); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
                Result DoRenameDirectory(const afs::Path &a, const afs::Path &b) override {
                    AMS_UNUSED(a, b); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
                Result DoCleanDirectoryRecursively(const afs::Path &path) override {
                    AMS_UNUSED(path); R_THROW(afs::ResultUnsupportedWriteForReadOnlyFileSystem());
                }
        };

        class DomainStorage final : public afs::IStorage {
            private:
                std::unique_ptr<impl::StorageDomainObject> m_object;
                std::shared_ptr<afs::IStorage> m_base;
                ncm::ProgramId m_program_id;
                std::atomic_bool m_read_traced{false};
                usb_storage::game_wake::Title *m_trace;
                const usb_storage::LaunchPin m_launch_pin;
                const u32 m_open_epoch;
                template<class Fn>
                Result Trace(usb_storage::game_wake::Op op, u32 range, s64 offset, s64 size, Fn fn) {
                    auto ticket = usb_storage::game_wake::Begin(m_trace, op, range, offset, size,
                        os::GetSystemTick().ToTimeSpan().GetNanoSeconds());
                    const Result result = fn();
                    usb_storage::game_wake::Finish(ticket, result.GetValue(), os::GetSystemTick().ToTimeSpan().GetNanoSeconds());
                    return result;
                }
            public:
                DomainStorage(std::unique_ptr<impl::StorageDomainObject> object, std::shared_ptr<afs::IStorage> base,
                    ncm::ProgramId program_id, usb_storage::LaunchPin pin)
                    : m_object(std::move(object)), m_base(std::move(base)), m_program_id(program_id),
                      m_trace(usb_storage::game_wake::Bind(program_id.value)), m_launch_pin(std::move(pin)),
                      m_open_epoch(usb_storage::game_wake::wake_epoch.load(std::memory_order_acquire)) { }

                Result Read(s64 offset, void *buffer, size_t size) override {
                    const u32 read_epoch = usb_storage::game_wake::wake_epoch.load(std::memory_order_acquire);
                    const Result result = Trace(usb_storage::game_wake::Op::Read, 0, offset, size,
                        [&] { return m_base->Read(offset, buffer, size); });
                    if (m_launch_pin && afs::ResultIntegrityVerificationStorageCorrupted::Includes(result)) {
                        usb_storage::QueueWakeComparison(m_launch_pin, m_open_epoch, read_epoch, offset, size, result);
                    }
                    if (!m_read_traced.exchange(true, std::memory_order_relaxed)) {
                        char detail[128]{};
                        std::snprintf(detail, sizeof(detail), "R157 storage first read: id=%u off=%lld size=%zu",
                            m_object->GetId(), static_cast<long long>(offset), size);
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsUsbStorageFirstRead,
                            m_program_id, 0, result, detail);
                    }
                    R_RETURN(result);
                }
                Result Write(s64 offset, const void *buffer, size_t size) override {
                    R_RETURN(Trace(usb_storage::game_wake::Op::Write, 0, offset, size,
                        [&] { return m_base->Write(offset, buffer, size); }));
                }
                Result Flush() override { R_RETURN(Trace(usb_storage::game_wake::Op::Flush, 0, 0, 0, [&] { return m_base->Flush(); })); }
                Result SetSize(s64 size) override { R_RETURN(Trace(usb_storage::game_wake::Op::SetSize, 0, 0, size, [&] { return m_base->SetSize(size); })); }
                Result GetSize(s64 *out) override { R_RETURN(Trace(usb_storage::game_wake::Op::GetSize, 0, 0, 0, [&] { return m_base->GetSize(out); })); }
                Result OperateRange(void *dst, size_t dst_size, afs::OperationId op_id, s64 offset, s64 size,
                    const void *src, size_t src_size) override {
                    R_RETURN(Trace(usb_storage::game_wake::Op::Range, static_cast<u32>(op_id), offset, size,
                        [&] { return m_base->OperateRange(dst, dst_size, op_id, offset, size, src, src_size); }));
                }
        };

        Result WrapUsbFileSystemForMitm(std::shared_ptr<afs::fsa::IFileSystem> *out,
            sf::cmif::DomainObjectId *out_id, std::shared_ptr<afs::fsa::IFileSystem> base,
            std::shared_ptr<::Service> forward, ncm::ProgramId program_id, bool trace_control) {
            R_UNLESS(out != nullptr && out_id != nullptr && base != nullptr && forward != nullptr, afs::ResultInvalidArgument());
            *out_id = sf::cmif::InvalidDomainObjectId;
            auto backend = std::make_shared<NativeDomainBackend>(std::move(forward));
            R_UNLESS(backend != nullptr, afs::ResultAllocationMemoryFailed());
            std::unique_ptr<DomainObject> object;
            R_TRY(OpenDomainObject(std::addressof(object), backend));
            const sf::cmif::DomainObjectId object_id{object->GetId()};
            auto wrapped = std::make_shared<DomainFileSystem>(std::move(object), std::move(base), program_id, trace_control);
            R_UNLESS(wrapped != nullptr, afs::ResultAllocationMemoryFailed());
            *out = std::move(wrapped);
            *out_id = object_id;
            R_SUCCEED();
        }
    }

    Result WrapUsbControlFileSystemForMitm(std::shared_ptr<afs::fsa::IFileSystem> *out,
        sf::cmif::DomainObjectId *out_id, std::shared_ptr<afs::fsa::IFileSystem> base,
        std::shared_ptr<::Service> forward, ncm::ProgramId program_id) {
        R_RETURN(WrapUsbFileSystemForMitm(out, out_id, std::move(base), std::move(forward), program_id, true));
    }

    Result WrapUsbProgramFileSystemForMitm(std::shared_ptr<afs::fsa::IFileSystem> *out,
        sf::cmif::DomainObjectId *out_id, std::shared_ptr<afs::fsa::IFileSystem> base,
        std::shared_ptr<::Service> forward, ncm::ProgramId program_id) {
        R_RETURN(WrapUsbFileSystemForMitm(out, out_id, std::move(base), std::move(forward), program_id, false));
    }

    Result WrapUsbStorageForMitm(std::shared_ptr<afs::IStorage> *out,
        sf::cmif::DomainObjectId *out_id, std::shared_ptr<afs::IStorage> base,
        std::shared_ptr<::Service> forward, ncm::ProgramId program_id, usb_storage::LaunchPin pin) {
        R_UNLESS(out != nullptr && out_id != nullptr && base != nullptr && forward != nullptr, afs::ResultInvalidArgument());
        *out_id = sf::cmif::InvalidDomainObjectId;
        auto backend = std::make_shared<impl::StorageDomainBackend>(std::move(forward));
        R_UNLESS(backend != nullptr, afs::ResultAllocationMemoryFailed());
        auto object = std::make_unique<impl::StorageDomainObject>(backend);
        R_UNLESS(object != nullptr, afs::ResultAllocationMemoryFailed());
        const Result result = object->Initialize();
        char detail[128]{};
        std::snprintf(detail, sizeof(detail), "R157 storage reserve: cmd=1200 domain=%u id=%u",
            backend->IsDomain() ? 1u : 0u, object->GetId());
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsUsbStorageReservation,
            program_id, 0, result, detail);
        R_TRY(result);
        const sf::cmif::DomainObjectId object_id{object->GetId()};
        auto wrapped = std::make_shared<DomainStorage>(std::move(object), std::move(base), program_id, std::move(pin));
        R_UNLESS(wrapped != nullptr, afs::ResultAllocationMemoryFailed());
        *out = std::move(wrapped);
        *out_id = object_id;
        R_SUCCEED();
    }
}
