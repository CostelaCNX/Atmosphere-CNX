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
#include <stratosphere/usb/usb_runtime_diagnostics.hpp>
#include <stratosphere/usb/usb_io_diagnostics.hpp>
#include <stratosphere/usb/usb_pause_diagnostics.hpp>
#include <stratosphere/usb/usb_startup_io_diagnostics.hpp>
#include <stratosphere/usb/usb_keep_alive_test_core.hpp>
#include <stratosphere/usb/usb_write_transfer.hpp>
#include <stratosphere/usb/usb_completion_probe.hpp>
#include <stratosphere/usb/usb_host_write.hpp>
#include <stratosphere/usb/usb_reader_startup_profile.hpp>
#include <stratosphere/usb/usb_interface_recovery.hpp>
#include <stratosphere/usb/usb_sleep_diagnostics.hpp>
#include "usb_keep_alive.hpp"
#include "usb_startup.hpp"

#if defined(ATMOSPHERE_OS_HORIZON)
#include "usb_reader_discovery.hpp"

namespace ams::usb::storage {

    namespace {

        constexpr TimeSpan WorkerPollInterval = TimeSpan::FromMilliSeconds(500);
        /* How long a failed eject stays visible to the panel before remounting. */
        constexpr u64 EjectFailureHoldNs = 5000000000ULL;
        /* The public benchmark route targets one storage Service. Keep this
         * state outside Service so existing object layout stays unchanged.
         * Other Service instances cannot inherit this instance's suspension. */
        os::SdkMutex g_keep_alive_test_mutex;
        keep_alive_test::Lease g_keep_alive_test_lease;
        const Service *g_keep_alive_test_service{};


        bool ShouldDisconnectAfterIo(Result result) {
            /* Filesystem contention, access/capacity refusals and caller
             * mistakes are NOT a broken transport. A stale handle already
             * has a changed generation; do not disconnect the new media too. */
            return R_FAILED(result)
                && !usb::ResultFileNotFound::Includes(result)
                && !usb::ResultAlreadyInitialized::Includes(result)
                && !usb::ResultResourceBusy::Includes(result)
                && !usb::ResultOperationDenied::Includes(result)
                && !usb::ResultInvalidParameter::Includes(result)
                && !usb::ResultMemAllocFailure::Includes(result)
                && !usb::ResultMediaChanged::Includes(result);
        }

    }

    Service::Service(bool read_only) : m_read_only(read_only) {
        m_status.state = State::Stopped;
        m_status.last_io_result = ResultSuccess().GetValue();
        m_status.interface_id = -1;
    }

    Service::~Service() {
        if (this->IsRunning()) {
            static_cast<void>(this->Stop());
        }
    }

    Result Service::BeginSystemOperation() {
        std::unique_lock lk(m_power_mutex);
        for (;;) {
            const auto admission = m_power_gate.Enter();
            if (admission == sleep::Admission::Entered) R_SUCCEED();
            R_UNLESS(admission != sleep::Admission::Rejected, usb::ResultNotInitialized());
            // Park only this caller. No volume/state lock is held, and Stop
            // and failed power coordination broadcast to release all waiters.
            m_power_cv.Wait(m_power_mutex);
        }
    }

    void Service::EndSystemOperation() {
        std::scoped_lock lk(m_power_mutex);
        const bool left = m_power_gate.Leave(); AMS_ASSERT(left); AMS_UNUSED(left);
        m_power_cv.Broadcast();
    }

    bool Service::IsSystemPowerPaused() const {
        std::scoped_lock lk(m_power_mutex);
        return m_power_gate.Paused();
    }

    bool Service::IsSystemPowerQuiescent() const {
        std::scoped_lock state_lk(m_mutex);
        std::scoped_lock lk(m_power_mutex);
        const auto phase = m_power_gate.GetPhase();
        return !(m_running && m_stop_requested) && !m_resume_in_progress && m_power_gate.Active() == 0 &&
            (phase == sleep::Phase::Suspended || phase == sleep::Phase::Failed || phase == sleep::Phase::Stopped);
    }

    void Service::FailSystemPower(Result result) {
        std::scoped_lock lk(m_power_mutex);
        m_power_gate.Fail();
        sleep_diagnostics::phase.store(static_cast<u32>(m_power_gate.GetPhase()));
        sleep_diagnostics::last_result.store(result.GetValue());
        sleep_diagnostics::failures.fetch_add(1);
        // Do not touch active endpoints or cached filesystem objects here.
        // The watcher must not acknowledge until actual quiescence.
        m_power_cv.Broadcast();
    }

    Result Service::CaptureRetainedInterface(host::Interface *out, u64 deadline) {
        for (;;) {
            s32 count = 0;
            const auto rc=m_client.QueryAcquiredInterfaces(m_acquired_interfaces,sizeof(m_acquired_interfaces),&count);
            R_UNLESS(count >= 0 && count <= static_cast<s32>(MaxInterfaceCount),usb::ResultInternalStateError());
            if (R_SUCCEEDED(rc)) {
                for (s32 i=0;i<count;++i) {
                    if (m_acquired_interfaces[i].inf.ID == m_device.GetInterfaceId()) {
                        *out = m_acquired_interfaces[i]; R_SUCCEED();
                    }
                }
            }
            R_UNLESS(!svc::ResultSessionClosed::Includes(rc),rc);
            const u64 now=impl::UsbNowNs();
            if (!deadline || now >= deadline || deadline-now <= 100'000'000ULL) {
                R_TRY(rc); R_THROW(usb::ResultMediaChanged());
            }
            // A short post-wake host restoration window is allowed only for
            // the original still-acquired session. Never acquire any new ID.
            os::SleepThread(TimeSpan::FromMilliSeconds(100));
        }
    }

    // Caller holds the volume lock and either owns an admitted worker slot or
    // has drained admission for sleep. Publish the witness only after all of
    // the writes, cache sync and direct readback have completed.
    Result Service::CaptureSleepWitness(const Status &status, bool preparing) {
        namespace diag = sleep_diagnostics;
        const auto stage = [preparing](diag::PrepareStage step) {
            if (preparing) diag::prepare_stage.store(step);
        };
        m_sleep_marker_armed = false;
        m_sleep_has_serial = false;
        m_sleep_generation = status.media_generation;
        m_sleep_partition_lba = status.partition_first_lba;
        const u64 deadline = sleep::Deadline(impl::UsbNowNs(),sleep::ProbeBudgetNs);
        stage(diag::CaptureInterface);
        R_TRY(CaptureRetainedInterface(&m_sleep_interface));
        m_device.SetSystemPowerDeadline(deadline);
        ON_SCOPE_EXIT { m_device.SetSystemPowerDeadline(0); };
        stage(diag::FlushVolume);
        if (m_writable_volume.IsInitialized()) R_TRY(m_writable_volume.Flush());
        if (m_writable_ready && m_writable_volume.IsInitialized() &&
                (m_fat32_volume.IsInitialized() || m_exfat_volume.IsInitialized())) {
            stage(diag::CaptureSerial);
            const auto index = m_sleep_interface.device_desc.iSerialNumber;
            if (index) {
                const auto serial = m_device.CaptureWakeSerial(m_sleep_serial, index, deadline);
                if (!usb::ResultOperationDenied::Includes(serial)) { R_TRY(serial); m_sleep_has_serial = true; }
            }
            stage(diag::WriteMarker);
            std::memcpy(m_sleep_marker.signature, sleep::MarkerSignature, sizeof(m_sleep_marker.signature));
            ::randomGet(m_sleep_marker.challenge, sizeof(m_sleep_marker.challenge));
            R_TRY(m_writable_volume.WriteSleepMarker(m_sleep_marker, &m_sleep_marker_armed));
        }
        stage(diag::SyncDevice);
        if (m_device.NeedsSystemSleepSync()) R_TRY(m_device.SynchronizeCacheForEject(deadline));
        stage(diag::VerifyMarker);
        if (m_sleep_marker_armed) R_TRY(VerifySleepMarker());
        stage(diag::ReadFingerprint);
        R_RETURN(m_device.CaptureSleepFingerprint(m_sleep_fingerprint, m_sleep_partition_lba, deadline, false));
    }

    void Service::TryArmSleepCheckpoint() {
        std::unique_lock volume_lk(m_volume_mutex, std::try_to_lock);
        if (!volume_lk.owns_lock()) return;
        if (!m_checkpoint_pending || IsSystemPowerPaused()) return;
        m_checkpoint_pending = false;
        m_checkpoint_valid = false;
        Status status{};
        if (R_FAILED(GetStatus(&status)) || status.state != State::Ready) return;
        {
            std::scoped_lock state_lk(m_mutex);
            if (m_stop_requested || m_reconnect_requested) return;
        }
        if (!m_writable_ready || !m_device.IsInitialized() || !m_writable_volume.IsInitialized() ||
                (!m_fat32_volume.IsInitialized() && !m_exfat_volume.IsInitialized())) return;
        const auto result = CaptureSleepWitness(status, false);
        m_checkpoint_valid = R_SUCCEEDED(result) && m_sleep_marker_armed && !m_device.NeedsSystemSleepSync();
        sleep_diagnostics::checkpoint_result.store(result.GetValue());
        if (R_FAILED(result) && (m_device.IsQuarantined() || usb::ResultMediaChanged::Includes(result) ||
                (ShouldDisconnectAfterIo(result) && !usb::ResultScsiCommandFailed::Includes(result)))) {
            // The optional check itself can lose an endpoint. Route that
            // through the normal disconnect path, then let the next mount
            // serve files without automatically retrying the same probe.
            m_skip_checkpoint_on_next_mount = true;
            CompleteIoLocked(usb::ResultTransactionError());
        }
    }

    Result Service::PrepareSystemSleep() {
        // An explicitly stopped service still participates in PSC transitions.
        // Park it as empty only after Stop has completed all worker teardown.
        {
            std::scoped_lock state_lk(m_mutex);
            std::scoped_lock power_lk(m_power_mutex);
            if (m_power_gate.GetPhase() == sleep::Phase::Stopped) {
                R_UNLESS(!m_running && !m_resume_in_progress && m_power_gate.Restart(), usb::ResultResourceBusy());
            }
        }
        {
            std::unique_lock lk(m_power_mutex);
            if (m_power_gate.GetPhase() == sleep::Phase::Suspended) R_SUCCEED();
            if (m_power_gate.GetPhase() == sleep::Phase::Awake) {
                const bool began = m_power_gate.BeginSleep(); AMS_ASSERT(began); AMS_UNUSED(began);
                sleep_diagnostics::prepares.fetch_add(1);
                sleep_diagnostics::last_prepare_ns.store(impl::UsbNowNs());
            }
            R_UNLESS(m_power_gate.GetPhase() == sleep::Phase::Draining || m_power_gate.GetPhase() == sleep::Phase::Failed,usb::ResultNotInitialized());
            sleep_diagnostics::phase.store(static_cast<u32>(m_power_gate.GetPhase()));
            const u64 deadline = sleep::Deadline(impl::UsbNowNs(),sleep::DrainBudgetNs);
            while (m_power_gate.Active()) {
                const u64 now = impl::UsbNowNs();
                // ResourceBusy means NOT quiescent: caller must retry, never
                // acknowledge PSC or free objects while admitted work runs.
                R_UNLESS(now < deadline,usb::ResultResourceBusy());
                m_power_cv.TimedWait(m_power_mutex,TimeSpan::FromNanoSeconds(deadline-now));
            }
            R_UNLESS(m_power_gate.GetPhase() == sleep::Phase::Draining,usb::ResultNotInitialized());
        }
        const auto prepare = [&]() -> Result {
            std::scoped_lock volume_lk(m_volume_mutex);
            m_sleep_had_media = m_device.IsInitialized();
            const bool checkpoint_valid = m_checkpoint_valid;
            m_checkpoint_valid = false;
            m_checkpoint_pending = false;
            sleep_diagnostics::prepare_stage.store(sleep_diagnostics::CheckVolume);
            sleep_diagnostics::checkpoint_used.store(0);
            sleep_diagnostics::marker_armed.store(0);
            sleep_diagnostics::rebind_stage.store(0);
            sleep_diagnostics::rebind_result.store(0);
            sleep_diagnostics::rebind_candidates.store(0);
            sleep_diagnostics::rebind_queries.store(0);
            sleep_diagnostics::rebind_query_result.store(0);
            sleep_diagnostics::rebind_visible_max.store(0);
            sleep_diagnostics::rebind_exact_max.store(0);
            u64 generation{};
            R_TRY(GetMediaGeneration(&generation));
            sleep_diagnostics::generation_before.store(generation);
            if (!m_sleep_had_media) {
                m_sleep_generation = generation;
                m_sleep_marker_armed = false;
                R_SUCCEED();
            }
            Status status{}; R_TRY(GetStatus(&status));
            { std::scoped_lock state_lk(m_mutex); R_UNLESS(status.state == State::Ready && status.media_generation == generation &&
                    !m_stop_requested && !m_reconnect_requested,usb::ResultNotInitialized()); }
            if (checkpoint_valid && m_sleep_generation == generation && m_sleep_marker_armed &&
                    m_writable_ready && !m_device.IsQuarantined() && !m_device.NeedsSystemSleepSync()) {
                // No mutation was attempted since durable readback. The USB
                // host may already be offline by the time PSC delivers sleep.
                sleep_diagnostics::prepare_stage.store(sleep_diagnostics::UseCheckpoint);
                sleep_diagnostics::checkpoint_used.store(1);
            } else {
                R_TRY(CaptureSleepWitness(status, true));
            }
            sleep_diagnostics::marker_armed.store(m_sleep_marker_armed ? 1 : 0);
            R_SUCCEED();
        }();
        if (R_FAILED(prepare)) { FailSystemPower(prepare); R_RETURN(prepare); }
        {
            std::scoped_lock lk(m_power_mutex);
            R_UNLESS(m_power_gate.Park(),usb::ResultNotInitialized());
            sleep_diagnostics::prepare_stage.store(sleep_diagnostics::Parked);
            sleep_diagnostics::phase.store(static_cast<u32>(m_power_gate.GetPhase()));
            sleep_diagnostics::last_result.store(0);
        }
        R_SUCCEED();
    }

    Result Service::VerifySleepMarker() {
        sleep::VolumeMarker disk{};
        volume::FileInfo file{};
        size_t read{};
        if (m_fat32_volume.IsInitialized()) {
            R_TRY(m_fat32_volume.GetFileInfo(sleep::MarkerPath, &file));
            R_UNLESS(!file.directory && file.size == sizeof(disk), usb::ResultMediaChanged());
            R_TRY(m_fat32_volume.ReadFile(sleep::MarkerPath, 0, &disk, sizeof(disk), &read));
        } else {
            R_UNLESS(m_exfat_volume.IsInitialized(), usb::ResultUnsupportedFilesystem());
            R_TRY(m_exfat_volume.GetFileInfo(sleep::MarkerPath, &file));
            R_UNLESS(!file.directory && file.size == sizeof(disk), usb::ResultMediaChanged());
            R_TRY(m_exfat_volume.ReadFile(sleep::MarkerPath, 0, &disk, sizeof(disk), &read));
        }
        R_UNLESS(read == sizeof(disk) && std::memcmp(&disk, &m_sleep_marker, sizeof(disk)) == 0, usb::ResultMediaChanged());
        R_SUCCEED();
    }

    Result Service::RebindAfterSleep(const Status &saved, u64 deadline) {
        R_UNLESS(m_sleep_marker_armed && saved.partition_index < saved.partition_count &&
            saved.partition_count <= volume::MaxPartitionCount, usb::ResultMediaChanged());
        sleep_diagnostics::rebind_attempts.fetch_add(1);
        sleep_diagnostics::rebind_stage.store(1);
        sleep_diagnostics::rebind_candidates.store(0);
        const bool was_fat32 = m_fat32_volume.IsInitialized();
        const bool was_exfat = m_exfat_volume.IsInitialized();
        R_UNLESS(was_fat32 != was_exfat && m_writable_volume.IsInitialized(), usb::ResultMediaChanged());

        // The old file cache was closed and device sync confirmed before parking.
        // Quarantine before any cleanup; do not call Disconnect(), which would
        // revoke logical handles before candidate validation has finished.
        m_device.Quarantine();
        R_TRY(m_writable_volume.Finalize());
        if (was_fat32) R_TRY(m_fat32_volume.Finalize());
        if (was_exfat) R_TRY(m_exfat_volume.Finalize());
        if (m_device.IsInitialized()) R_TRY(m_device.Finalize());
        if (m_client.IsInitialized()) R_TRY(m_client.Finalize());
        m_writable_ready = false;

        host::InterfaceFilter filter{};
        filter.Flags = 1u << 7;
        filter.bInterfaceClass = mass_storage::InterfaceClass;
        s32 match = -1;
        const auto transient = [](Result result) {
            return svc::ResultSessionClosed::Includes(result) || usb::ResultNotInitialized::Includes(result) ||
                   usb::ResultResourceBusy::Includes(result);
        };
        sleep_diagnostics::rebind_stage.store(2);
        while (match < 0) {
            { std::scoped_lock lk(m_power_mutex); R_UNLESS(m_power_gate.GetPhase() == sleep::Phase::Resuming, usb::ResultNotInitialized()); }
            R_UNLESS(impl::UsbNowNs() < deadline, usb::ResultMediaChanged());
            if (!m_client.IsInitialized()) {
                bool present{};
                R_TRY(sm::HasService(&present, sm::ServiceName::Encode("usb:hs")));
                if (!present) { os::SleepThread(TimeSpan::FromMilliSeconds(100)); continue; }
                const auto init = m_client.Initialize();
                if (transient(init)) { os::SleepThread(TimeSpan::FromMilliSeconds(100)); continue; }
                R_TRY(init);
            }
            s32 count{};
            const auto query = m_client.QueryAvailableInterfaces(&filter, m_available_interfaces, sizeof(m_available_interfaces), &count);
            sleep_diagnostics::rebind_queries.fetch_add(1);
            sleep_diagnostics::rebind_query_result.store(query.GetValue());
            if (transient(query)) {
                R_TRY(m_client.Finalize());
            } else {
                R_TRY(query);
                // A full array can hide a second matching candidate.
                R_UNLESS(count >= 0 && count <= static_cast<s32>(MaxInterfaceCount), usb::ResultOperationDenied());
                sleep_diagnostics::rebind_visible_max.store(std::max(sleep_diagnostics::rebind_visible_max.load(), static_cast<u32>(count)));
                R_UNLESS(count < static_cast<s32>(MaxInterfaceCount), usb::ResultOperationDenied());
                u32 candidates = 0;
                u32 exact = 0;
                for (s32 i = 0; i < count; ++i) {
                    const auto &candidate = m_available_interfaces[i];
                    if (!sleep::IsRebindCandidate(candidate, m_sleep_interface)) continue;
                    ++candidates;
                    if (sleep::DescriptorMatch(candidate, m_sleep_interface)) ++exact;
                    match = i;
                }
                sleep_diagnostics::rebind_candidates.store(std::max(sleep_diagnostics::rebind_candidates.load(), candidates));
                sleep_diagnostics::rebind_exact_max.store(std::max(sleep_diagnostics::rebind_exact_max.load(), exact));
                R_UNLESS(candidates <= 1, usb::ResultOperationDenied());
            }
            if (match >= 0) {
                sleep_diagnostics::rebind_stage.store(3);
                const auto opened = m_device.InitializeForWake(&m_client, &m_available_interfaces[match], saved.lun, deadline);
                if (transient(opened)) {
                    if (m_device.IsInitialized()) { m_device.Quarantine(); R_TRY(m_device.Finalize()); }
                    R_TRY(m_client.Finalize());
                    match = -1;
                } else R_TRY(opened);
            }
            if (match < 0) os::SleepThread(TimeSpan::FromMilliSeconds(100));
        }
        auto &candidate = m_available_interfaces[match];
        m_device.SetWakeReadOnly(true);
        m_device.SetSystemPowerDeadline(deadline);
        ON_SCOPE_EXIT { m_device.SetSystemPowerDeadline(0); };
        const auto &geometry = m_device.GetGeometry();
        R_UNLESS(geometry.block_size == saved.block_size && geometry.block_count == saved.block_count, usb::ResultMediaChanged());
        if (m_sleep_has_serial) {
            u8 serial[32]{};
            R_TRY(m_device.CaptureWakeSerial(serial, candidate.device_desc.iSerialNumber, deadline));
            R_UNLESS(std::memcmp(serial, m_sleep_serial, sizeof(serial)) == 0, usb::ResultMediaChanged());
        }
        sleep_diagnostics::rebind_stage.store(4);
        u8 fingerprint[64]{};
        R_TRY(m_device.CaptureSleepFingerprint(fingerprint, m_sleep_partition_lba, deadline, true));
        R_UNLESS(std::memcmp(fingerprint, m_sleep_fingerprint, sizeof(fingerprint)) == 0, usb::ResultMediaChanged());
        volume::Partition layout[volume::MaxPartitionCount]{};
        size_t count{};
        R_TRY(volume::DiscoverPartitions(&m_device, layout, volume::MaxPartitionCount, &count));
        R_UNLESS(count == saved.partition_count, usb::ResultMediaChanged());
        for (size_t i = 0; i < count; ++i) {
            const auto &a = layout[i]; const auto &b = m_partitions[i];
            R_UNLESS(a.first_lba == b.first_lba && a.sector_count == b.sector_count && a.style == b.style &&
                    a.type == b.type && a.bootable == b.bootable && std::memcmp(a.name,b.name,sizeof(a.name)) == 0,
                    usb::ResultMediaChanged());
        }
        sleep_diagnostics::rebind_stage.store(5);
        const auto &partition = layout[saved.partition_index];
        if (was_fat32) {
            R_TRY(m_fat32_volume.Initialize(&m_device, partition));
        } else {
            R_TRY(m_exfat_volume.Initialize(&m_device, partition));
        }
        R_TRY(VerifySleepMarker());
        // Only after the challenge matches do we reconstruct FatFs. Its clean
        // sector, metadata and open-file caches were dropped with the old mount.
        sleep_diagnostics::rebind_stage.store(6);
        R_TRY(m_writable_volume.Initialize(&m_device, partition));
        R_UNLESS(impl::UsbNowNs() < deadline, usb::ResultMediaChanged());
        R_SUCCEED();
    }

    Result Service::ReserveSystemWake() {
        std::scoped_lock lk(m_power_mutex);
        auto phase = m_power_gate.GetPhase();
        if (phase == sleep::Phase::Awake || phase == sleep::Phase::Resuming) R_SUCCEED();
        R_UNLESS(m_power_gate.BeginWake(), usb::ResultNotInitialized());
        sleep_diagnostics::phase.store(static_cast<u32>(m_power_gate.GetPhase()));
        R_SUCCEED();
    }

    void Service::InvalidateFailedWake(Result result) {
        std::scoped_lock volume_lk(m_volume_mutex);
        m_device.Quarantine(); // only after all previously admitted work drained
        m_writable_ready=false; m_filesystem_type=FileSystemType::Unknown;
        this->AdvanceMediaGeneration();
        this->SetState(State::Error,result);
        this->SetLastIoResult(result);
        this->RequestReconnect();
    }

    void Service::AllowFailedWakeRecovery() {
        std::scoped_lock lk(m_power_mutex);
        m_failed_wake_recovery_allowed = m_power_gate.GetPhase() == sleep::Phase::Failed;
    }

    void Service::DenyFailedWakeRecovery() {
        std::scoped_lock lk(m_power_mutex);
        m_failed_wake_recovery_allowed = false;
    }

    bool Service::IsRecoveredFromFailedWake() const {
        std::scoped_lock lk(m_power_mutex);
        return m_power_gate.GetPhase() == sleep::Phase::Awake && !m_resume_in_progress;
    }

    bool Service::TryRecoverFailedWake() {
        /* A failed wake used to latch the gate for the whole boot, so the
         * drive stayed unusable until reboot even after a replug. Once PSC
         * reports FullAwake and nothing is admitted, discard the old device
         * state and rediscover the drive exactly like a new hot-plug. The
         * media generation advances, so handles opened before sleep fail
         * instead of reaching a possibly different drive. */
        const auto can_recover = [&]() {
            return !m_stop_requested && m_running && m_failed_wake_recovery_allowed && !m_resume_in_progress &&
                m_power_gate.GetPhase() == sleep::Phase::Failed && m_power_gate.Active() == 0;
        };
        {
            std::scoped_lock lk(m_mutex);
            std::scoped_lock power_lk(m_power_mutex);
            if (!can_recover()) return false;
        }
        /* Lock order matches the rest of the service: volume, state, power. */
        std::scoped_lock volume_lk(m_volume_mutex);
        std::scoped_lock lk(m_mutex);
        std::scoped_lock power_lk(m_power_mutex);
        if (!can_recover()) return false;
        m_device.Quarantine(); // Never flush pre-sleep dirty state into the rediscovered drive.
        m_writable_ready = false;
        m_checkpoint_valid = false;
        m_checkpoint_pending = false;
        if (!m_power_gate.Recover()) return false;
        m_failed_wake_recovery_allowed = false;
        if (!m_reconnect_requested) io_diagnostics::reconnect_requests.fetch_add(1, std::memory_order_relaxed);
        m_reconnect_requested = true;
        m_status.state = State::Starting;
        m_status.writable = 0;
        sleep_diagnostics::phase.store(static_cast<u32>(m_power_gate.GetPhase()));
        m_power_cv.Broadcast();
        return true;
    }

    Result Service::ResumeSystemSleep() {
        R_TRY(ReserveSystemWake());
        {
            std::scoped_lock lk(m_power_mutex);
            if (m_power_gate.GetPhase() == sleep::Phase::Awake) R_SUCCEED();
            R_UNLESS(m_power_gate.GetPhase() == sleep::Phase::Resuming, usb::ResultNotInitialized());
            R_UNLESS(!m_resume_in_progress && m_power_gate.Active() == 0, usb::ResultResourceBusy());
            m_resume_in_progress = true;
            sleep_diagnostics::resumes.fetch_add(1);
            sleep_diagnostics::last_resume_ns.store(impl::UsbNowNs());
        }
        ON_SCOPE_EXIT {
            std::scoped_lock lk(m_power_mutex);
            m_resume_in_progress = false;
            m_power_cv.Broadcast();
        };
        bool attempted = false;
        const auto resume = [&]() -> Result {
            std::scoped_lock volume_lk(m_volume_mutex);
            { std::scoped_lock power_lk(m_power_mutex);
              R_UNLESS(m_power_gate.GetPhase() == sleep::Phase::Resuming, usb::ResultNotInitialized()); }
            u64 generation{};
            R_TRY(GetMediaGeneration(&generation));
            R_UNLESS(generation == m_sleep_generation, usb::ResultMediaChanged());
            if (m_sleep_had_media) {
                bool retained = false;
                const u64 now = impl::UsbNowNs();
                const auto probe_deadline = sleep::Deadline(now, sleep::ProbeBudgetNs);
                const auto lookup_deadline = m_sleep_marker_armed ? sleep::Deadline(now, 500'000'000ULL) : probe_deadline;
                Result previous = usb::ResultMediaChanged();
                if (m_device.IsInitialized() && m_client.IsInitialized()) {
                    host::Interface current{};
                    previous = CaptureRetainedInterface(&current, lookup_deadline);
                    if (usb::ResultInternalStateError::Includes(previous)) R_RETURN(previous);
                    if (R_SUCCEEDED(previous)) {
                        retained = current.inf.ID == m_sleep_interface.inf.ID && current.deviceID == m_sleep_interface.deviceID &&
                            current.busID == m_sleep_interface.busID && current.timestamp == m_sleep_interface.timestamp &&
                            current.inf.deviceID_2 == m_sleep_interface.inf.deviceID_2 && sleep::DescriptorMatch(current,m_sleep_interface);
                        if (!retained) previous = usb::ResultMediaChanged();
                    }
                    if (retained) {
                        u8 fingerprint[64]{};
                        m_device.SetSystemPowerDeadline(probe_deadline);
                        ON_SCOPE_EXIT { m_device.SetSystemPowerDeadline(0); };
                        previous = m_device.CaptureSleepFingerprint(fingerprint,m_sleep_partition_lba,probe_deadline,true);
                        if (usb::ResultMediaChanged::Includes(previous)) R_RETURN(previous);
                        retained = R_SUCCEEDED(previous);
                        if (retained) {
                            R_UNLESS(std::memcmp(fingerprint,m_sleep_fingerprint,sizeof(fingerprint))==0,usb::ResultMediaChanged());
                            if (m_sleep_marker_armed) {
                                previous = VerifySleepMarker();
                                if (R_FAILED(previous)) {
                                    // The old endpoint can disappear between the fingerprint
                                    // and marker reads. Only a transport fault permits rebind.
                                    R_UNLESS(svc::ResultSessionClosed::Includes(previous) || svc::ResultTimedOut::Includes(previous) ||
                                        usb::ResultTransactionError::Includes(previous) || usb::ResultInterrupted::Includes(previous) ||
                                        usb::ResultNotInitialized::Includes(previous) || usb::ResultScsiCommandFailed::Includes(previous), previous);
                                    retained = false;
                                }
                            }
                        }
                    }
                }
                if (!retained) {
                    if (!m_sleep_marker_armed) R_RETURN(previous);
                    Status saved{};
                    R_TRY(GetStatus(&saved));
                    R_UNLESS(saved.state == State::Ready && saved.media_generation == m_sleep_generation, usb::ResultMediaChanged());
                    attempted = true;
                    R_TRY(RebindAfterSleep(saved, sleep::Deadline(now, 20'000'000'000ULL)));
                } else if (m_writable_volume.IsInitialized()) {
                    // Reads after the early checkpoint can have opened cached
                    // cursors. Drop those only after the retained media proof.
                    R_TRY(m_writable_volume.Flush());
                }
            }
            // A concurrent Stop or PSC fault cancels publication. Preserve lock
            // order: volume -> status -> admission. Provisional writes remain
            // blocked until the same-media proof and final mount both succeeded.
            std::scoped_lock state_lk(m_mutex);
            std::scoped_lock power_lk(m_power_mutex);
            R_UNLESS(!m_stop_requested && m_media_generation == m_sleep_generation &&
                m_power_gate.GetPhase() == sleep::Phase::Resuming, usb::ResultNotInitialized());
            R_UNLESS(m_power_gate.CompleteWake(), usb::ResultNotInitialized());
            if (attempted) {
                m_device.SetWakeReadOnly(false);
                m_writable_ready = true;
                m_status.interface_id = m_device.GetInterfaceId();
                sleep_diagnostics::rebind_successes.fetch_add(1);
                sleep_diagnostics::rebind_stage.store(7);
            }
            m_checkpoint_valid = false;
            m_checkpoint_pending = m_sleep_had_media && m_writable_ready;
            sleep_diagnostics::last_result.store(0);
            m_power_cv.Broadcast();
            R_SUCCEED();
        }();
        if (attempted) sleep_diagnostics::rebind_result.store(resume.GetValue());
        if (R_FAILED(resume)) {
            if (usb::ResultMediaChanged::Includes(resume)) sleep_diagnostics::identity_failures.fetch_add(1);
            FailSystemPower(resume);
            if (!ShouldStop()) InvalidateFailedWake(resume);
        }
        { std::scoped_lock lk(m_power_mutex); sleep_diagnostics::phase.store(static_cast<u32>(m_power_gate.GetPhase())); }
        u64 generation{}; static_cast<void>(GetMediaGeneration(&generation));
        sleep_diagnostics::generation_after.store(generation);
        R_RETURN(resume);
    }

    Result Service::Start() {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(!m_running, usb::ResultAlreadyInitialized());

        { std::scoped_lock power_lk(m_power_mutex);
          R_UNLESS(m_power_gate.GetPhase() != sleep::Phase::Failed, usb::ResultNotInitialized());
          R_UNLESS(!m_resume_in_progress, usb::ResultResourceBusy());
          if (m_power_gate.GetPhase()==sleep::Phase::Stopped) {
              R_UNLESS(m_power_gate.Restart(),usb::ResultResourceBusy());
          }
          R_UNLESS(m_power_gate.GetPhase() == sleep::Phase::Awake, usb::ResultResourceBusy());
        }
        m_stop_requested = false;
        m_reconnect_requested = false;
        m_status = {};
        m_status.state = State::Starting;
        m_status.last_io_result = ResultSuccess().GetValue();
        m_status.interface_id = -1;
        m_status.media_generation = m_media_generation;

        const auto result = os::CreateThread(std::addressof(m_thread), ThreadEntry, this, m_thread_stack, sizeof(m_thread_stack), os::LowestSystemThreadPriority);
        if (R_FAILED(result)) {
            m_status.state = State::Stopped;
            m_status.last_result = static_cast<Result>(result).GetValue();
            R_RETURN(result);
        }

        m_thread_created = true;
        m_running = true;
        m_status.running = true;
        os::SetThreadName(std::addressof(m_thread), "ams:usb-storage");
        os::StartThread(std::addressof(m_thread));
        R_SUCCEED();
    }

    Result Service::Stop() {
        {
            std::scoped_lock lk(m_mutex);
            R_UNLESS(m_running && m_thread_created, usb::ResultNotInitialized());
            R_UNLESS(!m_stop_requested, usb::ResultResourceBusy());
            m_stop_requested = true;
        }

        { std::unique_lock power_lk(m_power_mutex);
          // Cancelling a sleep transition revokes its pending wake ticket.
          // Start must never reopen storage before PSC learns the result.
          const auto phase = m_power_gate.GetPhase();
          if ((phase == sleep::Phase::Awake || phase == sleep::Phase::Stopped) && !m_resume_in_progress) {
              m_power_gate.Stop();
          } else {
              m_power_gate.Fail();
          }
          m_power_cv.Broadcast();
          while (m_resume_in_progress) m_power_cv.Wait(m_power_mutex); }
        os::WaitThread(std::addressof(m_thread));
        os::DestroyThread(std::addressof(m_thread));

        std::scoped_lock lk(m_mutex);
        m_thread_created = false;
        m_running = false;
        m_stop_requested = false;
        m_reconnect_requested = false;
        m_status = {};
        m_status.state = State::Stopped;
        m_status.last_io_result = ResultSuccess().GetValue();
        m_status.media_generation = m_media_generation;
        R_SUCCEED();
    }

    Result Service::GetStatus(Status *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        *out = m_status;
        R_SUCCEED();
    }

    Result Service::GetMediaGeneration(u64 *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        std::scoped_lock lk(m_mutex);
        *out = m_media_generation;
        R_SUCCEED();
    }

    Result Service::CheckMediaGenerationLocked(u64 media_generation) const {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_media_generation == media_generation, usb::ResultMediaChanged());
        R_UNLESS(!m_eject.Blocked() && !m_stop_requested && !m_reconnect_requested && m_status.state == State::Ready,
            usb::ResultNotInitialized());
        R_SUCCEED();
    }

    Result Service::GetFileInfo(const char *path, volume::FileInfo *out) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(path != nullptr && out != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            { std::scoped_lock state_lk(m_mutex); R_UNLESS(!m_eject.Blocked(),usb::ResultResourceBusy()); }
            if (m_writable_ready) {
                result = m_writable_volume.GetFileInfo(path, out);
            } else {
                switch (m_filesystem_type) {
                    case FileSystemType::Fat32:
                        result = m_fat32_volume.GetFileInfo(path, out);
                        break;
                    case FileSystemType::ExFat:
                        result = m_exfat_volume.GetFileInfo(path, out);
                        break;
                    case FileSystemType::Unknown:
                        result = usb::ResultNotInitialized();
                        break;
                    AMS_UNREACHABLE_DEFAULT_CASE();
                }
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::ReadFile(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(path != nullptr && out_read != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            pause_diagnostics::Scope volume_wait(pause_diagnostics::Stage::VolumeReadWait, size);
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            volume_wait.Finish(ResultSuccess());
            { std::scoped_lock state_lk(m_mutex); R_UNLESS(!m_eject.Blocked(),usb::ResultResourceBusy()); }
            if (m_writable_ready) {
                result = m_writable_volume.ReadFile(path, offset, buffer, size, out_read);
            } else {
                switch (m_filesystem_type) {
                    case FileSystemType::Fat32:
                        result = m_fat32_volume.ReadFile(path, offset, buffer, size, out_read);
                        break;
                    case FileSystemType::ExFat:
                        result = m_exfat_volume.ReadFile(path, offset, buffer, size, out_read);
                        break;
                    case FileSystemType::Unknown:
                        result = usb::ResultNotInitialized();
                        break;
                    AMS_UNREACHABLE_DEFAULT_CASE();
                }
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::ReadDirectory(const char *path, volume::DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(path != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            { std::scoped_lock state_lk(m_mutex); R_UNLESS(!m_eject.Blocked(),usb::ResultResourceBusy()); }
            if (m_writable_ready) {
                result = m_writable_volume.ReadDirectory(path, out, max_entries, skip_entries, include_directories, include_files, out_count);
            } else {
                switch (m_filesystem_type) {
                    case FileSystemType::Fat32:
                        result = m_fat32_volume.ReadDirectory(path, out, max_entries, skip_entries, include_directories, include_files, out_count);
                        break;
                    case FileSystemType::ExFat:
                        result = m_exfat_volume.ReadDirectory(path, out, max_entries, skip_entries, include_directories, include_files, out_count);
                        break;
                    case FileSystemType::Unknown:
                        result = usb::ResultNotInitialized();
                        break;
                    AMS_UNREACHABLE_DEFAULT_CASE();
                }
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::GetFileInfoAtGeneration(const char *path, volume::FileInfo *out, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(path != nullptr && out != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            R_TRY(this->CheckMediaGenerationLocked(media_generation));
            if (m_writable_ready) {
                result = m_writable_volume.GetFileInfo(path, out);
            } else {
                switch (m_filesystem_type) {
                    case FileSystemType::Fat32:
                        result = m_fat32_volume.GetFileInfo(path, out);
                        break;
                    case FileSystemType::ExFat:
                        result = m_exfat_volume.GetFileInfo(path, out);
                        break;
                    case FileSystemType::Unknown:
                        result = usb::ResultNotInitialized();
                        break;
                    AMS_UNREACHABLE_DEFAULT_CASE();
                }
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::InvalidateReadCacheAtGeneration(u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        std::scoped_lock lk(m_volume_mutex);
        R_TRY(CheckMediaGenerationLocked(media_generation));
        // The native read-only FAT32/exFAT readers already read through to disk.
        R_SUCCEED_IF(!m_writable_ready);
        const Result rc = m_writable_volume.InvalidateReadCache();
        CompleteIoLocked(rc);
        R_RETURN(rc);
    }

    Result Service::ReadFileAtGeneration(const char *path, u64 offset, void *buffer, size_t size, size_t *out_read, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(path != nullptr && out_read != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            pause_diagnostics::Scope volume_wait(pause_diagnostics::Stage::VolumeReadWait, size);
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            volume_wait.Finish(ResultSuccess());
            R_TRY(this->CheckMediaGenerationLocked(media_generation));
            if (m_writable_ready) {
                result = m_writable_volume.ReadFile(path, offset, buffer, size, out_read);
            } else {
                switch (m_filesystem_type) {
                    case FileSystemType::Fat32:
                        result = m_fat32_volume.ReadFile(path, offset, buffer, size, out_read);
                        break;
                    case FileSystemType::ExFat:
                        result = m_exfat_volume.ReadFile(path, offset, buffer, size, out_read);
                        break;
                    case FileSystemType::Unknown:
                        result = usb::ResultNotInitialized();
                        break;
                    AMS_UNREACHABLE_DEFAULT_CASE();
                }
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::ReadDirectoryAtGeneration(const char *path, volume::DirectoryEntryInfo *out, size_t max_entries, size_t skip_entries, bool include_directories, bool include_files, size_t *out_count, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(path != nullptr && out_count != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            R_TRY(this->CheckMediaGenerationLocked(media_generation));
            if (m_writable_ready) {
                result = m_writable_volume.ReadDirectory(path, out, max_entries, skip_entries, include_directories, include_files, out_count);
            } else {
                switch (m_filesystem_type) {
                    case FileSystemType::Fat32:
                        result = m_fat32_volume.ReadDirectory(path, out, max_entries, skip_entries, include_directories, include_files, out_count);
                        break;
                    case FileSystemType::ExFat:
                        result = m_exfat_volume.ReadDirectory(path, out, max_entries, skip_entries, include_directories, include_files, out_count);
                        break;
                    case FileSystemType::Unknown:
                        result = usb::ResultNotInitialized();
                        break;
                    AMS_UNREACHABLE_DEFAULT_CASE();
                }
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::GetSpaceSize(u64 *out_free_size, u64 *out_total_size) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(out_free_size != nullptr && out_total_size != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            pause_diagnostics::Scope volume_wait(pause_diagnostics::Stage::VolumeSpaceWait);
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            volume_wait.Finish(ResultSuccess());
            pause_diagnostics::Scope space_query(pause_diagnostics::Stage::SpaceQuery);
            { std::scoped_lock state_lk(m_mutex); R_UNLESS(!m_eject.Blocked(),usb::ResultResourceBusy()); }
            if (m_writable_ready) {
                result = m_writable_volume.GetSpaceSize(out_free_size, out_total_size);
            } else {
                switch (m_filesystem_type) {
                    case FileSystemType::Fat32:
                        result = m_fat32_volume.GetSpaceSize(out_free_size, out_total_size);
                        break;
                    case FileSystemType::ExFat:
                        result = m_exfat_volume.GetSpaceSize(out_free_size, out_total_size);
                        break;
                    case FileSystemType::Unknown:
                        result = usb::ResultNotInitialized();
                        break;
                    AMS_UNREACHABLE_DEFAULT_CASE();
                }
            }
            this->CompleteIoLocked(result);
            space_query.Finish(result);
        }
        R_RETURN(result);
    }

    Result Service::IsWritable(bool *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
        std::scoped_lock lk(m_volume_mutex);
        startup_lock_wait.Finish();
        *out = m_writable_ready;
        R_SUCCEED();
    }

    Result Service::CreateFileAtGeneration(const char *path, u64 size, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(!m_read_only, usb::ResultOperationDenied());
        R_UNLESS(path != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            result = this->CheckMediaGenerationLocked(media_generation);
            if (R_SUCCEEDED(result)) {
                m_checkpoint_valid = false;
                m_checkpoint_pending = false;
                result = m_writable_ready ? m_writable_volume.CreateFile(path, size) : static_cast<Result>(usb::ResultOperationDenied());
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::ResizeFileAtGeneration(const char *path, u64 size, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(!m_read_only, usb::ResultOperationDenied());
        R_UNLESS(path != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            result = this->CheckMediaGenerationLocked(media_generation);
            if (R_SUCCEEDED(result)) {
                m_checkpoint_valid = false;
                m_checkpoint_pending = false;
                result = m_writable_ready ? m_writable_volume.ResizeFile(path, size) : static_cast<Result>(usb::ResultOperationDenied());
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::DeleteFileAtGeneration(const char *path, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(!m_read_only, usb::ResultOperationDenied());
        R_UNLESS(path != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            result = this->CheckMediaGenerationLocked(media_generation);
            if (R_SUCCEEDED(result)) {
                m_checkpoint_valid = false;
                m_checkpoint_pending = false;
                result = m_writable_ready ? m_writable_volume.DeleteFile(path) : static_cast<Result>(usb::ResultOperationDenied());
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::RenameFileAtGeneration(const char *old_path, const char *new_path, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(!m_read_only, usb::ResultOperationDenied());
        R_UNLESS(old_path != nullptr && new_path != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            result = this->CheckMediaGenerationLocked(media_generation);
            if (R_SUCCEEDED(result)) {
                m_checkpoint_valid = false;
                m_checkpoint_pending = false;
                result = m_writable_ready ? m_writable_volume.RenameFile(old_path, new_path) : static_cast<Result>(usb::ResultOperationDenied());
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::WriteFileAtGeneration(const char *path, u64 offset, const void *buffer, size_t size, size_t *out_written, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(!m_read_only, usb::ResultOperationDenied());
        R_UNLESS(path != nullptr && out_written != nullptr, usb::ResultInvalidParameter());
        *out_written = 0;
        Result result = ResultSuccess();
        {
            pause_diagnostics::Scope volume_wait(pause_diagnostics::Stage::VolumeWriteWait, size);
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            volume_wait.Finish(ResultSuccess());
            result = this->CheckMediaGenerationLocked(media_generation);
            if (R_SUCCEEDED(result)) {
                m_checkpoint_valid = false;
                m_checkpoint_pending = false;
                result = m_writable_ready ? m_writable_volume.WriteFile(path, offset, buffer, size, out_written) : static_cast<Result>(usb::ResultOperationDenied());
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::CreateDirectoryAtGeneration(const char *path, u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(!m_read_only, usb::ResultOperationDenied());
        R_UNLESS(path != nullptr, usb::ResultInvalidParameter());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Metadata);
            result = this->CheckMediaGenerationLocked(media_generation);
            if (R_SUCCEEDED(result)) {
                m_checkpoint_valid = false;
                m_checkpoint_pending = false;
                result = m_writable_ready ? m_writable_volume.CreateDirectory(path) : static_cast<Result>(usb::ResultOperationDenied());
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    Result Service::FlushAtGeneration(u64 media_generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(!m_read_only, usb::ResultOperationDenied());
        Result result = ResultSuccess();
        {
            startup_io_diagnostics::PhaseScope startup_lock_wait(NexusStartupIo_LockWait);
            std::scoped_lock lk(m_volume_mutex);
            startup_lock_wait.Finish();
            const startup_io_diagnostics::PhaseScope startup_work(NexusStartupIo_Sync);
            result = this->CheckMediaGenerationLocked(media_generation);
            if (R_SUCCEEDED(result)) {
                result = m_writable_ready ? m_writable_volume.Flush() : static_cast<Result>(usb::ResultOperationDenied());
            }
            this->CompleteIoLocked(result);
        }
        R_RETURN(result);
    }

    bool Service::IsRunning() const {
        std::scoped_lock lk(m_mutex);
        return m_running;
    }

    Result Service::AcquireEjectLease(u64 generation) {
        std::scoped_lock lk(m_mutex);
        R_UNLESS(!m_stop_requested && !m_reconnect_requested && m_status.state==State::Ready,
                 usb::ResultNotInitialized());
        R_UNLESS(m_eject.Acquire(generation),usb::ResultResourceBusy());
        R_SUCCEED();
    }
    void Service::ReleaseEjectLease(u64 generation) {
        std::scoped_lock lk(m_mutex); m_eject.Release(generation);
    }
    eject::Status Service::GetEjectStatus() const {
        std::scoped_lock lk(m_mutex); return m_eject.Get();
    }
    Result Service::RequestEject(u64 request,u64 generation) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        // Do not wait behind any I/O or interrupt a transfer to satisfy eject.
        std::unique_lock volume_lock(m_volume_mutex,std::try_to_lock);
        R_UNLESS(volume_lock.owns_lock(),usb::ResultResourceBusy());
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_running && !m_stop_requested && !m_reconnect_requested && m_status.state==State::Ready,
                 usb::ResultNotInitialized());
        R_UNLESS(m_eject.Request(request,generation),usb::ResultResourceBusy());
        // Gate closes atomically with lease admission. Existing handles refuse
        // the request; new opens and I/O cannot enter after this point.
        m_status.state=State::Mounting;
        m_status.writable=0;
        R_SUCCEED();
    }
    Result Service::ResumeAfterEject() {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        std::scoped_lock lk(m_mutex);
        R_UNLESS(m_eject.Resume(),usb::ResultOperationDenied());
        m_status.state=State::Starting;
        R_SUCCEED();
    }
    Result Service::ControlKeepAliveTest(NexusUsbKeepAliveStatus *out, u64 caller_pid, const NexusUsbKeepAliveRequest &request) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(out && caller_pid && keep_alive_test::Lease::ValidRequest(request), usb::ResultInvalidParameter());
        const auto apply = [&]() -> Result {
            std::scoped_lock state_lk(m_mutex);
            if (request.operation == NEXUS_USB_KEEP_ALIVE_SUSPEND) {
                R_UNLESS(m_running && !m_stop_requested && !m_reconnect_requested &&
                         m_status.state == State::Ready && !m_eject.Blocked(), usb::ResultNotInitialized());
            }
            std::scoped_lock lease_lk(g_keep_alive_test_mutex);
            R_UNLESS(!g_keep_alive_test_service || g_keep_alive_test_service == this, usb::ResultOperationDenied());
            g_keep_alive_test_service = this;
            const auto outcome = g_keep_alive_test_lease.Control(impl::UsbNowNs(), m_media_generation, caller_pid, request, out);
            switch (outcome) {
                case keep_alive_test::Outcome::Success: R_SUCCEED();
                case keep_alive_test::Outcome::InvalidParameter: R_THROW(usb::ResultInvalidParameter());
                case keep_alive_test::Outcome::Busy: R_THROW(usb::ResultResourceBusy());
                case keep_alive_test::Outcome::Denied: R_THROW(usb::ResultOperationDenied());
                case keep_alive_test::Outcome::MediaChanged: R_THROW(usb::ResultMediaChanged());
            }
            R_THROW(usb::ResultInternalStateError());
        };
        /* Query only copies RAM and never queues behind the USB volume.
         * Mutations use the same lock order as the worker. In-flight probes
         * finish before suspension succeeds; no transfer is aborted. */
        if (request.operation == NEXUS_USB_KEEP_ALIVE_QUERY) R_RETURN(apply());
        std::scoped_lock volume_lk(m_volume_mutex);
        R_RETURN(apply());
    }

    Result Service::ControlWriteTransferTest(NexusUsbWriteTransferStatus *out, u64 caller_pid, const NexusUsbWriteTransferRequest &request) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(out && caller_pid && write_transfer::Lease::ValidRequest(request), usb::ResultInvalidParameter());
        const auto apply = [&]() -> Result {
            std::scoped_lock state_lk(m_mutex);
            if (request.operation == NEXUS_USB_WRITE_TRANSFER_ACQUIRE) {
                R_UNLESS(m_running && !m_stop_requested && !m_reconnect_requested &&
                         m_status.state == State::Ready && !m_eject.Blocked(), usb::ResultNotInitialized());
            }
            if (request.operation == NEXUS_USB_WRITE_TRANSFER_ACQUIRE)
                R_UNLESS(!completion_probe::IsActive() && !host_write::IsActive(), usb::ResultResourceBusy());
            switch (write_transfer::Control(m_media_generation, caller_pid, request, out)) {
                case write_transfer::Outcome::Success: R_SUCCEED();
                case write_transfer::Outcome::InvalidParameter: R_THROW(usb::ResultInvalidParameter());
                case write_transfer::Outcome::Busy: R_THROW(usb::ResultResourceBusy());
                case write_transfer::Outcome::Denied: R_THROW(usb::ResultOperationDenied());
                case write_transfer::Outcome::MediaChanged: R_THROW(usb::ResultMediaChanged());
            }
            R_THROW(usb::ResultInternalStateError());
        };
        // Query never queues behind a slow USB command. Mutations cannot
        // switch limits underneath an in-flight volume request.
        if (request.operation == NEXUS_USB_WRITE_TRANSFER_QUERY) R_RETURN(apply());
        std::scoped_lock volume_lk(m_volume_mutex);
        R_RETURN(apply());
    }
    Result Service::ControlCompletionProbeTest(NexusUsbCompletionProbeStatus *out, u64 caller_pid, const NexusUsbCompletionProbeRequest &request) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(out && caller_pid && completion_probe::Lease::ValidRequest(request), usb::ResultInvalidParameter());
        const auto apply = [&]() -> Result {
            std::scoped_lock state_lk(m_mutex);
            if (request.operation == NEXUS_USB_COMPLETION_PROBE_ACQUIRE) {
                R_UNLESS(m_running && !m_stop_requested && !m_reconnect_requested &&
                         m_status.state == State::Ready && !m_eject.Blocked(), usb::ResultNotInitialized());
            }
            if (request.operation == NEXUS_USB_COMPLETION_PROBE_ACQUIRE)
                R_UNLESS(!write_transfer::IsActive() && !host_write::IsActive(), usb::ResultResourceBusy());
            switch (completion_probe::Control(m_media_generation, caller_pid, request, out)) {
                case completion_probe::Outcome::Success: R_SUCCEED();
                case completion_probe::Outcome::InvalidParameter: R_THROW(usb::ResultInvalidParameter());
                case completion_probe::Outcome::Busy: R_THROW(usb::ResultResourceBusy());
                case completion_probe::Outcome::Denied: R_THROW(usb::ResultOperationDenied());
                case completion_probe::Outcome::MediaChanged: R_THROW(usb::ResultMediaChanged());
            }
            R_THROW(usb::ResultInternalStateError());
        };
        // Query never queues behind a slow USB command. Mutations cannot
        // change the completion policy underneath in-flight volume I/O.
        if (request.operation == NEXUS_USB_COMPLETION_PROBE_QUERY) R_RETURN(apply());
        std::scoped_lock volume_lk(m_volume_mutex);
        R_RETURN(apply());
    }

    Result Service::ControlHostWriteTest(NexusUsbHostWriteStatus *out, u64 caller_pid, const NexusUsbHostWriteRequest &request) {
        R_TRY(BeginSystemOperation());
        ON_SCOPE_EXIT { EndSystemOperation(); };
        R_UNLESS(out && caller_pid && host_write::Lease::ValidRequest(request), usb::ResultInvalidParameter());
        const auto apply = [&]() -> Result {
            std::scoped_lock state_lk(m_mutex);
            if (request.operation == NEXUS_USB_HOST_WRITE_ACQUIRE) {
                R_UNLESS(m_running && !m_stop_requested && !m_reconnect_requested &&
                         m_status.state == State::Ready && !m_eject.Blocked(), usb::ResultNotInitialized());
                R_UNLESS(!write_transfer::IsActive() && !completion_probe::IsActive(), usb::ResultResourceBusy());
            }
            switch (host_write::Control(m_media_generation, caller_pid, request, out)) {
                case host_write::Outcome::Success: R_SUCCEED();
                case host_write::Outcome::InvalidParameter: R_THROW(usb::ResultInvalidParameter());
                case host_write::Outcome::Busy: R_THROW(usb::ResultResourceBusy());
                case host_write::Outcome::Denied: R_THROW(usb::ResultOperationDenied());
                case host_write::Outcome::MediaChanged: R_THROW(usb::ResultMediaChanged());
            }
            R_THROW(usb::ResultInternalStateError());
        };
        // Mutations serialize with volume I/O; status queries only read RAM.
        if (request.operation == NEXUS_USB_HOST_WRITE_QUERY) R_RETURN(apply());
        std::scoped_lock volume_lk(m_volume_mutex);
        R_RETURN(apply());
    }

    bool Service::ProcessEject() {
        {
            std::scoped_lock lk(m_mutex);
            if(!m_eject.Blocked()) return false;
            if(m_eject.Get().phase==eject::Phase::Failed) {
                // Keep the failure visible to the eject panel briefly, then
                // remount from scratch instead of blocking the drive until
                // reboot. A partly finalized volume is never reused.
                if(impl::UsbNowNs()-m_eject_failed_ns < EjectFailureHoldNs) return true;
                m_eject.RecoverFromFailure();
                if (!m_reconnect_requested) io_diagnostics::reconnect_requests.fetch_add(1, std::memory_order_relaxed);
                m_reconnect_requested=true;
                m_status.state=State::Starting;
                return false;
            }
            if(m_eject.Get().phase!=eject::Phase::Pending) return true;
        }
        std::unique_lock volume_lock(m_volume_mutex,std::try_to_lock);
        if(!volume_lock.owns_lock()) return true;
        const auto step=[&](eject::Stage stage,auto operation) {
            {std::scoped_lock lk(m_mutex);m_eject.At(stage);}
            const Result rc=operation();
            if(R_FAILED(rc)) {
                std::scoped_lock lk(m_mutex);
                m_eject.Fail(rc.GetValue());m_status.state=State::Error;m_status.last_result=rc.GetValue();
                m_eject_failed_ns=impl::UsbNowNs();
            }
            return R_SUCCEEDED(rc);
        };
        if(!step(eject::Stage::FilesystemFlush,[&]() -> Result {
            R_UNLESS(m_device.IsInitialized(),usb::ResultNotInitialized());
            if(m_writable_volume.IsInitialized()) R_RETURN(m_writable_volume.Flush());
            R_UNLESS(m_filesystem_type!=FileSystemType::Unknown,usb::ResultNotInitialized());
            R_SUCCEED();
        })) return true;
        if(!step(eject::Stage::Unmount,[&]() -> Result {
            if(m_writable_volume.IsInitialized()) R_TRY(m_writable_volume.Finalize());
            if(m_fat32_volume.IsInitialized()) R_TRY(m_fat32_volume.Finalize());
            if(m_exfat_volume.IsInitialized()) R_TRY(m_exfat_volume.Finalize());
            m_writable_ready=false;m_filesystem_type=FileSystemType::Unknown;
            R_SUCCEED();
        })) return true;
        // After all filesystem finalization writes, before releasing transport.
        if(!step(eject::Stage::DeviceCache,[&](){return m_device.SynchronizeCacheForEject();})) return true;
        if(!step(eject::Stage::DeviceRelease,[&](){return m_device.Finalize();})) return true;
        if(!step(eject::Stage::HostRelease,[&](){return m_client.Finalize();})) return true;
        this->AdvanceMediaGeneration();
        {
            std::scoped_lock lk(m_mutex);
            m_status.state=State::Stopped;m_status.last_result=0;m_status.last_io_result=0;
            m_status.writable=0;m_status.filesystem_type=FileSystemType::Unknown;
            m_status.interface_id=-1;m_eject.Complete();
        }
        return true;
    }

    void Service::ThreadEntry(void *arg) {
        static_cast<Service *>(arg)->WorkerThread();
    }

    void Service::RunIdleKeepAlive() {
        /* One owner/one command stream. Never queue behind an active install,
         * filesystem operation, disconnect, or initialization. */
        std::unique_lock lk(m_volume_mutex, std::try_to_lock);
        if (!lk.owns_lock()) {
            io_diagnostics::keep_alive_busy_skips.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        {
            std::scoped_lock state_lk(m_mutex);
            if (m_stop_requested || m_reconnect_requested || m_status.state != State::Ready) return;
            std::scoped_lock lease_lk(g_keep_alive_test_mutex);
            if (g_keep_alive_test_service == this &&
                g_keep_alive_test_lease.ShouldSuppress(impl::UsbNowNs(), m_media_generation)) return;
        }
        if (!m_device.IsInitialized() || !impl::IsKeepAliveDue(impl::UsbNowNs(), io_diagnostics::last_command_ns.load(std::memory_order_relaxed))) return;

        io_diagnostics::keep_alive_attempts.fetch_add(1, std::memory_order_relaxed);
        const auto result = m_device.KeepAlive();
        io_diagnostics::keep_alive_last_result.store(result.GetValue(), std::memory_order_relaxed);
        io_diagnostics::keep_alive_last_ns.store(impl::UsbNowNs(), std::memory_order_relaxed);
        if (R_SUCCEEDED(result)) {
            io_diagnostics::keep_alive_successes.fetch_add(1, std::memory_order_relaxed);
        } else {
            io_diagnostics::keep_alive_failures.fetch_add(1, std::memory_order_relaxed);
            /* Reject old handles and all new volume operations before dropping
             * the lock. Worker owns teardown/reprobe; never continue using an
             * uncertain BOT/UAS stream or replay an interrupted install write. */
            m_writable_ready = false;
            m_filesystem_type = FileSystemType::Unknown;
            this->AdvanceMediaGeneration();
            this->SetLastIoResult(result);
            runtime_diagnostics::disconnect_reason.store(2, std::memory_order_relaxed);
            this->SetState(State::Error, result);
            this->RequestReconnect();
        }
    }

    void Service::WorkerThread() {
        host::InterfaceFilter filter{};
        mass_storage::MakeInterfaceFilter(std::addressof(filter));
        impl::StartupProbeBackoff startup_backoff;
        reader::InventoryCadence inventory_cadence;
        interface_recovery::Budget interface_recovery_budget;

        while (!this->ShouldStop()) {
            const auto admission = BeginSystemOperation();
            if (R_FAILED(admission)) {
                if (this->ShouldStop()) break;
                if (this->TryRecoverFailedWake()) continue;
                os::SleepThread(WorkerPollInterval); continue;
            }
            ON_SCOPE_EXIT { EndSystemOperation(); };
            if(this->ProcessEject()) {
                // Remain parked even if the drive is still plugged in. Only a
                // separate explicit reconnect request resumes discovery.
                os::SleepThread(WorkerPollInterval);continue;
            }
            /* File operations run on the ams:usb server thread. They may
             * discover that a USB interface/session has died, but only this
             * worker owns the root usb:hs client. Reopen it here after all
             * child volume/interface objects have been released. */
            if (this->ConsumeReconnectRequest()) {
                this->Disconnect();
                if (m_client.IsInitialized()) {
                    static_cast<void>(m_client.Finalize());
                }
                this->SetState(State::Starting, ResultSuccess());
            }

            if (!m_client.IsInitialized()) {
                const auto result = m_client.Initialize();
                this->SetProbeResult(ProbeStage::ClientInitialize, result);
                if (R_FAILED(result)) {
                    this->SetState(State::Error, result);
                    os::SleepThread(WorkerPollInterval);
                    continue;
                }
                this->SetWaiting(ResultSuccess());
            }

            if (m_device.IsInitialized()) {
                s32 acquired_count = 0;
                const auto result = m_client.QueryAcquiredInterfaces(m_acquired_interfaces, sizeof(m_acquired_interfaces), std::addressof(acquired_count));
                this->SetProbeResult(ProbeStage::QueryAcquiredInterfaces, result, 0, acquired_count > 0 ? static_cast<u32>(acquired_count) : 0);
                if (R_FAILED(result)) {
                    io_diagnostics::link_query_failures.fetch_add(1, std::memory_order_relaxed);
                    runtime_diagnostics::disconnect_reason.store(3, std::memory_order_relaxed);
                    this->Disconnect();
                    if (svc::ResultSessionClosed::Includes(result)) {
                        /* A removed/restarted usb:hs session cannot be
                         * reused. Drop the client so the next iteration
                         * performs the complete bind/event handshake. */
                        static_cast<void>(m_client.Finalize());
                        this->SetState(State::Starting, result);
                    } else {
                        this->SetState(State::Error, result);
                    }
                    os::SleepThread(WorkerPollInterval);
                    continue;
                }

                bool interface_present = false;
                for (s32 i = 0; i < acquired_count && i < static_cast<s32>(MaxInterfaceCount); ++i) {
                    if (m_acquired_interfaces[i].inf.ID == m_device.GetInterfaceId()) {
                        interface_present = true;
                        break;
                    }
                }

                if (!interface_present) {
                    io_diagnostics::link_missing_events.fetch_add(1, std::memory_order_relaxed);
                    runtime_diagnostics::disconnect_reason.store(4, std::memory_order_relaxed);
                    this->Disconnect();
                    this->SetWaiting(ResultSuccess());
                } else {
                    TryArmSleepCheckpoint();
                    this->RunIdleKeepAlive();
                    os::SleepThread(WorkerPollInterval);
                }
                continue;
            }

            // Stop/service polling remains responsive during the cooldown.
            // A newly inserted device may wait at most 30 seconds here.
            if (startup_backoff.IsWaiting(impl::UsbNowNs())) {
                os::SleepThread(WorkerPollInterval);
                continue;
            }
            io_diagnostics::probe_cooldown_ms.store(0);
            s32 available_count = 0;
            const auto result = reader::QueryAvailable(m_client, filter, m_available_interfaces, sizeof(m_available_interfaces), std::addressof(available_count));
            this->SetProbeResult(ProbeStage::QueryAvailableInterfaces, result, available_count > 0 ? static_cast<u32>(available_count) : 0);
            if (R_FAILED(result)) {
                if (svc::ResultSessionClosed::Includes(result)) {
                    /* Reconnect instead of repeatedly polling a dead
                     * service domain after usb:hs closes the session. */
                    static_cast<void>(m_client.Finalize());
                    this->SetState(State::Starting, result);
                } else {
                    this->SetState(State::Error, result);
                }
                os::SleepThread(WorkerPollInterval);
                continue;
            }

            if (inventory_cadence.Due(impl::UsbNowNs())) {
                reader::CaptureInventory(m_client, m_acquired_interfaces, sizeof(m_acquired_interfaces), impl::UsbNowNs());
                reader::CaptureDeviceMap(m_client, impl::UsbNowNs());
            }
            if (available_count <= 0) {
                this->SetWaiting(ResultSuccess());
                os::SleepThread(WorkerPollInterval);
                continue;
            }

            this->SetState(State::Mounting, ResultSuccess());

            Result probe_result = usb::ResultFileNotFound();
            bool selected = false;
            bool startup_failed = false;
            bool empty_media = false;
            bool nonempty_probe = false;
            mass_storage::Geometry selected_geometry{};
            mass_storage::InquiryData selected_inquiry{};
            volume::Partition selected_partition{};
            FileSystemType selected_filesystem_type = FileSystemType::Unknown;
            size_t selected_partition_index = 0;
            size_t selected_partition_count = 0;
            s32 selected_interface_id = -1;
            u8 selected_lun = 0;

            const size_t probe_interface_count = std::min<size_t>(available_count, util::size(m_available_interfaces));
            for (size_t interface_index = 0; interface_index < probe_interface_count && !selected && !this->ShouldStop(); ++interface_index) {
                const u64 slot_key = reader::SlotKey(m_available_interfaces[interface_index]);
                const int saved_lun = slot_preferences::Lookup(slot_key);
                const bool has_preference = saved_lun > 0 && saved_lun < static_cast<int>(MaxLunCount);
                // A preferred pass is exactly ONE normal, fully validated mount
                // attempt. Failure never suppresses the existing full scan.
                for (unsigned pass = has_preference ? 0 : 1; pass < 2 && !selected && !this->ShouldStop(); ++pass) {
                const bool preferred_pass = pass == 0;
                size_t probe_lun_count = 1;
                bool reset_next_lun = has_preference && !preferred_pass;
                bool slot_recovery_used = false;
                if (preferred_pass) probe_lun_count = static_cast<size_t>(saved_lun) + 1;
                if (reset_next_lun) {
                    // Preferred initialization/mount failed. Close every old
                    // handle before the fresh-session reset on normal slot 0.
                    this->Disconnect();
                    slot_preferences::Forget(slot_key);
                    slot_preferences::fallbacks.fetch_add(1);
                    slot_preferences::last_result.store(probe_result.GetValue());
                    io_diagnostics::slot_recovery_attempts.fetch_add(1);
                    io_diagnostics::slot_recovery_stage.store(1);
                    io_diagnostics::slot_recovery_from_lun.store(static_cast<u32>(saved_lun));
                    io_diagnostics::slot_recovery_to_lun.store(0);
                    io_diagnostics::slot_recovery_reset_result.store(UINT32_MAX);
                    io_diagnostics::slot_recovery_time_ns.store(impl::UsbNowNs());
                    const auto &device = m_available_interfaces[interface_index];
                    io_diagnostics::slot_recovery_vid.store(device.device_desc.idVendor);
                    io_diagnostics::slot_recovery_pid.store(device.device_desc.idProduct);
                    io_diagnostics::slot_recovery_device_id.store(device.deviceID);
                }
                io_diagnostics::reader_lun_count.store(1);
                io_diagnostics::reader_empty_lun_mask.store(0);
                for (size_t lun_index = preferred_pass ? static_cast<size_t>(saved_lun) : 0;
                     lun_index < probe_lun_count && (!preferred_pass || lun_index == static_cast<size_t>(saved_lun)) && !selected && !this->ShouldStop(); ++lun_index) {
                    Result initialize_result = ResultSuccess();
                    mass_storage::LunProbeInfo probe{};
                    if (reset_next_lun) {
                        io_diagnostics::slot_recovery_stage.store(2);
                        const auto refreshed = reader::RefreshSame(m_client, filter, std::addressof(m_available_interfaces[interface_index]));
                        io_diagnostics::slot_recovery_result.store(refreshed.GetValue());
                        if (R_FAILED(refreshed)) {
                            io_diagnostics::slot_recovery_stage.store(6);
                            probe_result = refreshed;
                            startup_failed = true;
                            break;
                        }
                        if (this->ShouldStop()) break;
                    }
                    const bool mapped_startup = lun_index == 0 && startup_profile::Matches(
                        m_available_interfaces[interface_index].device_desc, m_available_interfaces[interface_index].inf);
                    initialize_result = startup_profile::Attempt(mapped_startup, reset_next_lun, [&](bool reset) {
                        mapping::BeginAttempt(m_available_interfaces[interface_index], static_cast<u32>(lun_index), impl::UsbNowNs());
                        std::scoped_lock lk(m_volume_mutex);
                        const auto result = m_device.Initialize(std::addressof(m_client), std::addressof(m_available_interfaces[interface_index]), static_cast<u8>(lun_index), std::addressof(probe), reset);
                        mapping::EndAttempt(probe.lun_count, probe.medium_absent, result.GetValue());
                        return result;
                    }, [&]() -> Result {
                        if (this->ShouldStop()) return usb::ResultResourceBusy();
                        const auto refreshed = reader::RefreshSame(m_client, filter, std::addressof(m_available_interfaces[interface_index]));
                        if (R_FAILED(refreshed)) return refreshed;
                        if (this->ShouldStop()) return usb::ResultResourceBusy();
                        return ResultSuccess();
                    }, [&]() { return this->ShouldStop(); });
                    // One same-alternate recovery for an initial transport
                    // error only. The old Initialize has destroyed its handles.
                    // Healthy devices, C8's own profile, preferred slots, UAS,
                    // composites and live read/write failures never enter here.
                    const bool recover_interface = !mapped_startup && !reset_next_lun && !preferred_pass && lun_index == 0 &&
                        !m_device.IsInitialized() && !probe.medium_absent &&
                        (usb::ResultTransactionError::Includes(initialize_result) || svc::ResultTimedOut::Includes(initialize_result)) &&
                        interface_recovery::Eligible(m_available_interfaces[interface_index]);
                    initialize_result = interface_recovery::Attempt(recover_interface, initialize_result,
                        [&]() { return interface_recovery_budget.Take(m_available_interfaces[interface_index]); },
                        [&]() { return reader::RefreshSame(m_client, filter, std::addressof(m_available_interfaces[interface_index])); },
                        [&]() {
                            mapping::BeginAttempt(m_available_interfaces[interface_index], 0, impl::UsbNowNs());
                            std::scoped_lock lk(m_volume_mutex);
                            return m_device.Initialize(std::addressof(m_client), std::addressof(m_available_interfaces[interface_index]),
                                0, std::addressof(probe), false, true);
                        }, [&]() { return this->ShouldStop(); });
                    if (reset_next_lun) {
                        io_diagnostics::slot_recovery_result.store(initialize_result.GetValue());
                        if (io_diagnostics::slot_recovery_stage.load() != 7)
                            io_diagnostics::slot_recovery_stage.store(R_SUCCEEDED(initialize_result) ? 5 : 8);
                        reset_next_lun = false;
                    }
                    mapping::EndAttempt(probe.lun_count, probe.medium_absent, initialize_result.GetValue());
                    if (preferred_pass) io_diagnostics::reader_lun_count.store(probe.lun_count);
                    if (lun_index == 0) {
                        probe_lun_count = std::clamp<size_t>(probe.lun_count, 1, MaxLunCount);
                        io_diagnostics::reader_lun_count.store(static_cast<u32>(probe_lun_count));
                    }
                    this->SetProbeResult(ProbeStage::DeviceInitialize, initialize_result, static_cast<u32>(available_count));
                    if (R_FAILED(initialize_result)) {
                        probe_result = initialize_result;
                        if (preferred_pass) break;
                        if (probe.medium_absent) {
                            // Initialize has already released all endpoints.
                            // Advance only across the validated inventory.
                            empty_media = true;
                            io_diagnostics::reader_empty_lun_mask.fetch_or(1u << lun_index);
                            continue;
                        }
                        startup_failed = true;
                        if (probe.transport_failed && !slot_recovery_used && !m_device.IsInitialized() &&
                            probe.lun_count == probe_lun_count && lun_index + 1 < probe_lun_count) {
                            // One unknown/failed-slot handoff per interface per
                            // cycle. Do NOT label it empty or reuse its stream.
                            slot_recovery_used = true;
                            reset_next_lun = true;
                            io_diagnostics::slot_recovery_attempts.fetch_add(1);
                            io_diagnostics::slot_recovery_stage.store(1);
                            io_diagnostics::slot_recovery_from_lun.store(static_cast<u32>(lun_index));
                            io_diagnostics::slot_recovery_to_lun.store(static_cast<u32>(lun_index + 1));
                            io_diagnostics::slot_recovery_result.store(initialize_result.GetValue());
                            io_diagnostics::slot_recovery_reset_result.store(UINT32_MAX);
                            io_diagnostics::slot_recovery_time_ns.store(impl::UsbNowNs());
                            const auto &device = m_available_interfaces[interface_index];
                            io_diagnostics::slot_recovery_vid.store(device.device_desc.idVendor);
                            io_diagnostics::slot_recovery_pid.store(device.device_desc.idProduct);
                            io_diagnostics::slot_recovery_device_id.store(device.deviceID);
                            continue;
                        }
                        // A timeout/broken stream is not evidence of an empty
                        // slot. Stop this interface; do not multiply recovery.
                        break;
                    }
                    nonempty_probe = true;
                    this->AdvanceMediaGeneration();

                    mass_storage::InquiryData inquiry{};
                    Result inquiry_result = ResultSuccess();
                    {
                        std::scoped_lock lk(m_volume_mutex);
                        inquiry_result = m_device.GetInquiry(std::addressof(inquiry));
                        // The new RBC fallback is for direct-access block
                        // media, not optical/tape/vendor peripheral types.
                        if (R_SUCCEEDED(inquiry_result) &&
                            m_available_interfaces[interface_index].inf.interface_desc.bInterfaceSubClass == 0x01 &&
                            inquiry.peripheral_device_type != 0) {
                            inquiry_result = usb::ResultOperationDenied();
                        }
                    }
                    this->SetProbeResult(ProbeStage::GetInquiry, inquiry_result, static_cast<u32>(available_count));
                    if (R_FAILED(inquiry_result)) {
                        probe_result = inquiry_result;
                        this->Disconnect();
                        continue;
                    }

                    size_t partition_count = 0;
                    Result partition_result = ResultSuccess();
                    {
                        std::scoped_lock lk(m_volume_mutex);
                        partition_result = volume::DiscoverPartitions(std::addressof(m_device), m_partitions, util::size(m_partitions), std::addressof(partition_count));
                    }
                    this->SetProbeResult(ProbeStage::DiscoverPartitions, partition_result, static_cast<u32>(available_count));
                    if (R_FAILED(partition_result) || partition_count == 0) {
                        probe_result = partition_count == 0 ? static_cast<Result>(usb::ResultFileNotFound()) : partition_result;
                        this->Disconnect();
                        continue;
                    }

                    size_t partition_index = 0;
                    const size_t probe_partition_count = std::min<size_t>(partition_count, util::size(m_partitions));
                    FileSystemType filesystem_type = FileSystemType::Unknown;
                    Result volume_result = usb::ResultUnsupportedFilesystem();
                    {
                        std::scoped_lock lk(m_volume_mutex);
                        for (size_t i = 0; i < probe_partition_count; ++i) {
                            const auto fat32_result = m_fat32_volume.Initialize(std::addressof(m_device), m_partitions[i]);
                            this->SetProbeResult(ProbeStage::Fat32Mount, fat32_result, static_cast<u32>(available_count));
                            if (R_SUCCEEDED(fat32_result)) {
                                partition_index = i;
                                filesystem_type = FileSystemType::Fat32;
                                volume_result = fat32_result;
                                break;
                            }

                            const auto exfat_result = m_exfat_volume.Initialize(std::addressof(m_device), m_partitions[i]);
                            this->SetProbeResult(ProbeStage::ExFatMount, exfat_result, static_cast<u32>(available_count));
                            if (R_SUCCEEDED(exfat_result)) {
                                partition_index = i;
                                filesystem_type = FileSystemType::ExFat;
                                volume_result = exfat_result;
                                break;
                            }

                            volume_result = exfat_result;
                        }

                        /* The lightweight inspectors cover the common
                         * FAT32/exFAT cases. Fall back to FatFs for FAT12,
                         * FAT16, and otherwise valid FAT volumes whose boot
                         * layout is not understood by those inspectors. */
                        if (R_FAILED(volume_result) && !m_read_only) {
                            partition_index = 0;
                            /* Prefer the first partition that FatFs can
                             * actually mount, rather than assuming the
                             * first partition is the usable one. */
                            for (size_t i = 0; i < probe_partition_count; ++i) {
                                const auto writable_result = m_writable_volume.Initialize(std::addressof(m_device), m_partitions[i]);
                                this->SetProbeResult(ProbeStage::WritableMount, writable_result, static_cast<u32>(available_count));
                                if (R_SUCCEEDED(writable_result)) {
                                    partition_index = i;
                                    /* Keep the public status ABI unchanged. The fallback accepts
                                     * FAT12/FAT16 and bridge-specific FAT layouts through FatFs. */
                                    filesystem_type = FileSystemType::Fat32;
                                    volume_result = writable_result;
                                    m_writable_ready = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (R_FAILED(volume_result)) {
                        probe_result = volume_result;
                        this->Disconnect();
                        continue;
                    }

                    selected = true;
                    selected_geometry = m_device.GetGeometry();
                    selected_inquiry = inquiry;
                    selected_partition = m_partitions[partition_index];
                    selected_filesystem_type = filesystem_type;
                    selected_partition_index = partition_index;
                    selected_partition_count = partition_count;
                    selected_interface_id = m_device.GetInterfaceId();
                    selected_lun = static_cast<u8>(lun_index);
                    {
                        std::scoped_lock lk(m_volume_mutex);
                        m_filesystem_type = selected_filesystem_type;
                        if (m_read_only) {
                            m_writable_ready = false;
                        } else if (!m_writable_volume.IsInitialized()) {
                            const auto writable_result = m_writable_volume.Initialize(std::addressof(m_device), selected_partition);
                            this->SetProbeResult(ProbeStage::WritableMount, writable_result, static_cast<u32>(available_count));
                            m_writable_ready = R_SUCCEEDED(writable_result);
                        } else {
                            m_writable_ready = true;
                        }
                    }
                }
                if (selected) {
                    // Learn only after capacity, inquiry and filesystem mount
                    // succeeded. No learning from TUR/partial initialization.
                    slot_preferences::Remember(slot_key, selected_lun);
                    if (preferred_pass) slot_preferences::last_result.store(0);
                }
                }
            }

            if (!selected) {
                this->Disconnect();
                if (empty_media && !nonempty_probe && !startup_failed) {
                    this->SetWaiting(ResultSuccess());
                    startup_backoff.EmptyReader(impl::UsbNowNs());
                    io_diagnostics::probe_cooldown_ms.store(1000);
                } else {
                    this->SetState(State::Error, probe_result);
                }
                if (startup_failed) {
                    io_diagnostics::probe_failed_cycles.fetch_add(1);
                    io_diagnostics::probe_cooldown_ms.store(startup_backoff.Failed(impl::UsbNowNs()));
                }
                os::SleepThread(WorkerPollInterval);
                continue;
            }

            startup_backoff.Reset();
            io_diagnostics::probe_cooldown_ms.store(0);
            {
                std::scoped_lock volume_lk(m_volume_mutex);
                m_checkpoint_valid = false;
                m_checkpoint_pending = m_writable_ready && !m_skip_checkpoint_on_next_mount;
                m_skip_checkpoint_on_next_mount = false;
                this->SetReady(selected_geometry, selected_inquiry, selected_partition, selected_filesystem_type, selected_partition_index, selected_partition_count, selected_interface_id, selected_lun, m_writable_ready);
            }
            TryArmSleepCheckpoint();
            os::SleepThread(WorkerPollInterval);
        }

        // Stop may be initiated while the wake worker owns the root host
        // client. Wait for its cancellation/return before finalizing children.
        { std::unique_lock lk(m_power_mutex);
          while (m_resume_in_progress) m_power_cv.Wait(m_power_mutex); }
        this->Disconnect();
        if (m_client.IsInitialized()) {
            static_cast<void>(m_client.Finalize());
        }
    }

    bool Service::ShouldStop() const {
        std::scoped_lock lk(m_mutex);
        return m_stop_requested;
    }

    void Service::SetState(State state, Result result) {
        std::scoped_lock lk(m_mutex);
        m_status.state = state;
        m_status.last_result = static_cast<Result>(result).GetValue();
    }

    void Service::SetWaiting(Result result) {
        std::scoped_lock lk(m_mutex);
        m_status.state = State::WaitingForDevice;
        m_status.last_result = static_cast<Result>(result).GetValue();
        m_status.block_count = 0;
        m_status.block_size = 0;
        m_status.partition_first_lba = 0;
        m_status.partition_sector_count = 0;
        m_status.partition_style = 0;
        m_status.partition_index = 0;
        m_status.partition_count = 0;
        m_status.lun = 0;
        m_status.writable = 0;
        m_status.interface_id = -1;
        m_status.filesystem_type = FileSystemType::Unknown;
        m_status.media_generation = m_media_generation;
        std::memset(m_status.partition_name, 0, sizeof(m_status.partition_name));
        std::memset(m_status.vendor, 0, sizeof(m_status.vendor));
        std::memset(m_status.product, 0, sizeof(m_status.product));
        std::memset(m_status.revision, 0, sizeof(m_status.revision));
    }

    void Service::SetProbeResult(ProbeStage stage, Result result, u32 available_count, u32 acquired_count) {
        std::scoped_lock lk(m_mutex);
        m_status.probe_stage = static_cast<u32>(stage);
        m_status.probe_result = static_cast<Result>(result).GetValue();
        m_status.available_interface_count = available_count;
        m_status.acquired_interface_count = acquired_count;
        ++m_status.probe_attempts;
    }

    void Service::SetReady(const mass_storage::Geometry &geometry, const mass_storage::InquiryData &inquiry, const volume::Partition &partition, FileSystemType filesystem_type, size_t partition_index, size_t partition_count, s32 interface_id, u8 lun, bool writable) {
        std::scoped_lock lk(m_mutex);
        m_status.state = State::Ready;
        m_status.last_result = ResultSuccess().GetValue();
        m_status.media_generation = m_media_generation;
        m_status.block_count = geometry.block_count;
        m_status.block_size = geometry.block_size;
        m_status.partition_first_lba = partition.first_lba;
        m_status.partition_sector_count = partition.sector_count;
        m_status.partition_style = static_cast<u8>(partition.style);
        m_status.partition_index = static_cast<u8>(partition_index);
        m_status.partition_count = static_cast<u8>(partition_count > 0xFF ? 0xFF : partition_count);
        m_status.lun = lun;
        m_status.writable = writable;
        m_status.interface_id = interface_id;
        m_status.filesystem_type = filesystem_type;
        std::memcpy(m_status.partition_name, partition.name, sizeof(m_status.partition_name));
        std::memcpy(m_status.vendor, inquiry.vendor, sizeof(m_status.vendor));
        std::memcpy(m_status.product, inquiry.product, sizeof(m_status.product));
        std::memcpy(m_status.revision, inquiry.revision, sizeof(m_status.revision));
        m_status.partition_name[sizeof(m_status.partition_name) - 1] = '\0';
        m_status.vendor[sizeof(m_status.vendor) - 1] = '\0';
        m_status.product[sizeof(m_status.product) - 1] = '\0';
        m_status.revision[sizeof(m_status.revision) - 1] = '\0';
    }

    void Service::CompleteIoLocked(Result result) {
        std::scoped_lock lk(m_mutex);
        m_status.last_io_result = result.GetValue();
        if (!ShouldDisconnectAfterIo(result) || m_status.state != State::Ready) return;
        runtime_diagnostics::disconnect_reason.store(1, std::memory_order_relaxed);
        m_device.Quarantine();
        // Invalidate all old handles and reject unpinned operations before
        // another caller can enter the volume. Worker alone tears it down.
        m_writable_ready = false;
        m_filesystem_type = FileSystemType::Unknown;
        if (++m_media_generation == 0) ++m_media_generation;
        write_transfer::Invalidate();
        completion_probe::Invalidate();
        host_write::Invalidate();
        m_eject.NewGeneration(m_media_generation);
        m_status.media_generation = m_media_generation;
        m_status.state = State::Error;
        m_status.last_result = result.GetValue();
        m_status.writable = false;
        if (!m_reconnect_requested) io_diagnostics::reconnect_requests.fetch_add(1, std::memory_order_relaxed);
        m_reconnect_requested = true;
    }

    void Service::SetLastIoResult(Result result) {
        std::scoped_lock lk(m_mutex);
        m_status.last_io_result = static_cast<Result>(result).GetValue();
    }

    void Service::RequestReconnect() {
        std::scoped_lock lk(m_mutex);
        if (!m_reconnect_requested) io_diagnostics::reconnect_requests.fetch_add(1, std::memory_order_relaxed);
        m_reconnect_requested = true;
    }

    bool Service::ConsumeReconnectRequest() {
        std::scoped_lock lk(m_mutex);
        const bool requested = m_reconnect_requested;
        m_reconnect_requested = false;
        return requested;
    }

    void Service::AdvanceMediaGeneration() {
        std::scoped_lock lk(m_mutex);
        write_transfer::Invalidate();
        completion_probe::Invalidate();
        host_write::Invalidate();
        ++m_media_generation;
        if (m_media_generation == 0) {
            ++m_media_generation;
        }
        m_status.media_generation = m_media_generation;
        m_eject.NewGeneration(m_media_generation);
    }

    void Service::Disconnect() {
        std::scoped_lock lk(m_volume_mutex);
        m_checkpoint_valid = false;
        m_checkpoint_pending = false;
        // Stop while parked must not flush dirty handles into a sleeping host.
        if (IsSystemPowerPaused()) m_device.Quarantine();
        const bool had_media = m_device.IsInitialized() || m_fat32_volume.IsInitialized() || m_exfat_volume.IsInitialized() || m_writable_volume.IsInitialized();
        if (m_writable_volume.IsInitialized()) {
            static_cast<void>(m_writable_volume.Finalize());
        }
        m_writable_ready = false;
        if (m_fat32_volume.IsInitialized()) {
            static_cast<void>(m_fat32_volume.Finalize());
        }
        if (m_exfat_volume.IsInitialized()) {
            static_cast<void>(m_exfat_volume.Finalize());
        }
        m_filesystem_type = FileSystemType::Unknown;
        if (m_device.IsInitialized()) {
            static_cast<void>(m_device.Finalize());
        }
        if (had_media) {
            /* Invalidate open read-only handles as soon as the backing media is gone. */
            this->AdvanceMediaGeneration();
        }
    }

}

#else

namespace ams::usb::storage {

    Service::Service(bool read_only) : m_read_only(read_only) {
        m_status.state = State::Stopped;
        m_status.last_io_result = ResultSuccess().GetValue();
        m_status.media_generation = 0;
        m_status.interface_id = -1;
    }

    Service::~Service() = default;

    Result Service::Start() {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::Stop() {
        R_RETURN(usb::ResultNotInitialized());
    }

    Result Service::PrepareSystemSleep() { R_SUCCEED(); }
    Result Service::ReserveSystemWake() { R_SUCCEED(); }
    Result Service::ResumeSystemSleep() { R_SUCCEED(); }
    void Service::FailSystemPower(Result) { }
    void Service::AllowFailedWakeRecovery() { }
    void Service::DenyFailedWakeRecovery() { }
    bool Service::IsRecoveredFromFailedWake() const { return false; }
    bool Service::IsSystemPowerPaused() const { return false; }
    bool Service::IsSystemPowerQuiescent() const { return true; }

    Result Service::GetStatus(Status *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        *out = m_status;
        R_SUCCEED();
    }

    Result Service::GetMediaGeneration(u64 *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        *out = 0;
        R_SUCCEED();
    }

    Result Service::GetFileInfo(const char *, volume::FileInfo *) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::ReadFile(const char *, u64, void *, size_t, size_t *) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::ReadDirectory(const char *, volume::DirectoryEntryInfo *, size_t, size_t, bool, bool, size_t *) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::GetFileInfoAtGeneration(const char *, volume::FileInfo *, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::InvalidateReadCacheAtGeneration(u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::ReadFileAtGeneration(const char *, u64, void *, size_t, size_t *, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::ReadDirectoryAtGeneration(const char *, volume::DirectoryEntryInfo *, size_t, size_t, bool, bool, size_t *, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::GetSpaceSize(u64 *, u64 *) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::IsWritable(bool *out) const {
        R_UNLESS(out != nullptr, usb::ResultInvalidParameter());
        *out = false;
        R_SUCCEED();
    }

    Result Service::CreateFileAtGeneration(const char *, u64, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::ResizeFileAtGeneration(const char *, u64, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::DeleteFileAtGeneration(const char *, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::RenameFileAtGeneration(const char *, const char *, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::WriteFileAtGeneration(const char *, u64, const void *, size_t, size_t *out_written, u64) {
        R_UNLESS(out_written != nullptr, usb::ResultInvalidParameter());
        *out_written = 0;
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::CreateDirectoryAtGeneration(const char *, u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    Result Service::FlushAtGeneration(u64) {
        R_RETURN(usb::ResultOperationDenied());
    }

    bool Service::IsRunning() const {
        return false;
    }

    void Service::ThreadEntry(void *) { }
    Result Service::AcquireEjectLease(u64) { R_THROW(usb::ResultOperationDenied()); }
    void Service::ReleaseEjectLease(u64) { }
    eject::Status Service::GetEjectStatus() const { return {}; }
    Result Service::RequestEject(u64,u64) { R_THROW(usb::ResultOperationDenied()); }
    Result Service::ResumeAfterEject() { R_THROW(usb::ResultOperationDenied()); }
    Result Service::ControlKeepAliveTest(NexusUsbKeepAliveStatus *, u64, const NexusUsbKeepAliveRequest &) { R_THROW(usb::ResultOperationDenied()); }
    Result Service::ControlWriteTransferTest(NexusUsbWriteTransferStatus *, u64, const NexusUsbWriteTransferRequest &) { R_THROW(usb::ResultOperationDenied()); }
    Result Service::ControlCompletionProbeTest(NexusUsbCompletionProbeStatus *, u64, const NexusUsbCompletionProbeRequest &) { R_THROW(usb::ResultOperationDenied()); }
    Result Service::ControlHostWriteTest(NexusUsbHostWriteStatus *, u64, const NexusUsbHostWriteRequest &) { R_THROW(usb::ResultOperationDenied()); }
    bool Service::ProcessEject() { return false; }
    void Service::WorkerThread() { }
    bool Service::ShouldStop() const { return true; }
    void Service::SetState(State, Result) { }
    void Service::SetWaiting(Result) { }
    void Service::SetReady(const mass_storage::Geometry &, const mass_storage::InquiryData &, const volume::Partition &, FileSystemType, size_t, size_t, s32, u8, bool) { }
    void Service::SetLastIoResult(Result) { }
    void Service::RequestReconnect() { }
    bool Service::ConsumeReconnectRequest() { return false; }
    Result Service::CheckMediaGenerationLocked(u64) const { R_SUCCEED(); }
    void Service::AdvanceMediaGeneration() { }
    void Service::Disconnect() { }

}

#endif
