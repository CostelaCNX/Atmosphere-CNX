SL1 independent sleep/wake host tests

Run: python3 tests/run_usb_sleep_tests.py --cxx g++ --out <local-results-folder>
Requires a host C++20 compiler with threads. Builds O0 and O2 with warnings as
errors. No Switch, SD card or USB device is opened. The result folder contains
compile logs, test logs, generated verbatim source fragments and a SHA256 receipt.
Inputs are checked for concurrent edits before emitting a passing receipt.

usb_sleep_core_tests.cpp compiles the actual storage admission/reset-policy core.
It covers admitted-work drain, closed phases, stop/failure, repeated and cancelled
cycles, reachable-state invariants, overflow-safe deadlines and narrow current
power-reset sense acceptance. Deferred/medium-change sense is rejected.

usb_system_power_tests.cpp compiles the actual PSC coordinator/background gate.
It verifies ordering, no ACK while work remains, repeated/aborted/unknown states,
failed prepare/resume/ACK, SD-only failure reporting and concurrent close/drain.

usb_sleep_service_tests.cpp compiles the complete production Service sleep/wake
method bodies with deterministic native-state/USB/FS fixtures. Failures are
injected at each preparation boundary and each retained-interface identity
field, plus fingerprint/geometry errors, closed/missing sessions, temporary
absence, drain timeout and blocked-call release. Stop is simulated at the gate;
native worker/thread teardown is reviewed and target-built separately.

usb_sleep_transport_tests.cpp compiles the complete production ordinary command
policy and explicit sleep read/fingerprint/sync bodies. It preserves ordinary
write no-replay behavior, tests the single clean-resume reset allowance, shared
deadline and readiness bound, READ CAPACITY10/16 and READ10/16 layout, and dirty
submission tracking in the actual BOT/UAS entry prefixes. Hashing is a
byte-sensitive stand-in; native SHA and controller/device behavior are outside
this harness.

Passing host checks do not prove successful physical sleep recovery. Target
validation still needs normal SD and USB game launch, short/prolonged sleep,
repeated cycles and intentional unplug during sleep. Re-enumerated or changed
interfaces remain unsupported and fail closed; the tests do not endorse blind
rebinding or recovery of uncertain writes.
