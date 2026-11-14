/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>
#include "../usb_mitm/usb_hang_capture.hpp"

#include "../usb_mitm/usb_launch_trace.hpp"
#include "ns_am2_trace_mitm_service.hpp"
#include "ns_handoff_trace.hpp"
#include "ns_shim.h"

namespace ams::mitm::ns {

    namespace {

        constexpr u32 CmifInMagic  = util::FourCC<'S', 'F', 'C', 'I'>::Code;
        constexpr u32 CmifOutMagic = util::FourCC<'S', 'F', 'C', 'O'>::Code;

        struct ForwardRequestInfo {
            bool is_domain;
            u32 object_id;
            u32 command_id;
            const u8 *input;
            size_t input_size;
            const u8 *recv_buffer;
            size_t recv_buffer_size;
        };

        struct ForwardResponseInfo {
            Result result;
            const u8 *output;
            size_t output_size;
            u32 num_out_objects;
            u32 num_copy_handles;
            u32 num_move_handles;
        };

        bool GetRequestRawData(const sf::cmif::PointerAndSize &message, const u8 **out_raw, size_t *out_size) {
            if (message.GetPointer() == nullptr || message.GetSize() < sizeof(HipcHeader)) {
                return false;
            }

            const auto request = hipcParseRequest(const_cast<void *>(message.GetPointer()));
            const size_t data_size = static_cast<size_t>(request.meta.num_data_words) * sizeof(u32);
            if (request.data.data_words == nullptr || data_size < 0x10) {
                return false;
            }

            const uintptr_t raw_address = util::AlignUp(reinterpret_cast<uintptr_t>(request.data.data_words), 0x10);
            const size_t raw_size = data_size - 0x10;
            const uintptr_t message_begin = message.GetAddress();
            const uintptr_t message_end = message_begin + message.GetSize();
            if (raw_address < message_begin || raw_address > message_end || raw_size > message_end - raw_address) {
                return false;
            }

            *out_raw = reinterpret_cast<const u8 *>(raw_address);
            *out_size = raw_size;
            return true;
        }

        bool GetResponseRawData(const sf::cmif::PointerAndSize &message, const u8 **out_raw, size_t *out_size) {
            if (message.GetPointer() == nullptr || message.GetSize() < sizeof(HipcHeader)) {
                return false;
            }

            const auto response = hipcParseResponse(const_cast<void *>(message.GetPointer()));
            const size_t data_size = static_cast<size_t>(response.num_data_words) * sizeof(u32);
            if (response.data_words == nullptr || data_size < 0x10) {
                return false;
            }

            const uintptr_t raw_address = util::AlignUp(reinterpret_cast<uintptr_t>(response.data_words), 0x10);
            const size_t raw_size = data_size - 0x10;
            const uintptr_t message_begin = message.GetAddress();
            const uintptr_t message_end = message_begin + message.GetSize();
            if (raw_address < message_begin || raw_address > message_end || raw_size > message_end - raw_address) {
                return false;
            }

            *out_raw = reinterpret_cast<const u8 *>(raw_address);
            *out_size = raw_size;
            return true;
        }

        bool ParseForwardRequest(ForwardRequestInfo *out, const sf::cmif::PointerAndSize &message) {
            const u8 *raw = nullptr;
            size_t raw_size = 0;
            if (!GetRequestRawData(message, std::addressof(raw), std::addressof(raw_size))) {
                return false;
            }

            /* Map-alias output buffers are described by the request and stay
             * mapped until the server reply is returned. The forward observer
             * runs after stock NS filled them, so a bounded read here is a
             * passive snapshot of qlaunch's native command-2050 rights record. */
            const auto hipc_request = hipcParseRequest(const_cast<void *>(message.GetPointer()));
            out->recv_buffer = nullptr;
            out->recv_buffer_size = 0;
            if (hipc_request.meta.num_recv_buffers > 0 && hipc_request.data.recv_buffers != nullptr) {
                const auto *descriptor = std::addressof(hipc_request.data.recv_buffers[0]);
                out->recv_buffer = static_cast<const u8 *>(hipcGetBufferAddress(descriptor));
                out->recv_buffer_size = hipcGetBufferSize(descriptor);
            }

            if (raw_size >= sizeof(CmifDomainInHeader) + sizeof(CmifInHeader)) {
                const auto *domain_header = reinterpret_cast<const CmifDomainInHeader *>(raw);
                const auto *in_header = reinterpret_cast<const CmifInHeader *>(raw + sizeof(CmifDomainInHeader));
                if (domain_header->type == CmifDomainRequestType_SendMessage
                        && domain_header->data_size >= sizeof(CmifInHeader)
                        && domain_header->data_size <= raw_size - sizeof(CmifDomainInHeader)
                        && in_header->magic == CmifInMagic) {
                    out->is_domain = true;
                    out->object_id = domain_header->object_id;
                    out->command_id = in_header->command_id;
                    out->input = raw + sizeof(CmifDomainInHeader) + sizeof(CmifInHeader);
                    out->input_size = domain_header->data_size - sizeof(CmifInHeader);
                    return true;
                }
            }

            if (raw_size >= sizeof(CmifInHeader)) {
                const auto *in_header = reinterpret_cast<const CmifInHeader *>(raw);
                if (in_header->magic == CmifInMagic) {
                    out->is_domain = false;
                    out->object_id = 0;
                    out->command_id = in_header->command_id;
                    out->input = raw + sizeof(CmifInHeader);
                    out->input_size = raw_size - sizeof(CmifInHeader);
                    return true;
                }
            }

            return false;
        }

        bool ParseForwardResponse(ForwardResponseInfo *out, const sf::cmif::PointerAndSize &message, bool is_domain) {
            const u8 *raw = nullptr;
            size_t raw_size = 0;
            if (!GetResponseRawData(message, std::addressof(raw), std::addressof(raw_size))) {
                return false;
            }

            const auto hipc_response = hipcParseResponse(const_cast<void *>(message.GetPointer()));
            if (is_domain) {
                if (raw_size < sizeof(CmifDomainOutHeader) + sizeof(CmifOutHeader)) {
                    return false;
                }
                const auto *domain_header = reinterpret_cast<const CmifDomainOutHeader *>(raw);
                const auto *out_header = reinterpret_cast<const CmifOutHeader *>(raw + sizeof(CmifDomainOutHeader));
                if (out_header->magic != CmifOutMagic) {
                    return false;
                }
                out->result = out_header->result;
                out->output = raw + sizeof(CmifDomainOutHeader) + sizeof(CmifOutHeader);
                out->output_size = raw_size - sizeof(CmifDomainOutHeader) - sizeof(CmifOutHeader);
                out->num_out_objects = domain_header->num_out_objects;
            } else {
                if (raw_size < sizeof(CmifOutHeader)) {
                    return false;
                }
                const auto *out_header = reinterpret_cast<const CmifOutHeader *>(raw);
                if (out_header->magic != CmifOutMagic) {
                    return false;
                }
                out->result = out_header->result;
                out->output = raw + sizeof(CmifOutHeader);
                out->output_size = raw_size - sizeof(CmifOutHeader);
                out->num_out_objects = 0;
            }
            out->num_copy_handles = hipc_response.num_copy_handles;
            out->num_move_handles = hipc_response.num_move_handles;
            return true;
        }

        template<typename T>
        T ReadBounded(const u8 *data, size_t size, size_t offset) {
            T value{};
            if (data != nullptr && offset <= size && sizeof(value) <= size - offset) {
                std::memcpy(std::addressof(value), data + offset, sizeof(value));
            }
            return value;
        }

        struct ApplicationRightsOnClientWire {
            u64 application_id;
            u64 uid_low;
            u64 uid_high;
            u8 flags_x18;
            u8 flags_x19;
            u8 unknown_x1a[0x6];
        };
        static_assert(sizeof(ApplicationRightsOnClientWire) == 0x20);

        ncm::ProgramId FindApplicationId(const ForwardRequestInfo &request, ncm::ProgramId fallback) {
            const size_t scan_size = std::min(request.input_size, static_cast<size_t>(0x40));
            for (size_t offset = 0; offset <= scan_size && sizeof(u64) <= scan_size - offset; offset += sizeof(u32)) {
                const ncm::ProgramId candidate{ReadBounded<u64>(request.input, request.input_size, offset)};
                if (ncm::IsApplicationId(candidate) && !ncm::IsSystemProgramId(candidate)) {
                    return candidate;
                }
            }
            return fallback;
        }

    }

    void ObserveNsAm2ForwardTransport(const sf::cmif::PointerAndSize &request_message, bool completed, Result result, void *context) {
        auto *state = static_cast<NsAm2ForwardTraceState *>(context);
        if (!state) return;
        if (!completed) {
            ForwardRequestInfo request{};
            const bool decoded = ParseForwardRequest(std::addressof(request), request_message);
            state->pending_call = handoff::Next();
            state->pending_command = decoded ? request.command_id : UINT32_MAX;
            state->pending_object = decoded ? request.object_id : 0;
        }
        handoff::Record(state->pending_call, state->client_program_id,
            state->pending_command, state->pending_object,
            completed ? "native-return" : "native-send", result);
    }

    void ObserveNsAm2ForwardRequest(const sf::cmif::PointerAndSize &request_message, const sf::cmif::PointerAndSize &response_message, void *context) {
        const auto *state = static_cast<const NsAm2ForwardTraceState *>(context);
        if (state == nullptr
                || (state->client_program_id != ncm::SystemProgramId::Am
                    && state->client_program_id != ncm::SystemAppletId::Qlaunch)) {
            return;
        }

        ForwardRequestInfo request{};
        if (!ParseForwardRequest(std::addressof(request), request_message)) {
            return;
        }

        ForwardResponseInfo response{};
        if (!ParseForwardResponse(std::addressof(response), response_message, request.is_domain)) {
            char details[0x80]{};
            const char *const scope = state->read_only_control_data_child
                ? "ns-control-data"
                : (state->application_manager_child ? "ns-am-child" : "ns-am2");
            util::SNPrintf(details, sizeof(details), "%s response-parse-fail d=%u c=%u o=%08X in=%u",
                scope,
                request.is_domain ? 1u : 0u, request.command_id, request.object_id,
                static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))));
            const auto failure_stage = state->read_only_control_data_child
                ? usb::content::LoaderProbeStage::NsControlDataForwardFailure
                : usb_storage::NsAm2ForwardFailure;
            usb_storage::RecordLaunchBoundaryTrace(failure_stage, state->client_program_id, 0, sf::cmif::ResultInvalidInHeader(), details);
            return;
        }

        const ncm::ProgramId active_application_id = usb_storage::GetActiveLaunchTraceProgramIdForDiagnostics();
        const ncm::ProgramId fallback_id = active_application_id != ncm::InvalidProgramId
            ? active_application_id : state->client_program_id;
        const ncm::ProgramId trace_id = FindApplicationId(request, fallback_id);
        char details[0x80]{};
        if (state->read_only_control_data_child) {
            util::SNPrintf(details, sizeof(details),
                "ns-control-data d=%u c=%u in=%u out=%u src=%u f=%02X/%02X id=%016llX actual=%08X extra=%08X h=%u/%u",
                request.is_domain ? 1u : 0u,
                request.command_id,
                static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                ReadBounded<u8>(request.input, request.input_size, 0),
                ReadBounded<u8>(request.input, request.input_size, 1),
                ReadBounded<u8>(request.input, request.input_size, 2),
                static_cast<unsigned long long>(ReadBounded<u64>(request.input, request.input_size, sizeof(u64))),
                ReadBounded<u32>(response.output, response.output_size, 0),
                ReadBounded<u32>(response.output, response.output_size, sizeof(u32)),
                response.num_copy_handles,
                response.num_move_handles);
            usb_storage::RecordLaunchBoundaryTrace(
                R_FAILED(response.result)
                    ? usb::content::LoaderProbeStage::NsControlDataForwardFailure
                    : usb::content::LoaderProbeStage::NsControlDataForwardCommand,
                trace_id, 0, response.result, details);
        } else {
            util::SNPrintf(details, sizeof(details),
                "%s d=%u c=%u o=%08X in=%u out=%u n=%u h=%u/%u i=%016llX/%016llX w=%08X/%08X",
                state->application_manager_child ? "ns-am-child" : "ns-am2",
                request.is_domain ? 1u : 0u,
                request.command_id,
                request.object_id,
                static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                response.num_out_objects,
                response.num_copy_handles,
                response.num_move_handles,
                static_cast<unsigned long long>(ReadBounded<u64>(request.input, request.input_size, 0)),
                static_cast<unsigned long long>(ReadBounded<u64>(request.input, request.input_size, sizeof(u64))),
                ReadBounded<u32>(response.output, response.output_size, 0),
                ReadBounded<u32>(response.output, response.output_size, sizeof(u32)));
            usb_storage::RecordLaunchBoundaryTrace(R_FAILED(response.result) ? usb_storage::NsAm2ForwardFailure : usb_storage::NsAm2ForwardCommand,
                trace_id, 0, response.result, details);

            if (state->application_manager_child && request.command_id == 2050) {
                const u32 total = ReadBounded<u32>(response.output, response.output_size, 0);
                const u32 query_flags = ReadBounded<u32>(request.input, request.input_size, 0);
                ncm::ProgramId rights_application_id = trace_id;
                bool has_rights_record = false;
                if (R_SUCCEEDED(response.result) && total > 0
                        && request.recv_buffer != nullptr
                        && request.recv_buffer_size >= sizeof(ApplicationRightsOnClientWire)) {
                    const auto rights = ReadBounded<ApplicationRightsOnClientWire>(request.recv_buffer, request.recv_buffer_size, 0);
                    const ncm::ProgramId record_application_id{rights.application_id};
                    if (ncm::IsApplicationId(record_application_id) && !ncm::IsSystemProgramId(record_application_id)) {
                        rights_application_id = record_application_id;
                        has_rights_record = true;
                    }
                    util::SNPrintf(details, sizeof(details),
                        "rights total=%u buf=%u q=%08X id=%016llX uid=%016llX/%016llX f=%02X/%02X x=%08X/%04X",
                        total,
                        static_cast<u32>(std::min(request.recv_buffer_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                        query_flags,
                        static_cast<unsigned long long>(rights.application_id),
                        static_cast<unsigned long long>(rights.uid_low),
                        static_cast<unsigned long long>(rights.uid_high),
                        rights.flags_x18,
                        rights.flags_x19,
                        ReadBounded<u32>(rights.unknown_x1a, sizeof(rights.unknown_x1a), 0),
                        ReadBounded<u16>(rights.unknown_x1a, sizeof(rights.unknown_x1a), sizeof(u32)));
                } else {
                    util::SNPrintf(details, sizeof(details),
                        "rights total=%u buf=%u q=%08X record=unavailable",
                        total,
                        static_cast<u32>(std::min(request.recv_buffer_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                        query_flags);
                }
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationRightsResult,
                    rights_application_id, 0, response.result, details);

                /* A rights lookup occurs before qlaunch opens Player Select.
                 * Keep this boundary diagnostic-only: appletAE's structural
                 * observer remains active and can identify the later exact
                 * CreateApplication request without a process-wide marker.
                 * Both the bounded launch trace and USB route are armed at
                 * that exact mapped-title boundary instead. */
                if (R_SUCCEEDED(response.result) && total > 0 && has_rights_record) {
                    const bool mapped_usb = usb_storage::IsUsbMappedTitleLaunchEnabled()
                        && usb_storage::PrimeUsbProgramPathFromPersistentMap(rights_application_id);
                    if (mapped_usb) {
                        /* Preserve R112's trace selection and delayed launch
                         * marker. The forwarding layer now protects the native
                         * reply while the persistent-map lookup performs IPC. */
                        usb_storage::MarkLaunchTraceActiveForDiagnostics(rights_application_id);
                        usb_storage::hang_capture::NotifySelection(state->client_program_id, rights_application_id);
                        usb_storage::RecordLaunchBoundaryTrace(usb_storage::UsbRouteActivationDeferred,
                            rights_application_id, 0, ResultSuccess(), "mapped Program primed; USB marker deferred until CreateApplication");
                    }
                    char arm_details[0x80]{};
                    util::SNPrintf(arm_details, sizeof(arm_details),
                        "rights-observed id=%016llX total=%u mapped=%u",
                        static_cast<unsigned long long>(rights_application_id.value), total,
                        mapped_usb ? 1u : 0u);
                    usb_storage::RecordLaunchBoundaryTrace(usb_storage::UsbLaunchTraceArmed,
                        rights_application_id, 0, ResultSuccess(), arm_details);
                }
            } else if (state->application_manager_child && request.command_id == 906) {
                util::SNPrintf(details, sizeof(details),
                    "update requested=%u reason=%08X out=%u",
                    ReadBounded<u8>(response.output, response.output_size, 0) & 1u,
                    ReadBounded<u32>(response.output, response.output_size, sizeof(u32)),
                    static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))));
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsApplicationUpdateResult,
                    trace_id, 0, response.result, details);
            }
        }
    }

    Result NsAmChildTraceMitmService::GetMainApplicationProgramIndexByApplicationLaunchInfo(sf::Out<u8> out, const ApplicationLaunchInfoV19 &launch_info) {
        u8 stock_program_index = 0;
        const Result stock_result = serviceDispatchInOut(m_forward_service.get(), 97, launch_info, stock_program_index);

        const ncm::ProgramId application_id{launch_info.application_id};
        const ncm::ProgramId active_application_id = usb_storage::GetActiveUsbLaunchProgramIdForDiagnostics();
        const bool exact_usb_launch = usb_storage::IsUsbMappedTitleLaunchEnabled()
            && application_id != ncm::InvalidProgramId
            && (active_application_id == application_id || usb_storage::PrimeUsbProgramPathFromPersistentMap(application_id));

        char details[0xA0]{};
        util::SNPrintf(details, sizeof(details),
            "ns-am-child c=97 id=%016llX v=%u f=%08X s=%u/%u stock=%08X idx=%u usb=%u",
            static_cast<unsigned long long>(application_id.value),
            launch_info.version,
            launch_info.launch_flags,
            launch_info.application_storage_id,
            launch_info.update_storage_id,
            stock_result.GetValue(),
            stock_program_index,
            exact_usb_launch ? 1u : 0u);

        if (R_SUCCEEDED(stock_result)) {
            out.SetValue(stock_program_index);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsAm2ForwardCommand,
                application_id, 0, stock_result, details);
            R_SUCCEED();
        }

        /* R77's full chronological trace proved that command 97 is not part
         * of this failing launch sequence. Preserve its native result instead
         * of applying a speculative ProgramIndex substitution. */
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsAm2ForwardFailure,
            application_id, 0, stock_result, details);
        R_RETURN(stock_result);
    }

    Result NsAm2TraceMitmService::GetApplicationManagerInterface(sf::Out<sf::SharedPointer<impl::INsAmChildTraceMitmInterface>> out) {
        NsApplicationManagerInterface application_manager{};
        const Result result = nsGetApplicationManagerInterfaceFwd(m_forward_service.get(), std::addressof(application_manager));
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::NsAm2ObjectPublish, m_client_info.program_id, 0, result, "ns:am2 trace command=7996");
        R_TRY(result);

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(std::addressof(application_manager.s))};
        auto forward_service = sf::hipc::ServerSession::CreateForwardService();
        *forward_service = application_manager.s;
        application_manager.s = {};

        auto object_forward_service = forward_service;
        auto wrapped_object = sf::CreateSharedObjectEmplaced<impl::INsAmChildTraceMitmInterface, NsAmChildTraceMitmService>(std::move(object_forward_service), m_client_info);
        if (target_object_id == sf::cmif::InvalidDomainObjectId) {
            out.SetMitmValue(std::move(wrapped_object), std::move(forward_service));
        } else {
            /* A domain child remains on the parent session, whose root
             * observer already sees its raw-forwarded object commands. */
            out.SetValue(std::move(wrapped_object), target_object_id);
        }
        R_SUCCEED();
    }

}
