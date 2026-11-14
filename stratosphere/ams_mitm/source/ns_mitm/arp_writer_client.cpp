/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <stratosphere.hpp>

#include "../usb_mitm/usb_launch_trace.hpp"
#include "arp_reader_mitm_service.hpp"
#include "arp_writer_client.hpp"

namespace ams::mitm::arp {

    namespace {

        constexpr size_t MaxApplicationInstances = 32;
        constexpr size_t ArpWriterQueueCapacity = 8;
        constexpr size_t ArpWriterMaxAttempts = 8;
        constexpr TimeSpan ArpWriterRetryDelay = TimeSpan::FromMilliSeconds(50);

        constinit os::SdkMutex g_worker_start_mutex;
        constinit bool g_worker_started = false;
        constinit uintptr_t g_worker_queue_storage[ArpWriterQueueCapacity];
        os::MessageQueue g_worker_queue(g_worker_queue_storage, ArpWriterQueueCapacity);

        /* The appletAE callback must not call arp:r/arp:w directly. Keep a
         * tiny completion record so it can wait for the already-isolated
         * worker after native Start has returned. One active application
         * launch is expected from qlaunch; a new queue item replaces a failed
         * item, but never duplicates a successful one. */
        constinit os::SdkMutex g_completion_mutex;
        constinit os::SdkConditionVariable g_completion_cv;
        u64 g_completion_application_id{};
        u32 g_completion_result{};
        bool g_completion_pending{};
        bool g_completion_done{};

        constexpr size_t ArpWriterWorkerStackSize = 0x8000;
        alignas(os::ThreadStackAlignment) constinit u8 g_worker_stack[ArpWriterWorkerStackSize];
        constinit os::ThreadType g_worker_thread;

        Result ListApplicationInstanceIds(Service *reader, u64 *out_ids, s32 *out_count) {
            R_UNLESS(reader != nullptr && out_ids != nullptr && out_count != nullptr, fs::ResultNullptrArgument());

            R_RETURN(serviceDispatchOut(reader, 5, *out_count,
                .buffer_attrs = {
                    SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                },
                .buffers = {
                    { out_ids, MaxApplicationInstances * sizeof(*out_ids) },
                },
            ));
        }

        Result GetApplicationLaunchProperty(Service *reader, ApplicationLaunchProperty *out, u64 instance_id) {
            R_UNLESS(reader != nullptr && out != nullptr, fs::ResultNullptrArgument());
            R_RETURN(serviceDispatchInOut(reader, 0, instance_id, *out));
        }

        Result RegisterApplicationControlProperty(Service *writer, const ApplicationControlProperty *control, u64 instance_id) {
            R_UNLESS(writer != nullptr && control != nullptr, fs::ResultNullptrArgument());

            /* arp:w command 4 is the post-19.0.0 path. Its 0x4000-byte
             * ApplicationControlProperty is a type-0x15 fixed map-alias input
             * buffer (In | HipcMapAlias | FixedSize). Using HipcPointer here
             * makes the kernel copy the 16 KiB payload through the service's
             * pointer buffer; on Horizon 22.5.0 that overflows the available
             * pointer-buffer resource and returns 0xCE01. */
            R_RETURN(serviceDispatchIn(writer, 4, instance_id,
                .buffer_attrs = {
                    SfBufferAttr_HipcMapAlias | SfBufferAttr_In | SfBufferAttr_FixedSize,
                },
                .buffers = {
                    { control, sizeof(*control) },
                },
            ));
        }

    }

    Result RegisterUsbApplicationControlProperty(u64 *out_instance_id, ncm::ProgramId application_id) {
        R_UNLESS(out_instance_id != nullptr, fs::ResultNullptrArgument());
        *out_instance_id = 0;
        R_UNLESS(hos::GetVersion() >= hos::Version_19_0_0, svc::ResultNotImplemented());
        R_UNLESS(ncm::IsApplicationId(application_id) && !ncm::IsSystemProgramId(application_id), fs::ResultInvalidArgument());
        R_UNLESS(usb_storage::IsUsbContentLaunchPathEnabled(), usb::ResultOperationDenied());
        R_UNLESS(usb_storage::HasUsbProgramPath(application_id), fs::ResultPathNotFound());

        char details[0x80]{};
        Service reader{};
        const Result reader_result = smGetService(std::addressof(reader), "arp:r");
        util::SNPrintf(details, sizeof(details), "sm open arp:r");
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpReaderServiceOpen, application_id, 0, reader_result, details);
        R_TRY(reader_result);
        ON_SCOPE_EXIT { serviceClose(std::addressof(reader)); };

        std::array<u64, MaxApplicationInstances> instance_ids{};
        s32 instance_count = 0;
        const Result list_result = ListApplicationInstanceIds(std::addressof(reader), instance_ids.data(), std::addressof(instance_count));
        util::SNPrintf(details, sizeof(details), "arp:r c=5 count=%d max=%u", instance_count, static_cast<u32>(instance_ids.size()));
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpReaderListInstances, application_id, 0, list_result, details);
        R_TRY(list_result);

        const size_t bounded_count = static_cast<size_t>(std::clamp(instance_count, 0, static_cast<s32>(instance_ids.size())));
        u64 matched_instance_id = 0;
        ApplicationLaunchProperty matched_launch_property{};
        for (size_t i = 0; i < bounded_count; ++i) {
            ApplicationLaunchProperty launch_property{};
            const Result lookup_result = GetApplicationLaunchProperty(std::addressof(reader), std::addressof(launch_property), instance_ids[i]);
            if (R_SUCCEEDED(lookup_result) && launch_property.application_id == application_id) {
                matched_instance_id = instance_ids[i];
                matched_launch_property = launch_property;
                break;
            }
        }

        util::SNPrintf(details, sizeof(details), "arp:r match iid=%016llX v=%u s=%u p=%u k=%u",
            static_cast<unsigned long long>(matched_instance_id), matched_launch_property.version,
            matched_launch_property.storage_id, matched_launch_property.patch_storage_id,
            matched_launch_property.application_kind);
        Result match_result = ncm::ResultContentNotFound();
        if (matched_instance_id != 0) {
            match_result = ResultSuccess();
        }
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpReaderMatchInstance, application_id, 0, match_result, details);
        R_UNLESS(matched_instance_id != 0, ncm::ResultContentNotFound());

        auto control = std::make_unique<ApplicationControlProperty>();
        const Result control_result = ReadUsbApplicationControlProperty(control.get(), application_id);
        util::SNPrintf(details, sizeof(details), "USB NACP iid=%016llX bytes=%u",
            static_cast<unsigned long long>(matched_instance_id), static_cast<u32>(sizeof(*control)));
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpWriterReadControl, application_id,
            static_cast<u8>(ncm::ContentType::Control), control_result, details);
        R_TRY(control_result);

        Service writer{};
        const Result writer_result = smGetService(std::addressof(writer), "arp:w");
        util::SNPrintf(details, sizeof(details), "sm open arp:w");
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpWriterServiceOpen, application_id,
            static_cast<u8>(ncm::ContentType::Control), writer_result, details);
        R_TRY(writer_result);
        ON_SCOPE_EXIT { serviceClose(std::addressof(writer)); };

        const Result register_result = RegisterApplicationControlProperty(std::addressof(writer), control.get(), matched_instance_id);
        util::SNPrintf(details, sizeof(details), "arp:w c=4 iid=%016llX bytes=%u",
            static_cast<unsigned long long>(matched_instance_id), static_cast<u32>(sizeof(*control)));
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpWriterRegisterControl, application_id,
            static_cast<u8>(ncm::ContentType::Control), register_result, details);
        R_TRY(register_result);

        *out_instance_id = matched_instance_id;
        R_SUCCEED();
    }

    namespace {

        void ArpWriterWorkerThreadFunction(void *) {
            while (true) {
                uintptr_t queued_application_id = 0;
                g_worker_queue.Receive(std::addressof(queued_application_id));

                const ncm::ProgramId application_id{static_cast<u64>(queued_application_id)};
                if (!ncm::IsApplicationId(application_id) || ncm::IsSystemProgramId(application_id)) {
                    continue;
                }

                const Result content_not_found = ncm::ResultContentNotFound();
                Result result = content_not_found;
                u64 instance_id = 0;
                size_t attempts = 0;
                /* Unmapped SD/NAND applications are queued by the passive
                 * observer too. Do not spend the content-not-found retry
                 * budget on those ordinary launches; only a durable USB map
                 * is eligible for the bounded ARP retry window. */
                if (ncm::IsUsbTitleMapped(application_id)) {
                    for (; attempts < ArpWriterMaxAttempts; ++attempts) {
                        if (attempts != 0) {
                            os::SleepThread(ArpWriterRetryDelay);
                        }

                        result = RegisterUsbApplicationControlProperty(std::addressof(instance_id), application_id);
                        if (R_SUCCEEDED(result) || result.GetValue() != content_not_found.GetValue()) {
                            ++attempts;
                            break;
                        }
                    }
                } else {
                    attempts = 1;
                }

                char details[0x80]{};
                util::SNPrintf(details, sizeof(details),
                    "worker complete attempts=%u iid=%016llX result=%08X",
                    static_cast<u32>(attempts), static_cast<unsigned long long>(instance_id), result.GetValue());
                usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpWriterWorkerComplete, application_id, 0, result, details);

                {
                    std::scoped_lock lk(g_completion_mutex);
                    if (g_completion_application_id == application_id.value && g_completion_pending) {
                        g_completion_result = result.GetValue();
                        g_completion_pending = false;
                        g_completion_done = true;
                        g_completion_cv.Broadcast();
                    }
                }
            }
        }

    }

    Result StartArpWriterWorker() {
        std::scoped_lock lk(g_worker_start_mutex);
        if (g_worker_started) {
            R_SUCCEED();
        }

        const Result create_result = os::CreateThread(std::addressof(g_worker_thread), ArpWriterWorkerThreadFunction, nullptr,
            g_worker_stack, sizeof(g_worker_stack), os::GetThreadCurrentPriority(os::GetCurrentThread()));
        R_TRY(create_result);

        os::StartThread(std::addressof(g_worker_thread));
        g_worker_started = true;
        R_SUCCEED();
    }

    Result QueueUsbApplicationControlProperty(ncm::ProgramId application_id) {
        R_UNLESS(ncm::IsApplicationId(application_id) && !ncm::IsSystemProgramId(application_id), fs::ResultInvalidArgument());

        std::scoped_lock lk(g_worker_start_mutex);
        R_UNLESS(g_worker_started, usb::ResultNotInitialized());

        {
            std::scoped_lock completion_lk(g_completion_mutex);
            if (g_completion_application_id == application_id.value) {
                if (g_completion_pending || (g_completion_done && g_completion_result == ResultSuccess().GetValue())) {
                    /* CreateApplication and a later accessor discovery may
                     * describe the same title. Do not issue a second arp:w
                     * registration for a successful or in-flight item. */
                    R_SUCCEED();
                }
            }

            g_completion_application_id = application_id.value;
            g_completion_result = ncm::ResultContentNotFound().GetValue();
            g_completion_pending = true;
            g_completion_done = false;
        }

        const bool queued = g_worker_queue.TrySend(static_cast<uintptr_t>(application_id.value));
        const Result queue_result = queued ? static_cast<Result>(ResultSuccess())
                                           : static_cast<Result>(usb::ResultOperationDenied());
        if (!queued) {
            std::scoped_lock completion_lk(g_completion_mutex);
            if (g_completion_application_id == application_id.value) {
                g_completion_result = queue_result.GetValue();
                g_completion_pending = false;
                g_completion_done = true;
                g_completion_cv.Broadcast();
            }
        }
        char details[0x80]{};
        util::SNPrintf(details, sizeof(details), "worker queue queued=%u", queued ? 1u : 0u);
        usb_storage::RecordLaunchBoundaryTrace(usb_storage::ArpWriterWorkerQueued, application_id, 0, queue_result, details);
        R_RETURN(queue_result);
    }

    Result WaitForUsbApplicationControlProperty(ncm::ProgramId application_id, TimeSpan timeout) {
        R_UNLESS(ncm::IsApplicationId(application_id) && !ncm::IsSystemProgramId(application_id), fs::ResultInvalidArgument());
        R_UNLESS(timeout.GetNanoSeconds() >= 0, fs::ResultInvalidArgument());

        std::scoped_lock lk(g_completion_mutex);
        R_UNLESS(g_completion_application_id == application_id.value
                 && (g_completion_pending || g_completion_done), ncm::ResultContentNotFound());

        if (g_completion_pending) {
            if (!g_completion_cv.TimedWait(g_completion_mutex, timeout)) {
                /* The native Start result remains authoritative. A timeout
                 * only tells diagnostics that ARP was not ready in time. */
                R_RETURN(ncm::ResultContentNotFound());
            }
        }

        R_RETURN(static_cast<Result>(g_completion_result));
    }

}
