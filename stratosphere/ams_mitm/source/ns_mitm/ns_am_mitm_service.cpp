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
#include <stratosphere.hpp>
#include "../amsmitm_fs_utils.hpp"
#include "../usb_mitm/usb_launch_trace.hpp"
#include "ns_am_mitm_service.hpp"
#include "ns_shim.h"

namespace ams::mitm::ns {

    namespace {

        constexpr size_t ApplicationViewDeprecatedSize = 0x40;
        constexpr size_t ApplicationViewSize = 0x50;
        constexpr size_t ApplicationViewWithPromotionInfoSize = 0x70;
        constexpr size_t ApplicationViewStrideGrowthMax = 0x20;

        template<typename T>
        T ReadViewField(const void *view, size_t view_size, size_t offset) {
            T value{};
            if (view != nullptr && offset <= view_size && sizeof(value) <= view_size - offset) {
                std::memcpy(std::addressof(value), static_cast<const u8 *>(view) + offset, sizeof(value));
            }
            return value;
        }

        ncm::ProgramId ReadApplicationId(const sf::InBuffer &application_ids, size_t index) {
            ncm::ProgramId application_id{};
            const size_t offset = index * sizeof(application_id.value);
            if (application_ids.GetPointer() != nullptr && offset <= application_ids.GetSize() && sizeof(application_id.value) <= application_ids.GetSize() - offset) {
                std::memcpy(std::addressof(application_id.value), static_cast<const u8 *>(application_ids.GetPointer()) + offset, sizeof(application_id.value));
            }
            return application_id;
        }

        size_t DeriveApplicationViewStride(size_t output_size, size_t input_count, size_t nominal_view_size) {
            if (input_count == 0 || output_size % input_count != 0) {
                return nominal_view_size;
            }

            const size_t candidate = output_size / input_count;
            if (candidate < nominal_view_size || candidate > nominal_view_size + ApplicationViewStrideGrowthMax || candidate % alignof(u64) != 0) {
                return nominal_view_size;
            }

            return candidate;
        }

        ncm::ProgramId ReadReturnedApplicationId(const sf::OutBuffer &out_views, size_t view_size, size_t index) {
            ncm::ProgramId application_id{};
            if (out_views.GetPointer() != nullptr && view_size != 0 && index < out_views.GetSize() / view_size) {
                const void *view = static_cast<const u8 *>(out_views.GetPointer()) + (index * view_size);
                application_id.value = ReadViewField<u64>(view, view_size, 0x00);
            }
            return application_id;
        }

        void RecordApplicationViewResult(u32 command_id, const sf::OutBuffer &out_views, size_t view_size, size_t index, ncm::ProgramId application_id, bool is_usb, Result result) {
            char details[0x80]{};
            if (R_SUCCEEDED(result) && out_views.GetPointer() != nullptr && view_size != 0 && index < out_views.GetSize() / view_size && view_size <= out_views.GetSize() - (index * view_size)) {
                const void *view = static_cast<const u8 *>(out_views.GetPointer()) + (index * view_size);
                const u64 out_application_id = ReadViewField<u64>(view, view_size, 0x00);
                const u32 unk_x8 = ReadViewField<u32>(view, view_size, 0x08);
                const u32 flags = ReadViewField<u32>(view, view_size, 0x0C);
                const u32 unk_x20 = ReadViewField<u32>(view, view_size, 0x20);
                const u16 unk_x24 = ReadViewField<u16>(view, view_size, 0x24);
                const u32 unk_x40 = ReadViewField<u32>(view, view_size, 0x40);
                const u8 unk_x44 = ReadViewField<u8>(view, view_size, 0x44);
                util::SNPrintf(details, sizeof(details),
                    "view c=%u u=%u i=%u s=%X id=%016llX f=%08X x8=%08X x20=%08X x24=%04X x40=%08X x44=%02X",
                    command_id, is_usb ? 1u : 0u, static_cast<u32>(index), static_cast<u32>(view_size),
                    static_cast<unsigned long long>(out_application_id), flags, unk_x8, unk_x20, unk_x24, unk_x40, unk_x44);

                usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationViewResult, application_id, 0, result, details);

                const u64 tail_x48 = ReadViewField<u64>(view, view_size, 0x48);
                const u64 tail_x50 = ReadViewField<u64>(view, view_size, 0x50);
                const u64 tail_x58 = ReadViewField<u64>(view, view_size, 0x58);
                const u64 tail_x60 = ReadViewField<u64>(view, view_size, 0x60);
                const u64 tail_x68 = ReadViewField<u64>(view, view_size, 0x68);
                const u64 tail_x70 = ReadViewField<u64>(view, view_size, 0x70);
                util::SNPrintf(details, sizeof(details),
                    "tail c=%u u=%u i=%u %016llX%016llX%016llX%016llX%016llX%016llX",
                    command_id, is_usb ? 1u : 0u, static_cast<u32>(index),
                    static_cast<unsigned long long>(tail_x48), static_cast<unsigned long long>(tail_x50),
                    static_cast<unsigned long long>(tail_x58), static_cast<unsigned long long>(tail_x60),
                    static_cast<unsigned long long>(tail_x68), static_cast<unsigned long long>(tail_x70));
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationViewTail, application_id, 0, result, details);
                return;
            } else {
                util::SNPrintf(details, sizeof(details), "view c=%u u=%u i=%u s=%X forward failed", command_id, is_usb ? 1u : 0u, static_cast<u32>(index), static_cast<u32>(view_size));
            }
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationViewResult, application_id, 0, result, details);
        }

        Result GetApplicationViewImpl(Service *forward_service, const sf::OutBuffer &out_views, const sf::InBuffer &application_ids, u32 command_id, size_t nominal_view_size) {
            const size_t input_count = application_ids.GetSize() / sizeof(u64);
            const size_t view_size = DeriveApplicationViewStride(out_views.GetSize(), input_count, nominal_view_size);
            const size_t output_count = view_size == 0 ? 0 : out_views.GetSize() / view_size;
            const size_t count = std::min(input_count, output_count);

            ncm::ProgramId first_id{};
            ncm::ProgramId usb_application_id{};
            size_t usb_input_index = count;
            for (size_t i = 0; i < count; ++i) {
                const ncm::ProgramId application_id = ReadApplicationId(application_ids, i);
                if (i == 0) {
                    first_id = application_id;
                }
                if (usb_storage::IsUsbContentLaunchPathEnabled() && usb_storage::HasUsbProgramPath(application_id)) {
                    if (usb_input_index == count) {
                        usb_input_index = i;
                        usb_application_id = application_id;
                    }
                }
            }

            const ncm::ProgramId trace_id = usb_input_index < count ? usb_application_id : first_id;
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details), "view c=%u in=%u bytes=%u stride=%u usb_in=%d",
                command_id, static_cast<u32>(input_count), static_cast<u32>(out_views.GetSize()), static_cast<u32>(view_size),
                usb_input_index < count ? static_cast<int>(usb_input_index) : -1);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationViewAttempt, trace_id, 0, ResultSuccess(), details);

            const Result result = nsamGetApplicationViewFwd(forward_service, command_id,
                out_views.GetPointer(), out_views.GetSize(), application_ids.GetPointer(), application_ids.GetSize());

            if (usb_input_index < count) {
                size_t usb_output_index = count;
                size_t reference_index = count;
                ncm::ProgramId reference_application_id{};
                if (R_SUCCEEDED(result)) {
                    for (size_t i = 0; i < count; ++i) {
                        const ncm::ProgramId returned_application_id = ReadReturnedApplicationId(out_views, view_size, i);
                        const bool returned_is_application = ncm::IsApplicationId(returned_application_id);
                        const bool returned_is_usb = returned_is_application && usb_storage::HasUsbProgramPath(returned_application_id);
                        if (usb_output_index == count && returned_application_id == usb_application_id) {
                            usb_output_index = i;
                        }
                        if (!returned_is_usb && reference_index == count && returned_is_application && returned_application_id != usb_application_id) {
                            reference_index = i;
                            reference_application_id = returned_application_id;
                        }
                    }
                }

                if (usb_output_index == count) {
                    usb_output_index = usb_input_index;
                }
                /* R100's private Host-proxy lifetime fix makes Horizon's
                 * native USB-backed view match the controlled same-title SD
                 * view (0x00400F13). Preserve that native record byte-for-byte.
                 * The older 0x08000000 compatibility mutation belonged to the
                 * incomplete pre-R99 route and now changes a known-good value. */
                RecordApplicationViewResult(command_id, out_views, view_size, usb_output_index, usb_application_id, true, result);

                if (reference_index < count) {
                    RecordApplicationViewResult(command_id, out_views, view_size, reference_index, reference_application_id, false, result);
                } else {
                    util::SNPrintf(details, sizeof(details), "view c=%u stride=%u stock_reference=none", command_id, static_cast<u32>(view_size));
                    usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationViewResult, usb_application_id, 0, result, details);
                }
            } else if (count > 0) {
                const ncm::ProgramId returned_application_id = R_SUCCEEDED(result) ? ReadReturnedApplicationId(out_views, view_size, 0) : first_id;
                RecordApplicationViewResult(command_id, out_views, view_size, 0, returned_application_id, false, result);
            } else {
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationViewResult, trace_id, 0, result, "view returned no complete records");
            }

            R_RETURN(result);
        }

        Result GetApplicationContentPathImpl(Service *forward_service, const sf::OutBuffer &out_path, ncm::ProgramId application_id, u8 content_type) {
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsGetContentPathAttempt, application_id, content_type);
            char usb_path[ams::fs::EntryNameLengthMax + 1]{};
            if (usb_storage::IsUsbContentLaunchPathEnabled() && R_SUCCEEDED(usb_storage::GetUsbApplicationContentPath(usb_path, sizeof(usb_path), application_id, static_cast<ncm::ContentType>(content_type)))) {
                const size_t path_length = std::strlen(usb_path);
                R_UNLESS(path_length + 1 <= out_path.GetSize(), ncm::ResultBufferInsufficient());
                std::memcpy(out_path.GetPointer(), usb_path, path_length + 1);
                usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsGetContentPathResult, application_id, content_type, ResultSuccess(), usb_path);
                R_SUCCEED();
            }
            const Result result = nsamGetApplicationContentPathFwd(forward_service, out_path.GetPointer(), out_path.GetSize(), static_cast<u64>(application_id), static_cast<NcmContentType>(content_type));
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsGetContentPathResult, application_id, content_type, result, R_SUCCEEDED(result) ? reinterpret_cast<const char *>(out_path.GetPointer()) : nullptr);
            R_RETURN(result);
        }

        Result ResolveApplicationContentPathImpl(Service *forward_service, ncm::ProgramId application_id, u8 content_type) {
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsResolveContentPathAttempt, application_id, content_type);
            /* Always succeed for web applets asking about HBL to enable hbl_html, and applications with manual_html to allow custom manual data. */
            bool is_hbl = false;
            if ((R_SUCCEEDED(ams::pm::info::IsHblProgramId(std::addressof(is_hbl), application_id)) && is_hbl) || (static_cast<ncm::ContentType>(content_type) == ncm::ContentType::HtmlDocument && mitm::fs::HasSdManualHtmlContent(application_id))) {
                const Result result = nsamResolveApplicationContentPathFwd(forward_service, static_cast<u64>(application_id), static_cast<NcmContentType>(content_type));
                usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsResolveContentPathResult, application_id, content_type, result);
                R_SUCCEED();
            }
            const Result result = nsamResolveApplicationContentPathFwd(forward_service, static_cast<u64>(application_id), static_cast<NcmContentType>(content_type));
            char usb_path[ams::fs::EntryNameLengthMax + 1]{};
            if (usb_storage::IsUsbContentLaunchPathEnabled() && R_SUCCEEDED(usb_storage::GetUsbApplicationContentPath(usb_path, sizeof(usb_path), application_id, static_cast<ncm::ContentType>(content_type)))) {
                usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsResolveContentPathResult, application_id, content_type, ResultSuccess(), usb_path);
                R_SUCCEED();
            }
            usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsResolveContentPathResult, application_id, content_type, result);
            R_RETURN(result);
        }

        Result GetRunningApplicationProgramIdImpl(Service *forward_service, sf::Out<ncm::ProgramId> out, ncm::ProgramId application_id) {
            R_RETURN(nsamGetRunningApplicationProgramIdFwd(forward_service, reinterpret_cast<u64 *>(out.GetPointer()), static_cast<u64>(application_id)));
        }

        Result CheckApplicationLaunchVersionImpl(Service *forward_service, ncm::ProgramId application_id) {
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsLaunchVersionAttempt, application_id, 0, ResultSuccess(), "ns:am command=38");
            const Result result = nsamCheckApplicationLaunchVersionFwd(forward_service, application_id.value);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsLaunchVersionResult, application_id, 0, result, "ns:am command=38 result");
            R_RETURN(result);
        }

    }

    Result NsAmMitmService::GetApplicationViewDeprecated(const sf::OutBuffer &out_views, const sf::InBuffer &application_ids) {
        R_RETURN(GetApplicationViewImpl(m_forward_service.get(), out_views, application_ids, 3, ApplicationViewDeprecatedSize));
    }

    Result NsAmMitmService::GetApplicationContentPath(const sf::OutBuffer &out_path, ncm::ProgramId application_id, u8 content_type) {
        R_RETURN(GetApplicationContentPathImpl(m_forward_service.get(), out_path, application_id, content_type));
    }

    Result NsAmMitmService::ResolveApplicationContentPath(ncm::ProgramId application_id, u8 content_type) {
        R_RETURN(ResolveApplicationContentPathImpl(m_forward_service.get(), application_id, content_type));
    }

    Result NsAmMitmService::CheckApplicationLaunchVersion(ncm::ProgramId application_id) {
        R_RETURN(CheckApplicationLaunchVersionImpl(m_forward_service.get(), application_id));
    }

    Result NsAmMitmService::GetRunningApplicationProgramId(sf::Out<ncm::ProgramId> out, ncm::ProgramId application_id) {
        R_RETURN(GetRunningApplicationProgramIdImpl(m_forward_service.get(), out, application_id));
    }

    Result NsAmMitmService::GetApplicationView(const sf::OutBuffer &out_views, const sf::InBuffer &application_ids) {
        R_RETURN(GetApplicationViewImpl(m_forward_service.get(), out_views, application_ids, 1701, ApplicationViewSize));
    }

    Result NsAmMitmService::GetApplicationViewWithPromotionInfo(const sf::OutBuffer &out_views, const sf::InBuffer &application_ids) {
        R_RETURN(GetApplicationViewImpl(m_forward_service.get(), out_views, application_ids, 1704, ApplicationViewWithPromotionInfoSize));
    }

    Result NsAm2MitmService::GetReadOnlyApplicationControlDataInterface(sf::Out<sf::SharedPointer<impl::IControlDataTraceMitmInterface>> out) {
        NsReadOnlyApplicationControlDataInterface control_data{};
        const Result result = nsGetReadOnlyApplicationControlDataInterfaceFwd(m_forward_service.get(), std::addressof(control_data));
        usb_storage::RecordLaunchBoundaryTrace(usb::content::LoaderProbeStage::NsControlDataObjectPublish,
            m_client_info.program_id, 0, result, "ns:am2 command=7989 read-only control data");
        R_TRY(result);

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(control_data.s))};
        auto forward_service = sf::hipc::ServerSession::CreateForwardService();
        *forward_service = control_data.s;
        control_data.s = {};

        auto object_forward_service = forward_service;
        auto wrapped_object = sf::CreateSharedObjectEmplaced<impl::IControlDataTraceMitmInterface, NsControlDataTraceMitmService>(std::move(object_forward_service), m_client_info);
        if (target_object_id == sf::cmif::InvalidDomainObjectId) {
            out.SetMitmValue(std::move(wrapped_object), std::move(forward_service));
        } else {
            out.SetValue(std::move(wrapped_object), target_object_id);
        }
        R_SUCCEED();
    }

    Result NsAm2MitmService::GetApplicationManagerInterface(sf::Out<sf::SharedPointer<impl::IAmMitmInterface>> out) {
        NsApplicationManagerInterface application_manager{};
        const Result result = nsGetApplicationManagerInterfaceFwd(m_forward_service.get(), std::addressof(application_manager));
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsAm2ObjectPublish, m_client_info.program_id, 0, result, "ns:am2 command=7996");
        R_TRY(result);

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(application_manager.s))};
        auto forward_service = sf::hipc::ServerSession::CreateForwardService();
        *forward_service = application_manager.s;
        application_manager.s = {};

        /* The returned application-manager object exposes far more than the
         * three path commands handled here. Keep it as a MITM object so every
         * unknown command is transparently forwarded. Domain clients use the
         * native object id; non-domain clients receive a paired MITM session
         * carrying the native returned Service as its forward target. */
        auto object_forward_service = forward_service;
        auto wrapped_object = sf::CreateSharedObjectEmplaced<impl::IAmMitmInterface, NsAmMitmService>(std::move(object_forward_service), m_client_info);
        if (target_object_id == sf::cmif::InvalidDomainObjectId) {
            out.SetMitmValue(std::move(wrapped_object), std::move(forward_service));
        } else {
            out.SetValue(std::move(wrapped_object), target_object_id);
        }
        R_SUCCEED();
    }

}
