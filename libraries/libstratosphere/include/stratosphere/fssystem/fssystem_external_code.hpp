/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once
#include <vapours.hpp>
#include <stratosphere/ncm/ncm_ids.hpp>
#include <stratosphere/fs/fsa/fs_ifilesystem.hpp>
#include <stratosphere/fs/fs_remote_filesystem.hpp>

namespace ams::fssystem {

    fs::fsa::IFileSystem *GetExternalCodeFileSystem(ncm::ProgramId program_id);

    #if defined(ATMOSPHERE_OS_HORIZON)
    /* Register an already-open remote filesystem as Loader external code.
     * This is the in-process counterpart to ldr:shel command 65000 and lets
     * Loader retain an ams:usb filesystem object without a callback IPC
     * cycle through itself during process creation. Ownership of the remote
     * service transfers on success. */
    Result RegisterExternalCodeFileSystem(ncm::ProgramId program_id, const ::FsFileSystem &file_system);
    #endif

    Result CreateExternalCode(os::NativeHandle *out, ncm::ProgramId program_id);
    void   DestroyExternalCode(ncm::ProgramId program_id);

}
