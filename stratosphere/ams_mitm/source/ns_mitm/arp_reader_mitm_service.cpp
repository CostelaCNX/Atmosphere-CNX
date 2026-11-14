/*
 * Copyright (c) Atmosphere-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>
#include "../usb_mitm/usb_launch_trace.hpp"
#include "arp_reader_mitm_service.hpp"

namespace ams::mitm::arp {

    namespace {

        constexpr const char *ControlPropertyPath = "/control.nacp";

        Result GetApplicationLaunchPropertyForward(Service *forward_service, ApplicationLaunchProperty *out, u64 instance_id) {
            R_RETURN(serviceDispatchInOut(forward_service, 0, instance_id, *out));
        }

        Result GetApplicationControlPropertyForward(Service *forward_service, ApplicationControlProperty *out, u64 instance_id) {
            R_RETURN(serviceDispatchIn(forward_service, 1, instance_id,
                .buffer_attrs = {
                    SfBufferAttr_HipcMapAlias | SfBufferAttr_Out | SfBufferAttr_FixedSize,
                },
                .buffers = {
                    { out, sizeof(*out) },
                },
            ));
        }

        Result ReadUsbApplicationControlPropertyImpl(ApplicationControlProperty *out, ncm::ProgramId application_id) {
            R_UNLESS(out != nullptr, fs::ResultNullptrArgument());
            R_UNLESS(usb_storage::IsUsbContentLaunchPathEnabled(), usb::ResultOperationDenied());
            R_UNLESS(ncm::IsApplicationId(application_id), fs::ResultInvalidArgument());
            R_UNLESS(usb_storage::HasUsbProgramPath(application_id), fs::ResultPathNotFound());

            char content_path[fs::EntryNameLengthMax + 1]{};
            R_TRY(usb_storage::GetUsbApplicationContentPath(content_path, sizeof(content_path), application_id, ncm::ContentType::Control));

            std::shared_ptr<fs::fsa::IFileSystem> control_file_system;
            ncm::ProgramId resolved_application_id{};
            R_TRY(usb_storage::OpenUsbControlFileSystem(std::addressof(control_file_system), std::addressof(resolved_application_id), content_path, application_id));
            R_UNLESS(resolved_application_id == application_id, fs::ResultInvalidArgument());

            fs::Path control_path;
            R_TRY(control_path.InitializeWithNormalization(ControlPropertyPath));

            std::unique_ptr<fs::fsa::IFile> control_file;
            R_TRY(control_file_system->OpenFile(std::addressof(control_file), control_path, fs::OpenMode_Read));

            s64 control_size = 0;
            R_TRY(control_file->GetSize(std::addressof(control_size)));
            R_UNLESS(control_size == static_cast<s64>(sizeof(*out)), fs::ResultInvalidSize());

            size_t read_size = 0;
            R_TRY(control_file->Read(std::addressof(read_size), 0, out, sizeof(*out)));
            R_UNLESS(read_size == sizeof(*out), fs::ResultInvalidSize());
            R_SUCCEED();
        }

    }

    Result ReadUsbApplicationControlProperty(ApplicationControlProperty *out, ncm::ProgramId application_id) {
        R_RETURN(ReadUsbApplicationControlPropertyImpl(out, application_id));
    }

    Result ArpReaderMitmService::GetApplicationControlProperty(sf::Out<ApplicationControlProperty> out, u64 instance_id) {
        const Result stock_result = GetApplicationControlPropertyForward(m_forward_service.get(), out.GetPointer(), instance_id);
        if (R_SUCCEEDED(stock_result) || !fs::ResultPathNotFound::Includes(stock_result)) {
            R_RETURN(stock_result);
        }

        /* Since 10.0.0 command 1 is keyed by ApplicationInstanceId rather
         * than ProgramId. Resolve the instance through stock command 0; its
         * launch property remains authoritative for identity and version. */
        ApplicationLaunchProperty launch_property{};
        const Result launch_result = GetApplicationLaunchPropertyForward(m_forward_service.get(), std::addressof(launch_property), instance_id);

        char details[0x80]{};
        util::SNPrintf(details, sizeof(details), "arp:r c=1 iid=%016llX stock=%08X",
            static_cast<unsigned long long>(instance_id), stock_result.GetValue());
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpControlPropertyStock,
            R_SUCCEEDED(launch_result) ? launch_property.application_id : ncm::InvalidProgramId,
            static_cast<u8>(ncm::ContentType::Control), stock_result, details);

        util::SNPrintf(details, sizeof(details), "arp:r c=0 iid=%016llX id=%016llX v=%u s=%u p=%u k=%u",
            static_cast<unsigned long long>(instance_id),
            static_cast<unsigned long long>(launch_property.application_id.value), launch_property.version,
            launch_property.storage_id, launch_property.patch_storage_id, launch_property.application_kind);
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpLaunchPropertyResult,
            launch_property.application_id, 0, launch_result, details);

        R_UNLESS(R_SUCCEEDED(launch_result), stock_result);
        R_UNLESS(usb_storage::IsUsbContentLaunchPathEnabled(), stock_result);
        R_UNLESS(ncm::IsApplicationId(launch_property.application_id), stock_result);
        R_UNLESS(usb_storage::HasUsbProgramPath(launch_property.application_id), stock_result);

        const Result usb_result = ReadUsbApplicationControlProperty(out.GetPointer(), launch_property.application_id);
        util::SNPrintf(details, sizeof(details), "arp:r c=1 USB NACP iid=%016llX bytes=%u",
            static_cast<unsigned long long>(instance_id), static_cast<u32>(sizeof(ApplicationControlProperty)));
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpControlPropertyUsb,
            launch_property.application_id, static_cast<u8>(ncm::ContentType::Control), usb_result, details);
        R_RETURN(usb_result);
    }

}
