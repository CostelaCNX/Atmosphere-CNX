/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>

#include "../usb_mitm/usb_launch_trace.hpp"
#include "fs_mitm_service.hpp"
#include "../ns_mitm/ns_verification_trace.hpp"

namespace ams::mitm::fs {

    namespace {

        constexpr u32 CmifInMagic  = util::FourCC<'S', 'F', 'C', 'I'>::Code;
        constexpr u32 CmifOutMagic = util::FourCC<'S', 'F', 'C', 'O'>::Code;

        struct ForwardRequestInfo {
            bool is_domain;
            u32 object_id;
            u32 command_id;
            const u8 *input;
            size_t input_size;
            const u8 *send_buffer;
            size_t send_buffer_size;
            const u8 *recv_buffer;
            size_t recv_buffer_size;
            const u8 *exch_buffer;
            size_t exch_buffer_size;
            u32 num_send_statics;
            u32 num_recv_statics;
            u32 num_send_buffers;
            u32 num_recv_buffers;
            u32 num_exch_buffers;
        };

        struct ForwardResponseInfo {
            Result result;
            const u8 *output;
            size_t output_size;
            u32 num_out_objects;
            u32 num_statics;
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

            const auto hipc_request = hipcParseRequest(const_cast<void *>(message.GetPointer()));
            out->send_buffer = nullptr;
            out->send_buffer_size = 0;
            out->recv_buffer = nullptr;
            out->recv_buffer_size = 0;
            out->exch_buffer = nullptr;
            out->exch_buffer_size = 0;
            out->num_send_statics = hipc_request.meta.num_send_statics;
            out->num_recv_statics = hipc_request.meta.num_recv_statics;
            out->num_send_buffers = hipc_request.meta.num_send_buffers;
            out->num_recv_buffers = hipc_request.meta.num_recv_buffers;
            out->num_exch_buffers = hipc_request.meta.num_exch_buffers;
            if (hipc_request.meta.num_send_buffers > 0 && hipc_request.data.send_buffers != nullptr) {
                const auto *descriptor = std::addressof(hipc_request.data.send_buffers[0]);
                out->send_buffer = static_cast<const u8 *>(hipcGetBufferAddress(descriptor));
                out->send_buffer_size = hipcGetBufferSize(descriptor);
            }
            if (hipc_request.meta.num_recv_buffers > 0 && hipc_request.data.recv_buffers != nullptr) {
                const auto *descriptor = std::addressof(hipc_request.data.recv_buffers[0]);
                out->recv_buffer = static_cast<const u8 *>(hipcGetBufferAddress(descriptor));
                out->recv_buffer_size = hipcGetBufferSize(descriptor);
            }
            if (hipc_request.meta.num_exch_buffers > 0 && hipc_request.data.exch_buffers != nullptr) {
                const auto *descriptor = std::addressof(hipc_request.data.exch_buffers[0]);
                out->exch_buffer = static_cast<const u8 *>(hipcGetBufferAddress(descriptor));
                out->exch_buffer_size = hipcGetBufferSize(descriptor);
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
            out->num_statics = hipc_response.num_statics;
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

        void RecordHandoffBuffer(ncm::ProgramId application_id,
                const ForwardRequestInfo &request, const char *direction,
                const u8 *buffer, size_t size, Result result) {
            if (buffer == nullptr || size == 0) {
                return;
            }

            char details[0x80]{};
            util::SNPrintf(details, sizeof(details),
                "fs-buf %s c=%u o=%08X size=%llu w=%016llX/%016llX",
                direction,
                request.command_id,
                request.object_id,
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(ReadBounded<u64>(buffer, size, 0)),
                static_cast<unsigned long long>(ReadBounded<u64>(buffer, size, sizeof(u64))));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsHandoffBuffer,
                application_id, 0, result, details);
        }

    }

    namespace {
        bool IsActiveControlMount(const ForwardRequestInfo &request, ncm::ProgramId active_id) {
            if ((request.command_id != 8 && request.command_id != 10)
                    || request.input_size < 16
                    || !ncm::IsApplicationId(active_id)
                    || ncm::IsSystemProgramId(active_id)) {
                return false;
            }
            const size_t type_offset = request.command_id == 10 ? 4 : 0;
            return ReadBounded<u32>(request.input, request.input_size, type_offset)
                    == static_cast<u32>(FsFileSystemType_ContentControl)
                && ReadBounded<u64>(request.input, request.input_size, 8) == active_id.value;
        }

        bool IsTracedFsClient(const FsForwardTraceState *state) {
            return state != nullptr
                && (state->client_program_id == ncm::SystemProgramId::Am
                    || state->client_program_id == ncm::SystemProgramId::Ns
                    || state->client_program_id == ncm::SystemAppletId::Qlaunch);
        }
    }

    void ObserveFsForwardTransport(const sf::cmif::PointerAndSize &request_message,
        bool completed, Result transport_result, void *context) {
        const auto *state = static_cast<const FsForwardTraceState *>(context);
        if (!IsTracedFsClient(state)) {
            return;
        }
        ForwardRequestInfo request{};
        const bool decoded = ParseForwardRequest(std::addressof(request), request_message);
        const auto active_id = usb_storage::GetActiveLaunchTraceProgramIdForDiagnostics();
        if (completed) {
            usb_storage::RecordFsNativeReturn(request_message.GetAddress(), transport_result);
        } else if (ncm::IsApplicationId(active_id) && !ncm::IsSystemProgramId(active_id)) {
            usb_storage::BeginFsForwardProgress(state->client_program_id, active_id, request_message.GetAddress(),
                decoded ? request.command_id : 0xFFFFFFFFu, decoded ? request.object_id : 0,
                decoded ? static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))) : 0,
                decoded ? ReadBounded<u64>(request.input, request.input_size, 0) : 0, decoded);
        }
        if (!decoded) {
            return;
        }
        if (!IsActiveControlMount(request, active_id)) {
            return;
        }
        char details[0x80]{};
        util::SNPrintf(details, sizeof(details), "control-%s client=%016llX c=%u o=%08X transport=%08X session=%llX",
            completed ? "return" : "send",
            static_cast<unsigned long long>(state->client_program_id.value),
            request.command_id, request.object_id, transport_result.GetValue(),
            static_cast<unsigned long long>(request_message.GetAddress()));
        /* Memory-only trace: no FS/NCM/USB request, wait or drive access.
         * A return means the syscall returned, not that CMIF mount succeeded. */
        usb_storage::RecordLaunchBoundaryTrace(completed
                ? usb_storage::FsControlForwardReturn : usb_storage::FsControlForwardSend,
            active_id, 3, transport_result, details);
    }

    void ObserveFsForwardRequest(const sf::cmif::PointerAndSize &request_message,
        const sf::cmif::PointerAndSize &response_message, void *context) {
        /* Once a successful application-rights record has armed the passive
         * flight window, retain root FS calls even when the individual command
         * has no ApplicationId in its payload. The trace selector is separate
         * from USB routing, allowing the same binary to capture an SD control
         * launch without changing any filesystem decision. */
        constexpr u32 MaxSuccessRecordsPerClient = 32;
        constexpr u32 MaxFailureRecordsPerClient = 32;

        auto *state = static_cast<FsForwardTraceState *>(context);
        if (state == nullptr
                || (state->client_program_id != ncm::SystemProgramId::Am
                    && state->client_program_id != ncm::SystemProgramId::Ns
                    && state->client_program_id != ncm::SystemAppletId::Qlaunch)) {
            return;
        }

        usb_storage::RecordFsObserverProgress(request_message.GetAddress(), false);
        ON_SCOPE_EXIT { usb_storage::RecordFsObserverProgress(request_message.GetAddress(), true); };

        ForwardRequestInfo request{};
        ForwardResponseInfo response{};
        if (!ParseForwardRequest(std::addressof(request), request_message)) {
            return;
        }

        const ncm::ProgramId active_application_id = usb_storage::GetActiveLaunchTraceProgramIdForDiagnostics();
        const bool control_mount = IsActiveControlMount(request, active_application_id);
        if (!ParseForwardResponse(std::addressof(response), response_message, request.is_domain)) {
            if (control_mount) {
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlForwardDecodeFailure,
                    active_application_id, 3, ResultSuccess(), "control-reply decode=failed; result unknown");
            }
            return;
        }
        usb_storage::RecordFsReplyResult(request_message.GetAddress(), response.result);
        /* Verification does not imply a game-launch trace. Capture its native
         * FS answers before the unrelated launch selector/caps can hide them. */
        ams::mitm::ns::RecordVerificationFs(state->client_program_id,
            request.command_id, request.object_id, static_cast<u32>(request.input_size),
            static_cast<u32>(response.output_size), response.result);
        if (control_mount) {
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details),
                "control-cmif client=%016llX c=%u o=%08X obj=%u h=%u/%u session=%llX",
                static_cast<unsigned long long>(state->client_program_id.value),
                request.command_id, request.object_id, response.num_out_objects,
                response.num_copy_handles, response.num_move_handles,
                static_cast<unsigned long long>(request_message.GetAddress()));
            /* Never hide this completion behind the general per-client cap. */
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsControlForwardCmif,
                active_application_id, 3, response.result, details);
        }
        if (!ncm::IsApplicationId(active_application_id) || ncm::IsSystemProgramId(active_application_id)) {
            return;
        }

        bool detail_suppressed = false;
        {
            /* Cloned root sessions share this service-owned diagnostic state. */
            std::scoped_lock lk(state->lock);
            if (state->active_application_id != active_application_id) {
                state->active_application_id = active_application_id;
                state->success_records = 0;
                state->failure_records = 0;
            }

            if (R_SUCCEEDED(response.result)) {
                if (state->success_records >= MaxSuccessRecordsPerClient) {
                    detail_suppressed = true;
                } else {
                    ++state->success_records;
                }
            } else {
                if (state->failure_records >= MaxFailureRecordsPerClient) {
                    detail_suppressed = true;
                } else {
                    ++state->failure_records;
                }
            }
        }
        if (detail_suppressed) {
            usb_storage::RecordFsDetailSuppressed(R_FAILED(response.result));
            return;
        }

        char details[0x80]{};
        util::SNPrintf(details, sizeof(details),
            "client=%016llX d=%u c=%u o=%08X in=%u i0=%016llX i8=%016llX i16=%08X obj=%u h=%u/%u",
            static_cast<unsigned long long>(state->client_program_id.value),
            request.is_domain ? 1u : 0u,
            request.command_id,
            request.object_id,
            static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
            static_cast<unsigned long long>(ReadBounded<u64>(request.input, request.input_size, 0)),
            static_cast<unsigned long long>(ReadBounded<u64>(request.input, request.input_size, 8)),
            ReadBounded<u32>(request.input, request.input_size, 16),
            response.num_out_objects,
            response.num_copy_handles,
            response.num_move_handles);
        usb_storage::RecordLaunchBoundaryTrace(
            R_FAILED(response.result) ? usb_storage::FsForwardFailure : usb_storage::FsForwardCommand,
            active_application_id,
            0, response.result, details);

        /* R103's first new boundary is qlaunch's successful FS commands 83
         * and 67 after Player Select and before CreateApplication. Preserve
         * their inline shape and bounded first-buffer contents while the
         * original mapped request is still valid. This observer never writes
         * to the request, response or any client buffer. */
        util::SNPrintf(details, sizeof(details),
            "fs-ipc c=%u o=%08X in=%u out=%u b=%u/%u/%u p=%u/%u rs=%u obj=%u h=%u/%u i=%016llX w=%08X/%08X",
            request.command_id,
            request.object_id,
            static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
            static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
            request.num_send_buffers,
            request.num_recv_buffers,
            request.num_exch_buffers,
            request.num_send_statics,
            request.num_recv_statics,
            response.num_statics,
            response.num_out_objects,
            response.num_copy_handles,
            response.num_move_handles,
            static_cast<unsigned long long>(ReadBounded<u64>(request.input, request.input_size, 0)),
            ReadBounded<u32>(response.output, response.output_size, 0),
            ReadBounded<u32>(response.output, response.output_size, sizeof(u32)));
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::FsHandoffCommand,
            active_application_id, 0, response.result, details);

        RecordHandoffBuffer(active_application_id, request, "send",
            request.send_buffer, request.send_buffer_size, response.result);
        RecordHandoffBuffer(active_application_id, request, "recv",
            request.recv_buffer, request.recv_buffer_size, response.result);
        RecordHandoffBuffer(active_application_id, request, "exch",
            request.exch_buffer, request.exch_buffer_size, response.result);
    }

}
