/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "ncm_content_storage_impl.hpp"
#include "ncm_usb_content_file_system.hpp"
#include <stratosphere/ncm/ncm_usb_title_map.hpp>

namespace ams::ncm {
    // Base ordering is deliberate: close every cached file/directory in
    // ContentStorageImpl (and PlaceHolderAccessor) BEFORE unmounting its FS.
    class PinnedUsbMountOwner {
        protected:
            char m_mount_name[16]{};
            bool m_mounted{false};
            ~PinnedUsbMountOwner() { if (m_mounted) fs::Unmount(m_mount_name); }
    };
    class PinnedUsbContentStorage : private PinnedUsbMountOwner, public ContentStorageImpl {
        public:
            Result InitializePinned(u64 generation, RightsIdCache *cache) {
                // OpenPinnedUsbContentStorage holds the manager mutex.
                static u64 next_id = 0;
                R_UNLESS(next_id < 0xffffffff, fs::ResultAllocationMemoryFailed());
                util::SNPrintf(m_mount_name, sizeof(m_mount_name), "usbi%08x", static_cast<u32>(++next_id));
                R_TRY(MountPinnedUsbContentStorage(m_mount_name, generation));
                m_mounted = true;
                char root[20]{};
                util::SNPrintf(root, sizeof(root), "%s:/", m_mount_name);
                R_TRY(InitializeBase(root));
                R_RETURN(Initialize(root, MakeSha256HierarchicalContentFilePath_ForFat16KCluster,
                    MakeSha256HierarchicalPlaceHolderFilePath_ForFat16KCluster, true, cache));
            }
            Result Register(PlaceHolderId placeholder, ContentId content) override {
                R_TRY(this->FlushPlaceHolder());
                s64 expected_size = 0;
                R_TRY(this->GetSizeFromPlaceHolderId(std::addressof(expected_size), placeholder));
                R_TRY(ContentStorageImpl::Register(placeholder, content));
                // P2: a rename alone is not registration. Publish the SD index
                // only after the pinned USB file is flushed and reopened.
                // Keep the final file on any failure; never report false success.
                R_RETURN(this->EnsureRegistered(content, expected_size));
            }
            Result EnsureRegistered(ContentId content, s64 expected_size) {
                R_TRY(this->EnsureEnabled());
                R_UNLESS(content != InvalidContentId && expected_size > 0, fs::ResultInvalidArgument());
                R_TRY(fs::CommitSaveData(m_mount_name));
                // Keep the private mount name: GetPath converts to a common
                // routed path, which would no longer prove this pinned disk.
                PathString registered, path;
                registered.AssignFormat("%s/registered", m_root_path.Get());
                m_make_content_path_func(std::addressof(path), content, registered.Get());
                fs::FileHandle file;
                R_TRY(fs::OpenFile(std::addressof(file), path.Get(), fs::OpenMode_Read));
                ON_SCOPE_EXIT { fs::CloseFile(file); };
                s64 size = 0;
                R_TRY(fs::GetFileSize(std::addressof(size), file));
                R_UNLESS(size == expected_size, ncm::ResultInvalidOperation());
                // This API records location, not cryptographic validity. A
                // reusing installer must verify the full existing bytes first.
                R_TRY(RegisterUsbContentLocation(content));
                // The file remains pinned: a detach during SD publication must
                // reach the caller instead of completing the install on a new disk.
                R_RETURN(fs::CommitSaveData(m_mount_name));
            }
            Result Delete(ContentId content) override {
                R_TRY(ContentStorageImpl::Delete(content));
                R_TRY(fs::CommitSaveData(m_mount_name));
                R_RETURN(UnregisterUsbContentLocation(content));
            }
            Result FlushPlaceHolder() override {
                R_TRY(ContentStorageImpl::FlushPlaceHolder());
                R_RETURN(fs::CommitSaveData(m_mount_name));
            }
            Result CleanupAllPlaceHolder() override {
                // A per-install handle must not purge another install's work.
                // Clients delete only the placeholder IDs they own.
                R_THROW(fs::ResultNotImplemented());
            }
    };
    static_assert(IsIContentStorage<PinnedUsbContentStorage>);
}
