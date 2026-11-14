#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run SL7 production sleep cores and USB transport policy on the host.

No native USB device is opened. Clock, endpoint replies and power requests are
deterministic fixtures. Receipts pin the tested production and fixture sources.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def artifact_path(path):
    value = str(path)
    if value.startswith('/mnt/') and len(value) > 7 and value[6] == '/':
        return value[5].upper() + ':' + value[6:]
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        parser.error('Host C++20 compiler required; use --cxx.')
    project = Path(__file__).resolve().parents[2]
    library = project / 'libraries/libstratosphere'
    module = project / 'stratosphere/ams_mitm/source/usb_mitm'
    out = (args.out or project / 'tests/nexus-integration/sl7-results').resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_path = library / 'source/usb/usb_mass_storage.cpp'
    source_bytes = source_path.read_bytes()
    source = source_bytes.decode()
    start = source.index('    Result Device::ExecuteCommand(')
    end = source.index('    Result Device::ExecuteCommandOnce(', start)
    body = source[start:end]
    body = body[body.index('{') + 1:body.rfind('}')]
    (out / 'actual_execute_command.hpp').write_text(body)
    for method, following, header in [('ReadWakeCapacity', 'SynchronizeCacheForEject', 'actual_read_wake_capacity.hpp'),
                                       ('CaptureWakeSerial', 'GetMaxLun', 'actual_capture_wake_serial.hpp'),
                                       ('SynchronizeCacheForEject', 'ExecuteSleepRead', 'actual_sync_cache.hpp'),
                                       ('ExecuteSleepRead', 'CaptureSleepFingerprint', 'actual_execute_sleep_read.hpp'),
                                       ('CaptureSleepFingerprint', 'KeepAlive', 'actual_capture_sleep_fingerprint.hpp')]:
        start = source.index('    Result Device::' + method + '(')
        end = source.index('    Result Device::' + following + '(', start)
        text = source[start:end]
        (out / header).write_text(text[text.index('{') + 1:text.rfind('}')])
    for method, boundary, header in [('ExecuteCommandOnce', '        auto *cbw', 'actual_bot_submit_gate.hpp'),
                                     ('ExecuteCommandUasOnce', '        auto *command_iu', 'actual_uas_submit_gate.hpp')]:
        start = source.index('    Result Device::' + method + '(')
        text = source[start:source.index(boundary, start)]
        (out / header).write_text(text[text.index('{') + 1:])
    storage_path = library / 'source/usb/usb_storage.cpp'
    storage_bytes = storage_path.read_bytes()
    storage_source = storage_bytes.decode()
    for method in ['BeginSystemOperation', 'EndSystemOperation', 'IsSystemPowerPaused', 'IsSystemPowerQuiescent',
                   'FailSystemPower', 'CaptureRetainedInterface', 'CaptureSleepWitness', 'TryArmSleepCheckpoint',
                   'PrepareSystemSleep', 'VerifySleepMarker', 'RebindAfterSleep', 'CreateFileAtGeneration',
                   'ResizeFileAtGeneration', 'DeleteFileAtGeneration', 'RenameFileAtGeneration',
                   'WriteFileAtGeneration', 'CreateDirectoryAtGeneration',
                   'ReserveSystemWake', 'InvalidateFailedWake', 'ResumeSystemSleep', 'CompleteIoLocked']:
        start = re.search(r'    (?:Result|void|bool) Service::' + method + r'\(', storage_source).start()
        following = re.search(r'\n    (?:Result|void|bool) Service::', storage_source[start + 1:])
        text = storage_source[start:start + 1 + following.start()]
        (out / ('actual_service_' + method + '.hpp')).write_text(text[text.index('{') + 1:text.rfind('}')])
    start = storage_source.index('        bool ShouldDisconnectAfterIo(')
    end = storage_source.index('\n        }', start) + len('\n        }')
    (out / 'actual_io_fault_policy.hpp').write_text(storage_source[start:end])
    start = storage_source.index('                m_checkpoint_valid = false;', storage_source.index('            startup_backoff.Reset();\n            io_diagnostics::probe_cooldown_ms.store(0);'))
    end = storage_source.index('                this->SetReady(', start)
    (out / 'actual_mount_checkpoint_state.hpp').write_text(storage_source[start:end])
    start = storage_source.index('        std::scoped_lock lk(m_mutex);', storage_source.index('    Result Service::Start()'))
    end = storage_source.index('        m_stop_requested = false;', start)
    (out / 'actual_start_guards.hpp').write_text(storage_source[start:end])
    start = storage_source.index('        { std::unique_lock power_lk', storage_source.index('    Result Service::Stop()'))
    end = storage_source.index('        os::WaitThread(', start)
    (out / 'actual_stop_guards.hpp').write_text(storage_source[start:end])
    writable_path = library / 'source/usb/usb_writable_volume.cpp'
    writable_source = writable_path.read_text()
    start = writable_source.index('    Result WritableVolume::WriteSleepMarker(')
    start = writable_source.index('{', start) + 1
    end = writable_source.index('\n    }', start)
    (out / 'actual_write_sleep_marker.hpp').write_text(writable_source[start:end])
    jobs = ['usb_sleep_transport_tests.cpp', 'usb_sleep_core_tests.cpp', 'usb_system_power_tests.cpp', 'usb_sleep_service_tests.cpp',
            'usb_sleep_marker_tests.cpp']
    missing = [name for name in jobs if not (project / 'tests/nexus-integration' / name).exists()]
    if missing:
        print('Pending independent fixtures:', ', '.join(missing))
    records = []
    files = {source_path, storage_path, library / 'source/usb/usb_scsi_recovery.hpp', Path(__file__)}
    files.update(project / 'tests/nexus-integration' / name for name in jobs if (project / 'tests/nexus-integration' / name).exists())
    files.update(p for p in (library / 'include/stratosphere/usb/usb_wake_identity.hpp',
                            library / 'include/stratosphere/usb/usb_resume_marker.hpp',
                            library / 'include/stratosphere/usb/usb_reader_policy.hpp',
                            library / 'source/usb/usb_writable_volume.cpp',
                            library / 'include/stratosphere/usb/usb_sleep_core.hpp',
                            library / 'include/stratosphere/usb/usb_sleep_diagnostics.hpp',
                            module / 'usb_system_power_core.hpp') if p.exists())
    pins = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    pins[source_path] = hashlib.sha256(source_bytes).hexdigest()
    pins[storage_path] = hashlib.sha256(storage_bytes).hexdigest()
    for name in jobs:
        fixture = project / 'tests/nexus-integration' / name
        if not fixture.exists():
            continue
        for optimization in ('O0', 'O2'):
            stem = fixture.stem + '-' + optimization
            binary = out / (stem + ('.exe' if os.name == 'nt' else ''))
            command = [compiler, '-std=c++20', '-' + optimization, '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-I' + str(out), '-I' + str(library / 'include'), '-I' + str(library / 'source/usb'),
                       '-I' + str(module), str(fixture), '-o', str(binary)]
            build = subprocess.run(command, text=True, capture_output=True)
            (out / (stem + '-compile.log')).write_text(build.stdout + build.stderr)
            if build.returncode:
                print(build.stdout + build.stderr)
                build.check_returncode()
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=60)
            log = run.stdout + run.stderr
            (out / (stem + '.log')).write_text(log)
            print(log, end='')
            run.check_returncode()
            match = re.search(r'PASS (\d+) ', log)
            if not match:
                raise RuntimeError('Test did not emit assertion count: ' + stem)
            records.append({'name': fixture.stem, 'optimization': optimization, 'checks': int(match[1]),
                            'command': command, 'log': artifact_path(out / (stem + '.log')), 'passed': True})
    for path, expected in pins.items():
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError('Source changed during tests; rerun before publishing receipt: ' + str(path))
    receipt = {'passed': not missing, 'build': 'SL7', 'generated_utc': datetime.now(timezone.utc).isoformat(),
               'hardware_tested': False, 'drive_writes': False, 'missing_fixtures': missing, 'tests/nexus-integration': records,
               'total_checks': sum(t['checks'] for t in records),
               'files': {artifact_path(p): pins[p] for p in sorted(files)},
               'scope': ['Production storage admission and PSC coordinator headers compiled directly with exhaustive phase and sense-policy checks.',
                         'Four concurrent background workers across 1000 close/drain/open cycles; no admissions in acknowledged quiet windows.',
                         'Verbatim Service power lifecycle bodies with injected query, drain, flush, sync, identity and fingerprint failures.',
                         'Verbatim ordinary ExecuteCommand, explicit wake read/fingerprint and sync bodies plus BOT/UAS submission admission prefixes.',
                         'The wake acknowledgement precedes background recovery; repeated wake and overlapping sleep requests are exercised.',
                         'Ordinary failed writes never replay; dirty submission tracking and successful-cache-sync clearance tested for BOT and UAS.'],
               'limits': ['Host tests do not establish Switch PSC delivery/order or successful physical USB sleep recovery.',
                          'Transport policy compiles the entire production ExecuteCommand body; endpoint IPC and clock are fixtures.',
                          'Fingerprint harness uses a byte-sensitive hash stand-in; native SHA correctness is outside this test.',
                          'Stop notification is simulated through the production gate and condition interface; native thread teardown is not executed.',
                          'Target build, integration review and hardware sleep tests are recorded separately.']}
    (out / 'sleep-test-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
