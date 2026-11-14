/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_content_bridge.hpp>
#include <stratosphere/usb/usb_content_proxy.hpp>
#include "ncm_usb_content_header.hpp"
#include "ncm_usb_content_file_system.hpp"

namespace ams::ncm {
    namespace {
        Result ReadHeaderFallback(usb::content::ContentHeaderInfo *out, const Path &path,
                                  fs::ContentAttributes attr, Result native, u32 query) {
            /* Native success and all native errors except missing paths win.
             * No boot-time dependency: the post-boot monitor arms this route.
             * Tickets, metadata and NAND never enter this path. */
            if (!fs::ResultPathNotFound::Includes(native) || !IsUsbContentEnabled()
                    || attr != fs::ContentAttributes_None
                    || usb::content::GetSdContentNcaSuffix(path.str) == nullptr) {
                R_RETURN(native);
            }
            constexpr auto name = sm::ServiceName::Encode(usb::content::ServiceName);
            bool present = false;
            R_TRY(sm::HasService(std::addressof(present), name));
            R_UNLESS(present, fs::ResultSdCardNotPresent());
            os::NativeHandle handle;
            R_TRY(sm::GetServiceHandle(std::addressof(handle), name));
            ::Service service{};
            serviceCreate(std::addressof(service), handle);
            ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };

            /* Path is NCM-owned, not borrowed client MapAlias memory. */
            const Result result = serviceDispatchInOut(std::addressof(service), usb::content::CommandId_GetSdContentHeader,
                query, *out,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                .buffers = { { path.str, std::strlen(path.str) + 1 } });
            R_RETURN(impl::ClassifyUsbContentMissing(path.str, result));
        }
    }

    Result GetUsbAwareRightsId(RightsId *out, const Path &path, fs::ContentAttributes attr) {
        RightsId rights{};
        const Result native = hos::GetVersion() >= hos::Version_3_0_0
            ? fs::GetRightsId(std::addressof(rights.id), std::addressof(rights.key_generation), path.str, attr)
            : fs::GetRightsId(std::addressof(rights.id), path.str, attr);
        if (R_SUCCEEDED(native)) { *out = rights; R_SUCCEED(); }
        usb::content::ContentHeaderInfo header{};
        R_TRY(ReadHeaderFallback(std::addressof(header), path, attr, native, usb::content::HeaderQuery_Rights));
        std::memcpy(rights.id.data, header.rights_id, sizeof(rights.id.data));
        rights.key_generation = header.key_generation;
        *out = rights;
        R_SUCCEED();
    }

    Result GetUsbAwareProgramId(ProgramId *out, const Path &path, fs::ContentAttributes attr) {
        ProgramId program_id{};
        const Result native = fs::GetProgramId(std::addressof(program_id), path.str, attr);
        if (R_SUCCEEDED(native)) { *out = program_id; R_SUCCEED(); }
        usb::content::ContentHeaderInfo header{};
        R_TRY(ReadHeaderFallback(std::addressof(header), path, attr, native, usb::content::HeaderQuery_Program));
        *out = ProgramId{header.program_id};
        R_SUCCEED();
    }
}
