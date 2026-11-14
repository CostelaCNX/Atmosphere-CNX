/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>

namespace ams::ncm {
    Result MountUsbAwareContentStorage(const char *name, fs::ContentStorageId id);
    Result MountNativeSdContentStorage(const char *name);
    Result MountPinnedUsbContentStorage(const char *name, u64 generation);

    namespace impl {
        Result OpenUsbContentBridge(std::unique_ptr<fs::fsa::IFileSystem> *out, bool native_only = false, u64 generation = 0);
        Result WrapUsbContentFile(std::unique_ptr<fs::fsa::IFile> *out);
        Result WrapUsbContentDirectory(std::unique_ptr<fs::fsa::IDirectory> *out);
        Result ClassifyUsbContentMissing(const char *path, Result result);
        Result CheckUsbContentPathAvailable(ContentId id, const char *mounted_path, const char *common_path);
        void RecordUsbVerificationRead(ContentId id, const char *path, s64 offset, size_t size, Result result);
        void BeginUsbVerificationDigest(const void *owner, ContentId id, const char *path, fs::FileHandle file);
        void FinishUsbVerificationDigest(const void *owner, s64 offset, const void *data, size_t size, Result result);
        void CancelUsbVerificationDigest(const void *owner);

        /* Mounted at normal NCM startup, but does NOT contact ams_mitm there.
         * The monitor arms IsUsbContentEnabled only after Horizon initialization.
         * Keep native NAND/metadata mounts out of this class entirely. */
        class UsbAwareContentFileSystem final : public fs::fsa::IFileSystem, public fs::impl::Newable {
            private:
                os::SdkMutex m_mutex{};
                std::unique_ptr<fs::fsa::IFileSystem> m_stock;
                std::unique_ptr<fs::fsa::IFileSystem> m_bridge;
                const bool m_native_only;

                Result Select(fs::fsa::IFileSystem **out) {
                    *out = m_stock.get();
                    if (m_native_only) { R_SUCCEED(); }
                    if (!IsUsbContentEnabled()) {
                        R_SUCCEED();
                    }
                    if (!m_bridge) {
                        /* Once armed, errors must propagate: never silently install
                         * to SD because a USB IPC connection failed. The server
                         * itself chooses normal SD when no USB medium is mounted. */
                        R_TRY(OpenUsbContentBridge(std::addressof(m_bridge)));
                    }
                    *out = m_bridge.get();
                    R_SUCCEED();
                }

            public:
                explicit UsbAwareContentFileSystem(std::unique_ptr<fs::fsa::IFileSystem> stock, bool native_only = false)
                    : m_stock(std::move(stock)), m_native_only(native_only) { }

            private:
                #define NCM_BRIDGE_FORWARD(name, signature, arguments) \
                    Result Do##name signature override { \
                        std::scoped_lock lk(m_mutex); \
                        fs::fsa::IFileSystem *target = nullptr; \
                        R_TRY(this->Select(std::addressof(target))); \
                        R_RETURN(target->name arguments); \
                    }

                NCM_BRIDGE_FORWARD(CreateFile, (const fs::Path &p, s64 size, int flags), (p, size, flags))
                NCM_BRIDGE_FORWARD(DeleteFile, (const fs::Path &p), (p))
                NCM_BRIDGE_FORWARD(CreateDirectory, (const fs::Path &p), (p))
                NCM_BRIDGE_FORWARD(DeleteDirectory, (const fs::Path &p), (p))
                NCM_BRIDGE_FORWARD(DeleteDirectoryRecursively, (const fs::Path &p), (p))
                NCM_BRIDGE_FORWARD(RenameFile, (const fs::Path &a, const fs::Path &b), (a, b))
                NCM_BRIDGE_FORWARD(RenameDirectory, (const fs::Path &a, const fs::Path &b), (a, b))
                NCM_BRIDGE_FORWARD(Commit, (), ())
                NCM_BRIDGE_FORWARD(GetFreeSpaceSize, (s64 *out, const fs::Path &p), (out, p))
                NCM_BRIDGE_FORWARD(GetTotalSpaceSize, (s64 *out, const fs::Path &p), (out, p))
                NCM_BRIDGE_FORWARD(CleanDirectoryRecursively, (const fs::Path &p), (p))
                NCM_BRIDGE_FORWARD(GetFileTimeStampRaw, (fs::FileTimeStampRaw *out, const fs::Path &p), (out, p))
                NCM_BRIDGE_FORWARD(QueryEntry, (char *dst, size_t dn, const char *src, size_t sn, fs::fsa::QueryId q, const fs::Path &p), (dst, dn, src, sn, q, p))
                NCM_BRIDGE_FORWARD(CommitProvisionally, (s64 counter), (counter))
                NCM_BRIDGE_FORWARD(Rollback, (), ())
                NCM_BRIDGE_FORWARD(Flush, (), ())
                #undef NCM_BRIDGE_FORWARD

                Result DoGetEntryType(fs::DirectoryEntryType *out, const fs::Path &p) override {
                    std::scoped_lock lk(m_mutex);
                    fs::fsa::IFileSystem *target = nullptr;
                    R_TRY(this->Select(std::addressof(target)));
                    const Result result = target->GetEntryType(out, p);
                    R_RETURN(m_native_only ? result : ClassifyUsbContentMissing(p.GetString(), result));
                }

                Result DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out, const fs::Path &p, fs::OpenMode mode) override {
                    std::scoped_lock lk(m_mutex);
                    fs::fsa::IFileSystem *target = nullptr;
                    R_TRY(this->Select(std::addressof(target)));
                    const Result result = target->OpenFile(out, p, mode);
                    R_TRY(m_native_only ? result : ClassifyUsbContentMissing(p.GetString(), result));
                    if (m_native_only || target == m_bridge.get()) {
                        R_TRY(WrapUsbContentFile(out));
                    }
                    R_SUCCEED();
                }

                Result DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *out, const fs::Path &p, fs::OpenDirectoryMode mode) override {
                    std::scoped_lock lk(m_mutex);
                    fs::fsa::IFileSystem *target = nullptr;
                    R_TRY(this->Select(std::addressof(target)));
                    R_TRY(target->OpenDirectory(out, p, mode));
                    if (m_native_only || target == m_bridge.get()) {
                        R_TRY(WrapUsbContentDirectory(out));
                    }
                    R_SUCCEED();
                }
        };
    }
}
