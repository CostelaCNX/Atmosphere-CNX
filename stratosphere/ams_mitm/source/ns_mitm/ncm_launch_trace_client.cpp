/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#include <stratosphere.hpp>

#include "../usb_mitm/usbmitm_module.hpp"
#include "ncm_launch_trace_client.hpp"

namespace ams::mitm::ns {

    namespace {

        enum class RequestType : uintptr_t {
            Begin = 1,
            End   = 2,
        };

        struct TraceRequest {
            RequestType type;
            ncm::ProgramId application_id;
            u32 launch_result;
            u32 result;
        };

        constinit os::SdkMutex g_worker_start_mutex;
        constinit os::SdkMutex g_request_mutex;
        bool g_worker_started{};

        uintptr_t g_worker_queue_storage[2]{};
        os::MessageQueue g_worker_queue(g_worker_queue_storage, util::size(g_worker_queue_storage));
        os::Event g_request_complete(os::EventClearMode_AutoClear);
        TraceRequest g_request{};

        constexpr size_t WorkerStackSize = 0x4000;
        alignas(os::ThreadStackAlignment) u8 g_worker_stack[WorkerStackSize]{};
        os::ThreadType g_worker_thread{};

        u64 g_active_session{};
        ncm::ProgramId g_active_application_id{};
        u32 g_active_begin_result{usb::ResultNotInitialized().GetValue()};
        usb_storage::ncm_trace::LookupSnapshot g_lookup_snapshot{};

        Result OpenNcmService(Service *out) {
            R_UNLESS(out != nullptr, fs::ResultNullptrArgument());

            os::NativeHandle handle{};
            R_TRY(sm::GetServiceHandle(std::addressof(handle), sm::ServiceName::Encode("ncm")));
            serviceCreate(out, handle);
            R_SUCCEED();
        }

        Result BeginCapture(ncm::ProgramId application_id) {
            g_active_session = 0;
            g_active_application_id = ncm::InvalidProgramId;
            g_active_begin_result = usb::ResultNotInitialized().GetValue();

            Service service{};
            const Result open_result = OpenNcmService(std::addressof(service));
            if (R_FAILED(open_result)) {
                g_active_begin_result = open_result.GetValue();
                R_RETURN(open_result);
            }
            ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };

            u64 session = 0;
            const Result result = serviceDispatchInOut(std::addressof(service),
                usb_storage::ncm_trace::BeginTraceCommand, application_id.value, session);
            if (R_SUCCEEDED(result)) {
                g_active_session = session;
                g_active_application_id = application_id;
            }
            g_active_begin_result = result.GetValue();
            R_RETURN(result);
        }

        Result EndCapture(ncm::ProgramId application_id, Result launch_result) {
            std::memset(std::addressof(g_lookup_snapshot), 0, sizeof(g_lookup_snapshot));

            Result end_result = ncm::ResultContentNotFound();
            const u64 session = g_active_session;
            const u32 begin_result = g_active_begin_result;
            if (session != 0 && g_active_application_id == application_id) {
                Service service{};
                end_result = OpenNcmService(std::addressof(service));
                if (R_SUCCEEDED(end_result)) {
                    ON_SCOPE_EXIT { serviceClose(std::addressof(service)); };
                    end_result = serviceDispatchIn(std::addressof(service),
                        usb_storage::ncm_trace::EndTraceCommand, session,
                        .buffer_attrs = {
                            SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                        },
                        .buffers = {
                            { std::addressof(g_lookup_snapshot), sizeof(g_lookup_snapshot) },
                        },
                    );
                }
            }

            usb_storage::StoreNcmLaunchTraceForDiagnostics(g_lookup_snapshot,
                application_id, session, static_cast<Result>(begin_result),
                end_result, launch_result);

            g_active_session = 0;
            g_active_application_id = ncm::InvalidProgramId;
            g_active_begin_result = usb::ResultNotInitialized().GetValue();
            R_RETURN(end_result);
        }

        void WorkerThreadFunction(void *) {
            while (true) {
                uintptr_t raw_type = 0;
                g_worker_queue.Receive(std::addressof(raw_type));

                const auto type = static_cast<RequestType>(raw_type);
                Result result = fs::ResultInvalidArgument();
                if (type == RequestType::Begin) {
                    result = BeginCapture(g_request.application_id);
                } else if (type == RequestType::End) {
                    result = EndCapture(g_request.application_id,
                        static_cast<Result>(g_request.launch_result));
                }

                g_request.result = result.GetValue();
                g_request_complete.Signal();
            }
        }

        Result SubmitRequest(RequestType type, ncm::ProgramId application_id, Result launch_result) {
            R_UNLESS(ncm::IsApplicationId(application_id) && !ncm::IsSystemProgramId(application_id), fs::ResultInvalidArgument());

            std::scoped_lock request_lk(g_request_mutex);
            {
                std::scoped_lock start_lk(g_worker_start_mutex);
                R_UNLESS(g_worker_started, usb::ResultNotInitialized());
            }

            g_request.type = type;
            g_request.application_id = application_id;
            g_request.launch_result = launch_result.GetValue();
            g_request.result = usb::ResultNotInitialized().GetValue();

            g_worker_queue.Send(static_cast<uintptr_t>(type));
            g_request_complete.Wait();
            R_RETURN(static_cast<Result>(g_request.result));
        }

    }

    Result StartNcmLaunchTraceWorker() {
        std::scoped_lock lk(g_worker_start_mutex);
        R_SUCCEED_IF(g_worker_started);

        R_TRY(os::CreateThread(std::addressof(g_worker_thread), WorkerThreadFunction, nullptr,
            g_worker_stack, sizeof(g_worker_stack), os::GetThreadCurrentPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_worker_thread), "ams_mitm.NcmTrace");
        os::StartThread(std::addressof(g_worker_thread));
        g_worker_started = true;
        R_SUCCEED();
    }

    Result BeginNcmLaunchTrace(ncm::ProgramId application_id) {
        R_RETURN(SubmitRequest(RequestType::Begin, application_id, ResultSuccess()));
    }

    Result EndNcmLaunchTrace(ncm::ProgramId application_id, Result launch_result) {
        R_RETURN(SubmitRequest(RequestType::End, application_id, launch_result));
    }

}
