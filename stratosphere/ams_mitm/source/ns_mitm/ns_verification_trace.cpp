/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "ns_verification_trace.hpp"
#include "ns_verification_state.hpp"
#include "../usb_mitm/usb_launch_trace.hpp"

namespace ams::mitm::ns {
    namespace {
        constinit os::SdkMutex g_window_lock;
        verification::Window g_window;
        u64 NowMs() { return static_cast<u64>(os::GetSystemTick().ToTimeSpan().GetMilliSeconds()); }
        void Record(u32 stage, ncm::ProgramId id, Result result, const char *text) {
            usb_storage::RecordLaunchBoundaryTrace(
                static_cast<usb::content::LoaderProbeStage>(stage), id, 0, result, text);
        }

        class VerifyResultService : public sf::MitmServiceImplBase {
            ncm::ProgramId m_title{};
            u64 m_token{};
            bool m_detail_read{};
            unsigned m_records{};
            os::SdkMutex m_lock;
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            void Configure(ncm::ProgramId title, u64 token) { m_title = title; m_token = token; }
            ~VerifyResultService() {
                std::scoped_lock lock(g_window_lock);
                g_window.End(m_token);
            }
            Result Read(u32 command) {
                std::scoped_lock lock(m_lock);
                const u32 result = verification::ReadResult(command, m_detail_read,
                    [&](u32 cmd) -> u32 { return serviceDispatch(m_forward_service.get(), cmd); },
                    [&](u32 cmd, u32 value, bool diagnostic) {
                        if (m_records >= 8) return;
                        ++m_records;
                        char detail[0x80]{};
                        util::SNPrintf(detail, sizeof(detail),
                            "P6 verify-result session=%llu c=%u diagnostic=%u native=%08X",
                            static_cast<unsigned long long>(m_token), cmd, diagnostic ? 1u : 0u, value);
                        Record(cmd == 0 ? 226 : 227, m_title, static_cast<Result>(value), detail);
                    });
                R_RETURN(static_cast<Result>(result));
            }
            Result Get() { R_RETURN(Read(0)); }
            Result GetDetailResult() { R_RETURN(Read(3)); }
        };
        static_assert(impl::IsIVerifyResultMitm<VerifyResultService>);
    }

    bool GetVerificationOpenContext(u64 &title, u64 &token, bool &trace) {
        std::scoped_lock lock(g_window_lock);
        const auto now = NowMs();
        if (!g_window.Peek(now, title, token)) return false;
        trace = g_window.TakeOpen(now);
        return true; // Diagnostic quota never changes routing.
    }

    Result ForwardVerifyApplication(std::shared_ptr<Service> service,
        const sm::MitmProcessInfo &client, sf::OutCopyHandle event,
        sf::Out<sf::SharedPointer<impl::IVerifyResultMitm>> out,
        sf::CopyHandle &&memory, const VerifyApplicationRequest &request) {
        /* Only the HOME/AM verification path is instrumented. No drive reads
         * are needed to arm this window; SD-only tests can serve as controls. */
        if (!((client.program_id == ncm::SystemAppletId::Qlaunch
                  || client.program_id == ncm::SystemProgramId::Am)
            && ncm::IsApplicationId(ncm::ProgramId{request.application_id})
            && !ncm::IsSystemProgramId(ncm::ProgramId{request.application_id}))) {
            /* Raw forwarding restores the saved request and owns its copy
             * handles. Do not close this one when InHandleHolder unwinds. */
            memory.Detach();
            R_RETURN(sm::mitm::ResultShouldForwardToSession());
        }

        /* Allocate before starting the native async task. Own the exact native
         * event and child; never substitute the event or manufacture success. */
        auto forward = sf::hipc::ServerSession::CreateForwardService();
        auto object_forward = forward;
        auto object = sf::CreateSharedObjectEmplaced<impl::IVerifyResultMitm, VerifyResultService>(
            std::move(object_forward), client);
        if (object == nullptr) {
            /* An optional observer allocation must not stop native checking. */
            memory.Detach();
            R_RETURN(sm::mitm::ResultShouldForwardToSession());
        }
        const ncm::ProgramId title{request.application_id};
        u64 token;
        { std::scoped_lock lock(g_window_lock); token = g_window.Begin(title.value, NowMs()); }
        object.GetImpl().Configure(title, token);

        Handle native_event = INVALID_HANDLE;
        const Result result = serviceDispatchIn(service.get(), 1003, request,
            .in_num_handles = 1,
            .in_handles = {memory.GetOsHandle()},
            .out_num_objects = 1,
            .out_objects = forward.get(),
            .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
            .out_handles = std::addressof(native_event));
        sf::CopyHandle owned_event(native_event, native_event != INVALID_HANDLE);
        char detail[0x80]{};
        util::SNPrintf(detail, sizeof(detail), "P6 verify-start session=%llu flags=%08X tmem=%llu native=%08X",
            static_cast<unsigned long long>(token), request.flags,
            static_cast<unsigned long long>(request.transfer_memory_size), result.GetValue());
        Record(229, title, result, detail);
        R_TRY(result);

        const sf::cmif::DomainObjectId object_id{serviceGetObjectId(forward.get())};
        if (object_id == sf::cmif::InvalidDomainObjectId) {
            out.SetMitmValue(std::move(object), std::move(forward));
        } else {
            out.SetValue(std::move(object), object_id);
        }
        event.SetValue(std::move(owned_event));
        R_SUCCEED();
    }

    void RecordVerificationFs(ncm::ProgramId client, u32 command, u32 object,
        u32 input_size, u32 output_size, Result result) {
        if (client != ncm::SystemProgramId::Ns) return;
        u64 title{}, token{};
        {
            std::scoped_lock lock(g_window_lock);
            if (!g_window.Take(NowMs(), R_FAILED(result), title, token)) return;
        }
        char detail[0x80]{};
        util::SNPrintf(detail, sizeof(detail),
            "P6 verify-fs session=%llu c=%u object=%08X in=%u out=%u native=%08X",
            static_cast<unsigned long long>(token), command, object, input_size, output_size, result.GetValue());
        Record(228, ncm::ProgramId{title}, result, detail);
    }
}
