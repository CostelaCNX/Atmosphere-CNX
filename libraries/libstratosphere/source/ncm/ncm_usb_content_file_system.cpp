/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stratosphere.hpp>
#include <stratosphere/fs/fsa/fs_registrar.hpp>
#include <stratosphere/usb/usb_content_bridge.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include "../fs/fsa/fs_mount_utils.hpp"
#include "ncm_usb_content_file_system.hpp"
#include "ncm_usb_availability_policy.hpp"
#include "ncm_usb_verification_stream.hpp"

namespace ams::ncm {
    namespace {
        /* Do not forward borrowed client MapAlias pages into a second service.
         * A bounded, NCM-owned buffer also avoids the small NCM heap/stacks. */
        constexpr size_t TransferSize = 256_KB;
        alignas(0x1000) u8 g_transfer[TransferSize];
        constinit os::SdkMutex g_transfer_mutex;

        struct VerificationSlot {
            const void *owner{};
            ContentId id{};
            usb_verification::Stream<crypto::Sha256Generator> stream;
            unsigned progress{};
        };
        // Fixed storage, no background thread, no key access and no extra file
        // reads. Four simultaneous observers, at most sixteen starts per boot.
        VerificationSlot g_verification[4];
        constinit os::SdkMutex g_verification_mutex;
        unsigned g_verification_starts{};

        void SendVerificationDigestTrace(ContentId id, u32 stage, Result result, const char *detail) {
            constexpr auto name = sm::ServiceName::Encode(usb::content::ServiceName);
            bool present = false;
            if (R_FAILED(sm::HasService(std::addressof(present), name)) || !present) return;
            os::NativeHandle handle;
            if (R_FAILED(sm::GetServiceHandle(std::addressof(handle), name))) return;
            ::Service service{};
            serviceCreate(std::addressof(service), handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };
            usb::content::LoaderTraceInfo trace{};
            trace.stage = stage;
            trace.result = result.GetValue();
            trace.location_storage = trace.content_storage = static_cast<u8>(StorageId::SdCard);
            std::memcpy(trace.content_id, std::addressof(id), sizeof(id));
            util::Strlcpy(trace.path, detail, sizeof(trace.path));
            AMS_UNUSED(serviceDispatchIn(std::addressof(service), usb::content::CommandId_RecordLoaderTrace, trace));
        }

        class BridgeFile final : public fs::fsa::IFile, public fs::impl::Newable {
            private:
                std::unique_ptr<fs::fsa::IFile> m_file;
            public:
                explicit BridgeFile(std::unique_ptr<fs::fsa::IFile> file) : m_file(std::move(file)) { }
            private:
                Result DoRead(size_t *out, s64 offset, void *buffer, size_t size, const fs::ReadOption &option) override {
                    std::scoped_lock lk(g_transfer_mutex);
                    size_t done = 0;
                    while (done < size) {
                        const size_t count = std::min(size - done, TransferSize);
                        size_t read = 0;
                        R_TRY(m_file->Read(std::addressof(read), offset + done, g_transfer, count, option));
                        R_UNLESS(read <= count, fs::ResultUnexpectedInLocalFileSystemA());
                        std::memcpy(static_cast<u8 *>(buffer) + done, g_transfer, read);
                        done += read;
                        if (read < count) { break; }
                    }
                    *out = done;
                    R_SUCCEED();
                }
                Result DoWrite(s64 offset, const void *buffer, size_t size, const fs::WriteOption &option) override {
                    std::scoped_lock lk(g_transfer_mutex);
                    size_t done = 0;
                    while (done < size) {
                        const size_t count = std::min(size - done, TransferSize);
                        std::memcpy(g_transfer, static_cast<const u8 *>(buffer) + done, count);
                        R_TRY(m_file->Write(offset + done, g_transfer, count, fs::WriteOption::None));
                        done += count;
                    }
                    if (option.HasFlushFlag()) { R_TRY(m_file->Flush()); }
                    R_SUCCEED();
                }
                Result DoGetSize(s64 *out) override { R_RETURN(m_file->GetSize(out)); }
                Result DoSetSize(s64 size) override { R_RETURN(m_file->SetSize(size)); }
                Result DoFlush() override { R_RETURN(m_file->Flush()); }
                Result DoOperateRange(void *dst, size_t dn, fs::OperationId op, s64 off, s64 len, const void *src, size_t sn) override {
                    R_RETURN(m_file->OperateRange(dst, dn, op, off, len, src, sn));
                }
            public:
                sf::cmif::DomainObjectId GetDomainObjectId() const override { return m_file->GetDomainObjectId(); }
        };

        class BridgeDirectory final : public fs::fsa::IDirectory, public fs::impl::Newable {
            private:
                std::unique_ptr<fs::fsa::IDirectory> m_directory;
            public:
                explicit BridgeDirectory(std::unique_ptr<fs::fsa::IDirectory> dir) : m_directory(std::move(dir)) { }
            private:
                Result DoRead(s64 *out, fs::DirectoryEntry *entries, s64 count) override {
                    std::scoped_lock lk(g_transfer_mutex);
                    const s64 limit = std::min<s64>(count, TransferSize / sizeof(*entries));
                    s64 read = 0;
                    R_TRY(m_directory->Read(std::addressof(read), reinterpret_cast<fs::DirectoryEntry *>(g_transfer), limit));
                    R_UNLESS(read >= 0 && read <= limit, fs::ResultUnexpectedInLocalFileSystemA());
                    std::memcpy(entries, g_transfer, read * sizeof(*entries));
                    *out = read;
                    R_SUCCEED();
                }
                Result DoGetEntryCount(s64 *out) override { R_RETURN(m_directory->GetEntryCount(out)); }
            public:
                sf::cmif::DomainObjectId GetDomainObjectId() const override { return m_directory->GetDomainObjectId(); }
        };

        class SdContentMountName final : public fs::fsa::ICommonMountNameGenerator, public fs::impl::Newable {
            public:
                Result GenerateCommonMountName(char *out, size_t size) override {
                    const int count = util::SNPrintf(out, size, "%s:", fs::GetContentStorageMountName(fs::ContentStorageId::SdCard));
                    R_UNLESS(count > 0 && static_cast<size_t>(count) < size, fs::ResultTooLongPath());
                    R_SUCCEED();
                }
        };
    }

    namespace impl {
        void CancelUsbVerificationDigest(const void *owner) {
            ContentId id{};
            char detail[128]{};
            {
                std::scoped_lock lk(g_verification_mutex);
                for (auto &slot : g_verification) {
                    if (slot.owner != owner) continue;
                    id = slot.id;
                    util::SNPrintf(detail, sizeof(detail), "P5 incomplete bytes=%llu/%llu reason=cache-closed",
                        static_cast<unsigned long long>(slot.stream.Done()),
                        static_cast<unsigned long long>(slot.stream.Total()));
                    slot.stream.Cancel(); slot.owner = nullptr;
                    break;
                }
            }
            if (detail[0]) SendVerificationDigestTrace(id, 224, ResultSuccess(), detail);
        }

        void BeginUsbVerificationDigest(const void *owner, ContentId id, const char *path, fs::FileHandle file) {
            CancelUsbVerificationDigest(owner);
            if (!IsUsbContentEnabled() || !IsUsbContentLocationRegistered(id)) return;
            Path common{};
            if (R_FAILED(fs::ConvertToFsCommonPath(common.str, sizeof(common.str), path))
                || std::strncmp(common.str, "@SdCardContent:", 15) != 0) return;
            // GetSize does not read payload bytes. A diagnostic failure never
            // changes the caller's read/result. No stream exists without size.
            s64 total = 0;
            const Result size_result = fs::GetFileSize(std::addressof(total), file);
            char detail[128]{};
            u32 stage = 225;
            {
                std::scoped_lock lk(g_verification_mutex);
                if (g_verification_starts >= 16) return;
                ++g_verification_starts;
                if (R_FAILED(size_result) || total <= 0) {
                    stage = 224;
                    util::Strlcpy(detail, "P5 not sampled: file-size unavailable/empty", sizeof(detail));
                } else {
                    for (auto &slot : g_verification) {
                        if (slot.owner) continue;
                        slot.owner = owner; slot.id = id; slot.progress = 0;
                        slot.stream.Begin(static_cast<u64>(total));
                        util::SNPrintf(detail, sizeof(detail), "P5 reply-buffer SHA256 start bytes=%llu",
                            static_cast<unsigned long long>(total));
                        break;
                    }
                    if (!detail[0]) {
                        stage = 224;
                        util::Strlcpy(detail, "P5 not sampled: observer slots busy", sizeof(detail));
                    }
                }
            }
            SendVerificationDigestTrace(id, stage, size_result, detail);
        }

        void FinishUsbVerificationDigest(const void *owner, s64 offset, const void *data, size_t size, Result result) {
            ContentId id{};
            char detail[128]{};
            u32 stage = 225;
            {
                std::scoped_lock lk(g_verification_mutex);
                for (auto &slot : g_verification) {
                    if (slot.owner != owner) continue;
                    id = slot.id;
                    u8 digest[crypto::Sha256Generator::HashSize]{};
                    const auto step = slot.stream.Append(static_cast<u64>(offset), data, size,
                        R_SUCCEEDED(result), digest, sizeof(digest));
                    if (step == usb_verification::Step::Complete) {
                        char hex[65]{};
                        constexpr char digits[] = "0123456789abcdef";
                        for (size_t i = 0; i < sizeof(digest); ++i) {
                            hex[i * 2] = digits[digest[i] >> 4];
                            hex[i * 2 + 1] = digits[digest[i] & 15];
                        }
                        stage = 223;
                        util::SNPrintf(detail, sizeof(detail), "P5 full bytes=%llu id-match=%u sha256=%s",
                            static_cast<unsigned long long>(slot.stream.Done()),
                            std::memcmp(digest, std::addressof(id), sizeof(id)) == 0 ? 1u : 0u, hex);
                        slot.owner = nullptr;
                    } else if (step == usb_verification::Step::Aborted) {
                        stage = 224;
                        util::SNPrintf(detail, sizeof(detail), "P5 incomplete bytes=%llu/%llu off=%lld size=%llu reason=read-or-sequence",
                            static_cast<unsigned long long>(slot.stream.Done()),
                            static_cast<unsigned long long>(slot.stream.Total()),
                            static_cast<long long>(offset), static_cast<unsigned long long>(size));
                        slot.owner = nullptr;
                    } else if (step == usb_verification::Step::Progress) {
                        const unsigned progress = slot.stream.Done() / ((slot.stream.Total() + 3) / 4);
                        if (progress != slot.progress) {
                            slot.progress = progress;
                            util::SNPrintf(detail, sizeof(detail), "P5 reply-buffer hashed=%llu/%llu (incomplete)",
                                static_cast<unsigned long long>(slot.stream.Done()),
                                static_cast<unsigned long long>(slot.stream.Total()));
                        }
                    }
                    break;
                }
            }
            // Keep all IPC outside the observer lock. No client data escapes:
            // only counts, content ID and a full-file cryptographic digest.
            if (detail[0]) SendVerificationDigestTrace(id, stage, result, detail);
        }

        Result CheckUsbContentPathAvailable(ContentId id, const char *mounted_path, const char *common_path) {
            if (!IsUsbContentEnabled() || std::strncmp(common_path, "@SdCardContent:", 15) != 0
                || !IsUsbContentLocationRegistered(id)) R_SUCCEED();
            // Do not hand AM/NS a nominal SD path for an offline registered
            // USB NCA. The wrapped mount distinguishes an absent drive from
            // an absent file. Existing real SD content still takes precedence
            // when USB is absent, e.g. after reinstalling a title onto SD.
            bool has = false;
            R_TRY(fs::HasFile(std::addressof(has), mounted_path));
            R_UNLESS(has, ncm::ResultContentNotFound());
            R_SUCCEED();
        }

        Result ClassifyUsbContentMissing(const char *path, Result result) {
            // Do not turn genuine read/integrity errors into successful reads.
            // A real SD copy, even with a stale USB registration, always wins.
            if (!fs::ResultPathNotFound::Includes(result) || !IsUsbContentEnabled()) R_RETURN(result);
            const char *text = usb_availability::GetRegisteredId(path);
            if (!text) R_RETURN(result);
            const auto id = GetContentIdFromString(text, ContentIdStringLength);
            if (!id || *id == InvalidContentId || !IsUsbContentLocationRegistered(*id)) R_RETURN(result);

            constexpr auto name = sm::ServiceName::Encode(usb::content::ServiceName);
            bool present = false;
            R_TRY(sm::HasService(std::addressof(present), name));
            R_UNLESS(present, fs::ResultSdCardNotPresent());
            os::NativeHandle handle;
            R_TRY(sm::GetServiceHandle(std::addressof(handle), name));
            ::Service service{};
            serviceCreate(std::addressof(service), handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };
            // Snapshot only: command 5 never starts USB, mounts a disk, writes
            // a file or calls NCM back. Never use GetUsbContentStatusImpl here.
            usb::storage::Status status{};
            R_TRY(serviceDispatchOut(std::addressof(service), usb::content::CommandId_GetStorageStatus, status));
            R_UNLESS(status.running && status.state == usb::storage::State::Ready && status.media_generation != 0,
                fs::ResultSdCardNotPresent());
            // Mounted disk but no file is a real missing-file error, not an
            // unplug event. Do not conceal missing/truncated NCAs.
            R_RETURN(result);
        }

        void RecordUsbVerificationRead(ContentId id, const char *path, s64 offset, size_t size, Result result) {
            if (!IsUsbContentEnabled() || (R_SUCCEEDED(result) && offset != 0)) return;
            Path common{};
            if (R_FAILED(fs::ConvertToFsCommonPath(common.str, sizeof(common.str), path))
                || std::strncmp(common.str, "@SdCardContent:", 15) != 0
                || !IsUsbContentLocationRegistered(id)) return;
            // Bounded diagnostics: no per-chunk log writes and no key/buffer
            // contents. The existing independent publisher saves this ring.
            static std::atomic_uint successes{0}, failures{0};
            auto &counter = R_SUCCEEDED(result) ? successes : failures;
            if (counter.load(std::memory_order_relaxed) >= 64) return;
            if (counter.fetch_add(1, std::memory_order_relaxed) >= 64) return;
            constexpr auto name = sm::ServiceName::Encode(usb::content::ServiceName);
            bool present = false;
            if (R_FAILED(sm::HasService(std::addressof(present), name)) || !present) return;
            os::NativeHandle handle;
            if (R_FAILED(sm::GetServiceHandle(std::addressof(handle), name))) return;
            ::Service service{};
            serviceCreate(std::addressof(service), handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };
            usb::content::LoaderTraceInfo trace{};
            trace.stage = offset < 0 ? 222 : (R_SUCCEEDED(result) ? 220 : 221);
            trace.result = result.GetValue();
            trace.location_storage = trace.content_storage = static_cast<u8>(StorageId::SdCard);
            std::memcpy(trace.content_id, std::addressof(id), sizeof(id));
            util::SNPrintf(trace.path, sizeof(trace.path), "P4 NCM %s off=%lld bytes=%llu cache-reset=%u",
                offset < 0 ? "path" : "read", static_cast<long long>(offset), static_cast<unsigned long long>(size),
                offset >= 0 && R_FAILED(result) ? 1u : 0u);
            AMS_UNUSED(serviceDispatchIn(std::addressof(service), usb::content::CommandId_RecordLoaderTrace, trace));
        }

        Result OpenUsbContentBridge(std::unique_ptr<fs::fsa::IFileSystem> *out, bool native_only, u64 generation) {
            /* Never wait for registration. NCM must remain independent of MITM
             * during boot, and a broken/missing armed provider is an error. */
            constexpr auto name = sm::ServiceName::Encode(usb::content::ServiceName);
            bool present = false;
            R_TRY(sm::HasService(std::addressof(present), name));
            R_UNLESS(present, fs::ResultSdCardNotPresent());
            os::NativeHandle handle;
            R_TRY(sm::GetServiceHandle(std::addressof(handle), name));
            ::Service service{};
            serviceCreate(std::addressof(service), handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };
            ::FsFileSystem remote{};
            if (generation) {
                R_TRY(serviceDispatchIn(std::addressof(service), usb::content::CommandId_OpenPinnedUsbContentFileSystem, generation,
                    .out_num_objects = 1, .out_objects = std::addressof(remote.s)));
            } else {
                R_TRY(serviceDispatch(std::addressof(service), native_only ? usb::content::CommandId_OpenNativeSdContentFileSystem : usb::content::CommandId_OpenSdContentFileSystem,
                    .out_num_objects = 1, .out_objects = std::addressof(remote.s)));
            }
            auto fs = std::make_unique<fs::RemoteFileSystem>(remote);
            if (!fs) {
                fsFsClose(std::addressof(remote));
                R_THROW(fs::ResultAllocationMemoryFailed());
            }
            *out = std::move(fs);
            R_SUCCEED();
        }

        Result WrapUsbContentFile(std::unique_ptr<fs::fsa::IFile> *out) {
            auto wrapped = std::make_unique<BridgeFile>(std::move(*out));
            R_UNLESS(wrapped != nullptr, fs::ResultAllocationMemoryFailed());
            *out = std::move(wrapped);
            R_SUCCEED();
        }

        Result WrapUsbContentDirectory(std::unique_ptr<fs::fsa::IDirectory> *out) {
            auto wrapped = std::make_unique<BridgeDirectory>(std::move(*out));
            R_UNLESS(wrapped != nullptr, fs::ResultAllocationMemoryFailed());
            *out = std::move(wrapped);
            R_SUCCEED();
        }
    }

    Result MountPinnedUsbContentStorage(const char *name, u64 generation) {
        R_UNLESS(generation != 0, fs::ResultInvalidArgument());
        R_TRY(fs::impl::CheckMountNameAllowingReserved(name));
        std::unique_ptr<fs::fsa::IFileSystem> pinned;
        R_TRY(impl::OpenUsbContentBridge(&pinned, false, generation));
        // native_only means do not dynamically replace this underlying FS;
        // retain the existing owned-buffer forwarding for nested IPC.
        auto wrapper = std::make_unique<impl::UsbAwareContentFileSystem>(std::move(pinned), true);
        auto generator = std::make_unique<SdContentMountName>();
        R_UNLESS(wrapper != nullptr && generator != nullptr, fs::ResultAllocationMemoryFailed());
        R_RETURN(fs::fsa::Register(name, std::move(wrapper), std::move(generator)));
    }

    Result MountNativeSdContentStorage(const char *name) {
        R_TRY(fs::impl::CheckMountNameAllowingReserved(name));
        std::unique_ptr<fs::fsa::IFileSystem> native;
        R_TRY(impl::OpenUsbContentBridge(std::addressof(native), true));
        auto wrapper = std::make_unique<impl::UsbAwareContentFileSystem>(std::move(native), true);
        auto generator = std::make_unique<SdContentMountName>();
        R_UNLESS(wrapper != nullptr && generator != nullptr, fs::ResultAllocationMemoryFailed());
        R_RETURN(fs::fsa::Register(name, std::move(wrapper), std::move(generator)));
    }

    Result MountUsbAwareContentStorage(const char *name, fs::ContentStorageId id) {
        if (!usb::content::UseFileSystemContentRoute || id != fs::ContentStorageId::SdCard) {
            R_RETURN(fs::MountContentStorage(name, id));
        }
        R_TRY(fs::impl::CheckMountNameAllowingReserved(name));
        ::FsFileSystem native{};
        R_TRY(fsOpenContentStorageFileSystem(std::addressof(native), FsContentStorageId_SdCard));
        auto stock = std::make_unique<fs::RemoteFileSystem>(native);
        if (!stock) {
            fsFsClose(std::addressof(native));
            R_THROW(fs::ResultAllocationMemoryFailed());
        }
        auto wrapper = std::make_unique<impl::UsbAwareContentFileSystem>(std::move(stock));
        auto generator = std::make_unique<SdContentMountName>();
        R_UNLESS(wrapper != nullptr && generator != nullptr, fs::ResultAllocationMemoryFailed());
        R_RETURN(fs::fsa::Register(name, std::move(wrapper), std::move(generator)));
    }
}
