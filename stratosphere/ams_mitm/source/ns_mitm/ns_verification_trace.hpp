/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::ns {
    struct VerifyApplicationRequest {
        u32 flags;
        u32 padding;
        u64 application_id;
        u64 transfer_memory_size;
    };
    static_assert(util::is_pod<VerifyApplicationRequest>::value);
    static_assert(sizeof(VerifyApplicationRequest) == 24);
    static_assert(offsetof(VerifyApplicationRequest, application_id) == 8);
    static_assert(offsetof(VerifyApplicationRequest, transfer_memory_size) == 16);
}

/* All other progress-object commands (including Cancel, GetProgress and
 * GetErrorContext) remain raw forwarded with their buffers untouched. */
#define AMS_NS_VERIFY_RESULT_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, Get, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetDetailResult, (), ())
AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::ns::impl, IVerifyResultMitm, AMS_NS_VERIFY_RESULT_INFO, 0xF6D430A1)

#define AMS_NS_VERIFY_REQUEST_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 1003, Result, RequestVerifyApplication, \
        (sf::OutCopyHandle event, sf::Out<sf::SharedPointer<mitm::ns::impl::IVerifyResultMitm>> out, sf::CopyHandle &&memory, const mitm::ns::VerifyApplicationRequest &request), \
        (event, out, std::move(memory), request), hos::Version_5_0_0)

namespace ams::mitm::ns {
    bool GetVerificationOpenContext(u64 &title, u64 &token, bool &trace);
    Result ForwardVerifyApplication(std::shared_ptr<Service> service,
        const sm::MitmProcessInfo &client, sf::OutCopyHandle event,
        sf::Out<sf::SharedPointer<impl::IVerifyResultMitm>> out,
        sf::CopyHandle &&memory, const VerifyApplicationRequest &request);

    /* In-memory bounded observer; no IPC, SD operations, buffers or routing. */
    void RecordVerificationFs(ncm::ProgramId client, u32 command, u32 object,
        u32 input_size, u32 output_size, Result result);
}
