/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::usb_storage::impl {

    /* Owned exclusively by the post-boot trace writer. Do not use fsInitialize,
     * fsGetServiceSession, or the process-global mounted SD filesystem here:
     * those use ams_mitm's shared, single-slot libnx FS session pool. This
     * non-domain connection produces independent child sessions instead.
     * Native FS/SD can still block; this is not an out-of-band crash logger. */
    class TraceFileSystem {
        private:
            FsFileSystem m_sd{};

            Result Initialize() {
                R_SUCCEED_IF(serviceIsActive(std::addressof(m_sd.s)));
                os::NativeHandle handle{};
                R_TRY(sm::GetServiceHandle(std::addressof(handle), sm::ServiceName::Encode("fsp-srv")));
                Service root{};
                serviceCreate(std::addressof(root), handle);
                ON_SCOPE_EXIT { serviceClose(std::addressof(root)); };

                /* Match libnx FS initialization's caller identity, without
                 * converting this private connection into the global domain. */
                const u64 pid_placeholder = 0;
                R_TRY(serviceDispatchIn(std::addressof(root), 1, pid_placeholder, .in_send_pid = true));
                R_TRY(serviceDispatch(std::addressof(root), 18,
                    .out_num_objects = 1, .out_objects = std::addressof(m_sd.s)));
                R_SUCCEED();
            }

            Result DeleteIfPresent(const char *path) {
                const Result result = fsFsDeleteFile(std::addressof(m_sd), path);
                R_SUCCEED_IF(ams::fs::ResultPathNotFound::Includes(result));
                R_RETURN(result);
            }

        public:
            TraceFileSystem() = default;
            TraceFileSystem(const TraceFileSystem &) = delete;
            TraceFileSystem &operator=(const TraceFileSystem &) = delete;
            ~TraceFileSystem() {
                if (serviceIsActive(std::addressof(m_sd.s))) fsFsClose(std::addressof(m_sd));
            }

            Result Publish(const void *data, size_t size) {
                R_TRY(Initialize());
                constexpr const char *Directory = "/atmosphere/logs";
                constexpr const char *Temporary = "/atmosphere/logs/usb_loader_trace.txt.tmp";
                constexpr const char *Final = "/atmosphere/logs/usb_loader_trace.txt";
                const Result directory_result = fsFsCreateDirectory(std::addressof(m_sd), Directory);
                R_UNLESS(R_SUCCEEDED(directory_result) || ams::fs::ResultPathAlreadyExists::Includes(directory_result), directory_result);
                R_TRY(DeleteIfPresent(Temporary));
                R_TRY(fsFsCreateFile(std::addressof(m_sd), Temporary, static_cast<s64>(size), 0));
                {
                    FsFile file{};
                    R_TRY(fsFsOpenFile(std::addressof(m_sd), Temporary, FsOpenMode_Write, std::addressof(file)));
                    ON_SCOPE_EXIT { fsFileClose(std::addressof(file)); };
                    R_TRY(fsFileWrite(std::addressof(file), 0, data, size, FsWriteOption_Flush));
                }
                /* Replace only after a complete flushed write. As before, a
                 * failed publication must not acknowledge unsaved records. */
                R_TRY(DeleteIfPresent(Final));
                R_RETURN(fsFsRenameFile(std::addressof(m_sd), Temporary, Final));
            }
    };
}
