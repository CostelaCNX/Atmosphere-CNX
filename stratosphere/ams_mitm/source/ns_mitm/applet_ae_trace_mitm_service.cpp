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
#include "applet_ae_trace_mitm_service.hpp"
#include "ncm_launch_trace_client.hpp"

namespace ams::mitm::ns {

    namespace {

        constexpr u32 CmifInMagic  = util::FourCC<'S', 'F', 'C', 'I'>::Code;
        constexpr u32 CmifOutMagic = util::FourCC<'S', 'F', 'C', 'O'>::Code;
        struct DomainRequestInfo {
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
            u32 num_send_buffers;
            u32 num_recv_buffers;
            u32 num_exch_buffers;
            bool send_pid;
            u32 num_copy_handles;
            u32 num_move_handles;
        };

        struct DomainResponseInfo {
            Result result;
            const u8 *output;
            size_t output_size;
            u32 num_out_objects;
            u32 num_data_words;
            u32 num_copy_handles;
            u32 num_move_handles;
            u32 first_copy_handle;
            u32 first_move_handle;
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

        bool ParseDomainRequest(DomainRequestInfo *out, const sf::cmif::PointerAndSize &message) {
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

            if (raw_size < sizeof(CmifDomainInHeader) + sizeof(CmifInHeader)) {
                return false;
            }

            const auto *domain_header = reinterpret_cast<const CmifDomainInHeader *>(raw);
            if (domain_header->type != CmifDomainRequestType_SendMessage || domain_header->data_size < sizeof(CmifInHeader)
                    || domain_header->data_size > raw_size - sizeof(CmifDomainInHeader)) {
                return false;
            }

            const auto *in_header = reinterpret_cast<const CmifInHeader *>(raw + sizeof(CmifDomainInHeader));
            if (in_header->magic != CmifInMagic) {
                return false;
            }

            out->object_id = domain_header->object_id;
            out->command_id = in_header->command_id;
            out->input = raw + sizeof(CmifDomainInHeader) + sizeof(CmifInHeader);
            out->input_size = domain_header->data_size - sizeof(CmifInHeader);
            out->send_pid = hipc_request.meta.send_pid;
            out->num_copy_handles = hipc_request.meta.num_copy_handles;
            out->num_move_handles = hipc_request.meta.num_move_handles;
            return true;
        }

        bool IsOpenSystemAppletProxyRequest(const DomainRequestInfo &request) {
            /* appletAE command 100 is not unique: IApplicationCreator also
             * uses it for PopFloatingApplicationForDevelopment. The root
             * OpenSystemAppletProxy request carries a reserved u64, the
             * caller PID and one process handle. Require that wire signature
             * before treating command 100 as the root object transition. */
            return request.command_id == 100
                && request.input_size >= sizeof(u64)
                && request.send_pid
                && request.num_copy_handles + request.num_move_handles > 0;
        }

        bool ParseDomainResponse(DomainResponseInfo *out, const sf::cmif::PointerAndSize &message) {
            const u8 *raw = nullptr;
            size_t raw_size = 0;
            if (!GetResponseRawData(message, std::addressof(raw), std::addressof(raw_size)) || raw_size < sizeof(CmifDomainOutHeader) + sizeof(CmifOutHeader)) {
                return false;
            }

            const auto hipc_response = hipcParseResponse(const_cast<void *>(message.GetPointer()));

            const auto *domain_header = reinterpret_cast<const CmifDomainOutHeader *>(raw);
            const auto *out_header = reinterpret_cast<const CmifOutHeader *>(raw + sizeof(CmifDomainOutHeader));
            if (out_header->magic != CmifOutMagic) {
                return false;
            }

            out->result = out_header->result;
            out->output = raw + sizeof(CmifDomainOutHeader) + sizeof(CmifOutHeader);
            out->output_size = raw_size - sizeof(CmifDomainOutHeader) - sizeof(CmifOutHeader);
            out->num_out_objects = domain_header->num_out_objects;
            out->num_data_words = hipc_response.num_data_words;
            out->num_copy_handles = hipc_response.num_copy_handles;
            out->num_move_handles = hipc_response.num_move_handles;
            out->first_copy_handle = hipc_response.num_copy_handles > 0 ? hipc_response.copy_handles[0] : 0;
            out->first_move_handle = hipc_response.num_move_handles > 0 ? hipc_response.move_handles[0] : 0;
            return true;
        }

        u32 ReadResponseOutputWord(const DomainResponseInfo &response, size_t index) {
            u32 value = 0;
            const size_t offset = index * sizeof(value);
            if (offset <= response.output_size && response.output_size - offset >= sizeof(value)) {
                std::memcpy(std::addressof(value), response.output + offset, sizeof(value));
            }
            return value;
        }

        u64 ReadBufferWord(const u8 *buffer, size_t size, size_t offset) {
            u64 value = 0;
            if (buffer != nullptr && offset < size) {
                const size_t copy_size = std::min(sizeof(value), size - offset);
                std::memcpy(std::addressof(value), buffer + offset, copy_size);
            }
            return value;
        }

        void RecordActiveAppletBuffer(ncm::ProgramId application_id, const DomainRequestInfo &request,
                const char *direction, const u8 *buffer, size_t size, Result result) {
            if (buffer == nullptr || size == 0) {
                return;
            }

            char details[0x80]{};
            util::SNPrintf(details, sizeof(details),
                "ae-buf %s c=%u o=%08X size=%llu w=%016llX/%016llX",
                direction,
                request.command_id,
                request.object_id,
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(ReadBufferWord(buffer, size, 0)),
                static_cast<unsigned long long>(ReadBufferWord(buffer, size, sizeof(u64))));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletAeActiveBuffer,
                application_id, 0, result, details);
        }

        void RecordActiveAppletCommand(ncm::ProgramId application_id, const DomainRequestInfo &request,
                const DomainResponseInfo &response) {
            const u64 input0 = ReadBufferWord(request.input, request.input_size, 0);

            char details[0x80]{};
            util::SNPrintf(details, sizeof(details),
                "ae-ipc c=%u o=%08X in=%u out=%u n=%u b=%u/%u/%u h=%u/%u i=%016llX w=%08X/%08X",
                request.command_id,
                request.object_id,
                static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                response.num_out_objects,
                request.num_send_buffers,
                request.num_recv_buffers,
                request.num_exch_buffers,
                response.num_copy_handles,
                response.num_move_handles,
                static_cast<unsigned long long>(input0),
                ReadResponseOutputWord(response, 0),
                ReadResponseOutputWord(response, 1));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletAeActiveCommand,
                application_id, 0, response.result, details);

            /* Map-alias buffers are described by the request and remain mapped
             * until the forwarded stock reply returns. The observer runs after
             * that reply, so these bounded 16-byte snapshots expose Player
             * Select's native output without modifying qlaunch's buffers. */
            RecordActiveAppletBuffer(application_id, request, "send", request.send_buffer, request.send_buffer_size, response.result);
            RecordActiveAppletBuffer(application_id, request, "recv", request.recv_buffer, request.recv_buffer_size, response.result);
            RecordActiveAppletBuffer(application_id, request, "exch", request.exch_buffer, request.exch_buffer_size, response.result);
        }

        u32 ReadObjectOnlyResponse(const DomainResponseInfo &response) {
            /* Commands 100, 22, and IApplicationCreator commands 0/1/10
             * return one object and no scalar output. The domain object id is
             * conventionally the first word following the CMIF output header.
             * R43 records the complete bounded response shape when firmware
             * 22.5.0 does not expose a usable value there; it does not infer or
             * substitute another native object identifier. */
            u32 object_id = 0;
            if (R_SUCCEEDED(response.result) && response.output_size >= sizeof(object_id)) {
                std::memcpy(std::addressof(object_id), response.output, sizeof(object_id));
            }
            return object_id;
        }

        ncm::ProgramId ReadProgramId(const DomainRequestInfo &request) {
            ncm::ProgramId program_id{};
            if (request.input_size >= sizeof(program_id.value)) {
                std::memcpy(std::addressof(program_id.value), request.input, sizeof(program_id.value));
            }
            return program_id;
        }

        AppletAeApplicationAccessorTrace *FindAccessor(AppletAeForwardTraceState *state, u32 object_id) {
            for (size_t i = 0; i < state->accessor_count; ++i) {
                if (state->accessors[i].object_id == object_id) {
                    return state->accessors + i;
                }
            }
            return nullptr;
        }

        void StoreAccessor(AppletAeForwardTraceState *state, u32 object_id, ncm::ProgramId application_id) {
            if (object_id == 0) {
                return;
            }
            if (auto *existing = FindAccessor(state, object_id); existing != nullptr) {
                existing->application_id = application_id;
                existing->ncm_trace_armed = false;
                return;
            }
            if (state->accessor_count < AppletAeMaxTrackedApplicationAccessors) {
                auto &entry = state->accessors[state->accessor_count++];
                entry.object_id = object_id;
                entry.application_id = application_id;
                entry.ncm_trace_armed = false;
            }
        }

        void ArmNcmTrace(AppletAeForwardTraceState *state, u32 accessor_object_id, ncm::ProgramId application_id, const char *boundary) {
            /* R90's independent preflight proved that the post-mount NCM
             * ProgramId cache resolves Meta, Control and Program content on
             * first, Host and reopened SD handles. Do not keep the private
             * launch flight recorder armed across qlaunch application
             * creation: its cross-service capture is diagnostic-only and is
             * now the sole extra participant between the successful Control
             * NACP read and Program/code open. Keep the worker and ABI in the
             * binary so this is a one-variable runtime change. */
            AMS_UNUSED(state);

            /* Rights lookup precedes Player Select, so it is too early to
             * publish either process-wide marker there. CreateApplication is
             * the first exact application boundary after Player Select and
             * still precedes IApplicationAccessor::Start and every observed
             * Control/Program FS request. Activate both the bounded outer
             * trace and USB route only for an exact mapped title here;
             * repeated Start/accessor calls leave them untouched. This does
             * not re-enable the retired cross-service NCM recorder. */
            const bool exact_usb_application = ncm::IsApplicationId(application_id)
                && !ncm::IsSystemProgramId(application_id)
                && usb_storage::IsUsbMappedTitleLaunchEnabled()
                && usb_storage::PrimeUsbProgramPathFromPersistentMap(application_id);
            if (exact_usb_application) {
                const bool activate_trace = usb_storage::GetActiveLaunchTraceProgramIdForDiagnostics() != application_id;
                const bool activate_usb = usb_storage::GetActiveUsbLaunchProgramIdForDiagnostics() != application_id;
                if (activate_trace) {
                    usb_storage::MarkLaunchTraceActiveForDiagnostics(application_id);
                }
                if (activate_usb) {
                    usb_storage::MarkUsbLaunchActiveForDiagnostics(application_id);
                }
                if (!activate_trace && !activate_usb) {
                    return;
                }
                char details[0x80]{};
                util::SNPrintf(details, sizeof(details),
                    "usb+trace active boundary=%s accessor=%u",
                    boundary != nullptr ? boundary : "unknown", accessor_object_id);
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::UsbRouteActivatedAtCreate,
                    application_id, 0, ResultSuccess(), details);
            }
        }

        void RemoveAccessor(AppletAeForwardTraceState *state, u32 object_id) {
            for (size_t i = 0; i < state->accessor_count; ++i) {
                if (state->accessors[i].object_id == object_id) {
                    --state->accessor_count;
                    if (i != state->accessor_count) {
                        state->accessors[i] = state->accessors[state->accessor_count];
                    }
                    state->accessors[state->accessor_count] = {};
                    return;
                }
            }
        }

        void RecordObjectResult(usb::content::LoaderProbeStage stage, ncm::ProgramId program_id, const char *name, const DomainRequestInfo &request, const DomainResponseInfo &response, u32 returned_object_id) {
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details),
                "%s c=%u o=%08X r=%08X n=%u s=%u d=%u c=%u m=%u h=%08X/%08X w=%08X/%08X/%08X/%08X",
                name,
                request.command_id,
                request.object_id,
                returned_object_id,
                response.num_out_objects,
                static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                response.num_data_words,
                response.num_copy_handles,
                response.num_move_handles,
                response.first_copy_handle,
                response.first_move_handle,
                ReadResponseOutputWord(response, 0),
                ReadResponseOutputWord(response, 1),
                ReadResponseOutputWord(response, 2),
                ReadResponseOutputWord(response, 3));
            usb_storage::RecordLaunchBoundaryTrace(stage, program_id, 0, response.result, details);
        }

    }

    void ObserveAppletAeForwardTransport(const sf::cmif::PointerAndSize &request_message,
            bool completed, Result transport_result, void *context) {
        AMS_UNUSED(context);
        const u64 session = request_message.GetAddress();
        if (completed) {
            usb_storage::RecordAppletNativeReturn(session, transport_result);
            return;
        }
        const auto active = usb_storage::GetActiveLaunchTraceProgramIdForDiagnostics();
        // Capture HOME close/launch traffic even with no mapped USB game active.
        // InvalidProgramId is deliberately "unknown", not a guessed application.
        DomainRequestInfo request{};
        const bool decoded = ParseDomainRequest(std::addressof(request), request_message);
        /* No mapped-buffer reads, NCM/FS requests, routes or disk writes.
         * Unknown wire shapes remain visible rather than silently skipped. */
        usb_storage::BeginAppletForwardProgress(active, session,
            decoded ? request.command_id : std::numeric_limits<u32>::max(),
            decoded ? request.object_id : 0,
            decoded ? static_cast<u32>(request.input_size) : 0,
            decoded ? ReadProgramId(request).value : 0, decoded);
    }

    void ObserveAppletAeForwardRequest(const sf::cmif::PointerAndSize &request_message, const sf::cmif::PointerAndSize &response_message, void *context) {
        usb_storage::RecordAppletObserverProgress(request_message.GetAddress(), false);
        ON_SCOPE_EXIT { usb_storage::RecordAppletObserverProgress(request_message.GetAddress(), true); };
        auto *state = static_cast<AppletAeForwardTraceState *>(context);
        if (state == nullptr) {
            return;
        }

        DomainRequestInfo request{};
        if (!ParseDomainRequest(std::addressof(request), request_message)) {
            return;
        }

        const ncm::ProgramId qlaunch = ncm::SystemAppletId::Qlaunch;
        DomainResponseInfo response{};
        if (!ParseDomainResponse(std::addressof(response), response_message)) {
            const ncm::ProgramId active_application_id = usb_storage::GetActiveLaunchTraceProgramIdForDiagnostics();
            if (active_application_id != ncm::InvalidProgramId
                    || request.command_id == 0 || request.command_id == 1 || request.command_id == 10
                    || request.command_id == 22 || request.command_id == 30 || request.command_id == 100
                    || request.command_id == 110 || request.command_id == 120) {
                char details[0x80]{};
                util::SNPrintf(details, sizeof(details),
                    "response-parse-fail c=%u o=%08X in=%u pid=%u c=%u m=%u",
                    request.command_id,
                    request.object_id,
                    static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                    request.send_pid ? 1u : 0u,
                    request.num_copy_handles,
                    request.num_move_handles);
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletAeResponseParseFailed,
                    active_application_id != ncm::InvalidProgramId ? active_application_id : qlaunch,
                    0, sf::cmif::ResultInvalidInHeader(), details);
            }
            return;
        }

        const ncm::ProgramId active_application_id = usb_storage::GetActiveLaunchTraceProgramIdForDiagnostics();
        if (ncm::IsApplicationId(active_application_id) && !ncm::IsSystemProgramId(active_application_id)) {
            constexpr u32 MaxActiveTraceCommands = 96;
            if (state->active_trace_application_id != active_application_id) {
                state->active_trace_application_id = active_application_id;
                state->active_trace_records = 0;
            }
            if (state->active_trace_records < MaxActiveTraceCommands) {
                ++state->active_trace_records;
                RecordActiveAppletCommand(active_application_id, request, response);
            }
        }

        /* appletAE::OpenSystemAppletProxy (root command 100). */
        if (request.command_id == 100 && !IsOpenSystemAppletProxyRequest(request)) {
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details),
                "skip100 o=%08X in=%u pid=%u c=%u m=%u n=%u s=%u",
                request.object_id,
                static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                request.send_pid ? 1u : 0u,
                request.num_copy_handles,
                request.num_move_handles,
                response.num_out_objects,
                static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletAeCommand100Rejected, qlaunch, 0, response.result, details);
            return;
        }

        if (IsOpenSystemAppletProxyRequest(request)) {
            const u32 proxy_object_id = ReadObjectOnlyResponse(response);
            state->root_object_id = request.object_id;
            state->system_applet_proxy_object_id = proxy_object_id;
            state->application_creator_object_id = 0;
            state->accessor_count = 0;
            RecordObjectResult(usb_storage::AppletAeProxyResult, qlaunch, "ae-proxy", request, response, proxy_object_id);
            return;
        }

        /* ISystemAppletProxy::GetApplicationCreator (command 22). */
        if (request.object_id == state->system_applet_proxy_object_id && request.command_id == 22) {
            const u32 creator_object_id = ReadObjectOnlyResponse(response);
            state->application_creator_object_id = creator_object_id;
            RecordObjectResult(usb_storage::AppletApplicationCreatorResult, qlaunch, "app-creator", request, response, creator_object_id);
            return;
        }

        /* IApplicationCreator::CreateApplication (command 0). */
        if (request.object_id == state->application_creator_object_id && request.command_id == 0) {
            const ncm::ProgramId application_id = ReadProgramId(request);
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details), "create c=0 creator=%u passive=1", request.object_id);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletCreateApplicationAttempt, application_id, 0, ResultSuccess(), details);

            const u32 accessor_object_id = ReadObjectOnlyResponse(response);
            StoreAccessor(state, accessor_object_id, application_id);
            util::SNPrintf(details, sizeof(details), "create c=0 creator=%u accessor=%u passive=1", request.object_id, accessor_object_id);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletCreateApplicationResult, application_id, 0, response.result, details);
            if (R_SUCCEEDED(response.result) && accessor_object_id != 0) {
                ArmNcmTrace(state, accessor_object_id, application_id, "create");
            }
            return;
        }

        /* Keep the alternate queued-launch path visible. Its ApplicationId is
         * learned later from accessor command 120 if qlaunch uses it. */
        if (request.object_id == state->application_creator_object_id && request.command_id == 1) {
            const u32 accessor_object_id = ReadObjectOnlyResponse(response);
            StoreAccessor(state, accessor_object_id, ncm::InvalidProgramId);
            RecordObjectResult(usb_storage::AppletCreateApplicationResult, ncm::InvalidProgramId, "pop-app", request, response, accessor_object_id);
            return;
        }

        /* A qlaunch appletAE domain can already contain its proxy/creator
         * objects when this observer starts seeing forwarded requests. R44's
         * firmware-22.5.0 trace gives an unambiguous recovery signature:
         * command 0, one application-shaped u64 in, one domain object out.
         * Learn application creator/accessor roles from local wire data. Any
         * post-forward ARP update is separately gated on an exact mapped USB
         * Program path and never changes the native appletAE result. */
        if (request.command_id == 0 && request.input_size == sizeof(u64)
                && response.num_out_objects == 1 && R_SUCCEEDED(response.result)) {
            const ncm::ProgramId application_id = ReadProgramId(request);
            const u32 accessor_object_id = ReadObjectOnlyResponse(response);
            if (accessor_object_id != 0 && ncm::IsApplicationId(application_id) && !ncm::IsSystemProgramId(application_id)) {
                state->application_creator_object_id = request.object_id;

                char details[0x80]{};
                util::SNPrintf(details, sizeof(details), "create-inferred c=0 creator=%u passive=1", request.object_id);
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletCreateApplicationAttempt, application_id, 0, ResultSuccess(), details);

                StoreAccessor(state, accessor_object_id, application_id);
                util::SNPrintf(details, sizeof(details), "create-inferred c=0 creator=%u accessor=%u passive=1", request.object_id, accessor_object_id);
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletCreateApplicationResult, application_id, 0, response.result, details);
                ArmNcmTrace(state, accessor_object_id, application_id, "create-inferred");
                return;
            }
        }

        auto *accessor = FindAccessor(state, request.object_id);
        if (accessor == nullptr) {
            if (request.command_id == 0 || request.command_id == 1 || request.command_id == 10
                    || request.command_id == 22 || request.command_id == 30 || request.command_id == 110
                    || request.command_id == 120) {
                const ncm::ProgramId candidate_id = ReadProgramId(request);
                char details[0x80]{};
                util::SNPrintf(details, sizeof(details),
                    "candidate c=%u o=%08X in=%u n=%u s=%u r=%08X id=%016llX",
                    request.command_id,
                    request.object_id,
                    static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                    response.num_out_objects,
                    static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                    ReadObjectOnlyResponse(response),
                    static_cast<unsigned long long>(candidate_id.value));
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletAeCandidateCommand, candidate_id, 0, response.result, details);
            }
            return;
        }

        /* Record every call on the mapped application's accessor, including
         * commands that are not named by the public interface wrappers. This
         * is a passive wire snapshot: request and response are not changed. */
        {
            u64 input0 = 0;
            u64 input8 = 0;
            if (request.input_size >= sizeof(input0)) {
                std::memcpy(std::addressof(input0), request.input, sizeof(input0));
            }
            if (request.input_size >= 2 * sizeof(input8)) {
                std::memcpy(std::addressof(input8), request.input + sizeof(input8), sizeof(input8));
            }
            char details[0xC0]{};
            util::SNPrintf(details, sizeof(details),
                "accessor-raw c=%u o=%08X in=%u out=%u n=%u h=%u/%u i=%016llX/%016llX w=%08X/%08X/%08X/%08X",
                request.command_id,
                request.object_id,
                static_cast<u32>(std::min(request.input_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                static_cast<u32>(std::min(response.output_size, static_cast<size_t>(std::numeric_limits<u32>::max()))),
                response.num_out_objects,
                response.num_copy_handles,
                response.num_move_handles,
                static_cast<unsigned long long>(input0),
                static_cast<unsigned long long>(input8),
                ReadResponseOutputWord(response, 0),
                ReadResponseOutputWord(response, 1),
                ReadResponseOutputWord(response, 2),
                ReadResponseOutputWord(response, 3));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletAccessorRawCommand,
                accessor->application_id, 0, response.result, details);
        }

        /* IApplicationAccessor::GetApplicationId (command 120). */
        if (request.command_id == 120 && R_SUCCEEDED(response.result) && response.output_size >= sizeof(u64)) {
            std::memcpy(std::addressof(accessor->application_id.value), response.output, sizeof(accessor->application_id.value));
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details), "accessor-id c=120 obj=%u id=%016llX", request.object_id, static_cast<unsigned long long>(accessor->application_id.value));
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletAccessorCommandResult, accessor->application_id, 0, response.result, details);
            ArmNcmTrace(state, request.object_id, accessor->application_id, "accessor-id");
            return;
        }

        /* IApplicationAccessor::Start (command 10) is the last qlaunch-side
         * boundary before AM hands the title to PGL/PM. This observer is
         * deliberately read-only: preserve the native request/result and do
         * not queue ARP work or wait on qlaunch's forwarding path. */
        if (request.command_id == 10) {
            /* This service's ShouldMitm accepts Qlaunch only. No client query or IPC here. */
            usb_storage::hang_capture::NotifyStart(ncm::SystemAppletId::Qlaunch, accessor->application_id, response.result);
            ArmNcmTrace(state, request.object_id, accessor->application_id, "start-fallback");
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details), "start c=10 accessor=%u passive=1", request.object_id);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletApplicationStartAttempt, accessor->application_id, 0, ResultSuccess(), details);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletApplicationStartResult, accessor->application_id, 0, response.result, details);
            return;
        }

        /* Start is asynchronous. qlaunch receives the actual application exit
         * or launch failure from IApplicationAccessor::GetResult (command 30)
         * after the state-changed event. Preserve that native Result verbatim. */
        if (request.command_id == 30) {
            const ncm::ProgramId application_id = accessor->application_id;
            char details[0x80]{};
            util::SNPrintf(details, sizeof(details), "get-result c=30 accessor=%u passive=1", request.object_id);
            usb_storage::RecordLaunchBoundaryTrace(usb_storage::AppletApplicationGetResult, application_id, 0, response.result, details);
            if (accessor->ncm_trace_armed) {
                const Result trace_result = EndNcmLaunchTrace(application_id, response.result);
                util::SNPrintf(details, sizeof(details), "ncm-end accessor=%u launch=%08X",
                    request.object_id, response.result.GetValue());
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::NcmLaunchTraceEnd,
                    application_id, 0, trace_result, details);
                accessor->ncm_trace_armed = false;
            }
            usb_storage::ClearUsbLaunchActiveForDiagnostics(application_id);
            usb_storage::ClearLaunchTraceActiveForDiagnostics(application_id);
            RemoveAccessor(state, request.object_id);
        }
    }

}
