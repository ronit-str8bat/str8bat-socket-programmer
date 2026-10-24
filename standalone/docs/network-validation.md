# Network firmware build validation

## Current: local-ap-v9 GP6 SWD translator OE (2026-10-01)

Candidate: `build-local-hotspot/standalone_programmer-gp6-oe-local-ap-v9-gp18.uf2`.
UF2 SHA-256: `499e3adc58ee16377f80fd3454e82af158f018e94c3f04b8751d21973989403d`.
UF2 size: 1,876,480 bytes; application payload: 937,888 bytes.

The user reported v8 working, then explicitly requested GP6 for TXS0102 OE.
This supersedes the earlier GP4 request; GP4 remains unused. GP18 remains START.

- SWD initialization drives GP6 LOW before preparing the transport. The first
  SWD direction setup enables OE after idle pin states are set, before wire
  clocks, with a 2 us settling delay. OE remains HIGH in either data direction.
- Success/failure release drives OE LOW, waits 2 us, then releases SWD pins.
  Reconnection disables OE during reinitialization and enables it on use.
  NRST remains independently controlled. GP6 is included in pin collision checks.
- TI TXS0102 datasheet sections 5.6 and 7.3.4 specify active-high OE and 200 ns
  enable/disable timing; the selected delay adds margin. An external OE pulldown
  is needed to hold the translator disabled before firmware starts.
  Source: https://www.ti.com/lit/ds/symlink/txs0102.pdf
- GPIO trace tests check OE HIGH and settled at every clock, retained through
  direction changes, LOW at initialization/release and restored on subsequent
  access. Tests run through bit-bang and PIO mock transports. All programmer,
  button, CRC, SWD and embedding tests passed. SDK build and artifact checks passed.
- stm32_programmer.c, the target BIN and SRAM loader were not changed in this
  update. The user-confirmed BFB2 clock/halt recovery remains in place.
- D: filled during UF2 generation. Removed only the incomplete generic UF2 and
  the regenerable build-network/standalone_programmer.dis, then rebuilt and
  validated successfully. Renamed the generic UF2 to the candidate path to avoid
  another copy. Previous named UF2 files remain available.
- Not uploaded or tested with the physical translator during this update.

## Previous: local-ap-v8 restore debug enable and request halt (2026-10-01)

Candidate: `build-local-hotspot/standalone_programmer-bfb2-halt-fix-local-ap-v8-gp18.uf2`.
UF2 SHA-256: `a3c1e443508ffc095cf4448a6bdede1abf52938d8548326af7d195ff2e49af54`.
UF2 size: 1,875,968 bytes; application payload: 937,584 bytes.

Read-only COM10 report from the connected Pico:

```
PICO-PROGRAMMER local-ap-v7 result=6 stage=0 address=e000edf0 detail=e000edf0 loader=0 completed=0 retry=0 fast_rows=0 elapsed_ms=1334 halted=0
OPTIONS before=fffff8aa after=ffeff8aa bfb2_attempted=1 bfb2_cleared=0 phase=8 dhcsr=01010000 reconnects=1
```

SWD recovered, but C_DEBUGEN/C_HALT/S_HALT were clear and S_RETIRE_ST was set.
Polling for S_HALT alone cannot stop a running core with debug disabled. The
source now restores DEMCR vector catch and writes DHCSR DBGKEY|C_DEBUGEN|C_HALT
on entry to phase 8 and after reconnecting. It verifies both debug controls and
S_HALT on two consecutive reads. Only these idempotent debug-control writes can
be repeated; option programming and OBL_LAUNCH remain single-attempt operations.

- Added a model of debug controls lost at option reload, with the observed
  running DHCSR status. The regression fails with v7 and passes with v8, also
  with a transient communication error. Permanent halt/control failures still
  stop within the deadline before main-flash erase. All host test variants pass.
- SDK build and UF2/embedded-image validation passed. Target BIN and SRAM loader
  are unchanged. No v8 hardware upload or programming was performed.
- Vector catch can be lost at reload; software cannot claim the old application
  never ran during the recovery gap. Exact option/UID verification after halt
  remains mandatory and fails closed if the running code changed an option.
- D: reported insufficient space when copying the UF2. The already validated
  generic UF2 was renamed to the candidate path instead. Its final SHA matches;
  `build-local-hotspot/standalone_programmer.uf2` is absent until the next build.

## Previous: local-ap-v7 reconnect while waiting for reload halt (2026-10-01)

Candidate: `build-local-hotspot/standalone_programmer-bfb2-reconnect-local-ap-v7-gp18.uf2`.
UF2 SHA-256: `b41b389d067d725fcfbf773743ee908be1fd9899819c85bc7bd0bc93137bf8fc`.
UF2 size: 1,875,968 bytes; application payload: 937,584 bytes.

Read the connected Pico on COM10 using read-standalone-report.ps1 (no flash/reset
command sent). The repeating report was:

```
PICO-PROGRAMMER local-ap-v6 result=2 stage=0 address=e000edf0 detail=00000003 loader=0 completed=0 retry=0 fast_rows=0 elapsed_ms=567 halted=0
OPTIONS before=fffff8aa after=ffeff8aa bfb2_attempted=1 bfb2_cleared=0 phase=8
```

This locates the failure after OBL_LAUNCH and a valid fresh-reset status, while
waiting for halt. BAD_ACK caused the generic wait_bits helper to exit immediately.
It does not prove the BFB2 value after reload; option verification was not reached.

- Phase 8 now uses a bounded status-read loop. On communication failure it
  reinitializes the Pico SWD transport (preserving NRST), reconnects at 100 kHz,
  and retries status reads. Requires two consecutive halted observations.
- No option programming or OBL_LAUNCH is repeated. No physical target reset or
  forced resume is added. Same-target UID and all nine options must still match
  before bfb2_cleared=1 or any main-flash erase. The v6 4 MHz workaround remains.
- OPTIONS adds dhcsr (last successfully read reload status) and reconnects
  (phase-8 reconnection attempt count). Neither alone proves BFB2 was cleared.
- A regression injecting temporary errors after the valid reset status failed
  against v6 and passes with v7 for BAD_ACK, parity, FAULT and PIO timeout.
  Permanent failures remain bounded; recovery still rejects unchanged or
  corrupted option storage without erasing main flash. All host programmer,
  SWD and local AP/clock/CSV tests, SDK build and artifact validation passed.
- The target BIN and SRAM loader remain byte-for-byte unchanged. No v7 upload
  or physical programming test has been performed. The cause and duration of
  the physical communication failure remain unproven; the test demonstrates
  recovery if it is temporary, not a fix for persistent electrical faults.

Bench check: install v7 via BOOTSEL and press GP18. Repeat after target power
cycling. If red returns, read the report while leaving the Pico powered; retain
PICO-PROGRAMMER and OPTIONS including dhcsr/reconnects.

## Previous: local-ap-v6 option-reload clock fix (2026-10-01)

Candidate: `build-local-hotspot/standalone_programmer-bfb2-clock-fix-local-ap-v6-gp18.uf2`.
UF2 SHA-256: `30da99d21d107b834487f45ea9a179005a6cd29433768df42db7c25c65dd6f44`.
UF2 size: 1,875,456 bytes; application payload: 937,376 bytes.

Confirmed defect: v4/v5 called the 24 MHz MSI clock setup before BFB2 option
programming and OBL_LAUNCH. ST ES0250 revision 13, section 2.2.23, states that
option loading with MSI enabled at >= 8 MHz can corrupt loaded options and
engineering bytes. The documented workaround is MSI at 4 MHz or below before
OBL_LAUNCH. This affects both silicon revisions listed for STM32L476.
Source: https://www.st.com/resource/en/errata_sheet/es0250-stm32l476xxstm32l486xx-device-errata-stmicroelectronics.pdf

- BFB2 handling now selects and reads back MSI range 6 (4 MHz), with SWD at
  100 kHz. It uses the existing safe HSI transition, PLL shutdown and voltage
  readiness checks. A failed clock change stops before option unlock or writes.
- Main-flash setup selects range 9 (24 MHz) and SWD at 2 MHz only after option
  reload, halt, UID, all-option comparison and lock verification have succeeded.
- BFB2 remains the only option bit changed. No broad option erase, RDP change,
  protection removal, or replay of uncertain option writes was introduced.
- A new regression asserting 4 MHz at OPTSTRT/OBL_LAUNCH failed on the old code.
  It passes with the fix. Tests also require 24 MHz at main erase, cover initial
  MSI/HSI/HSE/PLL and low-power clocks, and stop before any option write if the
  4 MHz range cannot be established. All host test variants passed.
- Pico SDK build and UF2 payload validation passed. The original 524,288-byte
  v2.0.3 target BIN and 808-byte SRAM loader are byte-for-byte unchanged.
- This confirms a code defect, not that it explains every previous parity error
  or why BFB2 returned after power cycling. Physical validation is still pending.
  The diagnostic build holds the CPU halted after success; power cycling allows
  the flashed application to run. Its behavior after startup was not inspected.

Bench check: load this v6 UF2, leave BFB2 enabled initially, and press GP18 START.
Expected successful report: `local-ap-v6`, `result=0`, `bfb2_attempted=1`,
`bfb2_cleared=1`. For initial `fffff8aa`, after must be `ffeff8aa`. Then repeat
after cycling target power to check the reported failure sequence on hardware.
If it fails, retain both PICO-PROGRAMMER and OPTIONS lines; do not infer a
successful option reload from the after value alone.

## Previous: local-ap-v5 source review corrections (2026-10-01)

Candidate: `build-local-hotspot/standalone_programmer-reviewed-local-ap-v5-gp18.uf2`.
UF2 SHA-256: `c619805ac71bc6f0a2d433cae6d8a88c7f5833329062ee5deb35e7cda32719fb`.
UF2 size: 1,875,456 bytes; application payload: 937,352 bytes.

- Reinitializing SWD preserves an existing NRST hold. Previously `gpio_init`
  released NRST briefly before the next attempt asserted it again.
- Initial halt failures preserve SWD error details instead of relabeling every
  failed read as a reset fault. A halt timeout still reports a reset failure.
- Option phases now distinguish 4 (preparing reload), 7 (reload command
  attempted/reconnecting), 8 (waiting for halt) and 9 (verifying reloaded state).
  Existing 1/2/3/5/6 retain their meanings. Phase 7 does not prove the reload
  command reached the target. Only `bfb2_cleared=1` confirms the complete check.
- The user's v4 `result=2 detail=4 address=e000edf0 phase=4` means a DHCSR
  communication parity error, but v4 cannot identify which reload substep.
  `after=ffeff8aa` may only be a shadow read before reload. This is not proof
  that BFB2 is durably cleared. Main flash has not been erased in that attempt.
- Regression tests cover reset preservation and parity failures at initial halt,
  before reload, reconnect and after reload. All host programmer/SWD and local
  AP/clock/CSV tests passed. SDK build and artifact validation passed.
- The embedded 524,288-byte v2.0.3 image is byte-for-byte unchanged. GP18 remains
  START; GP4/GP6 remain unused. No hardware was flashed in this review.
- These corrections do not establish or fix the physical cause of the parity
  error. Reset/reload behavior still requires hardware qualification. Direct
  3.3 V Pico to approximately 1.8 V target wiring remains an unresolved signal
  compatibility concern; software tests cannot validate its voltage margins.

## Previous: local-ap-v4 BFB2-only correction (2026-10-01)

Candidate: `build-local-hotspot/standalone_programmer-bfb2-clear-gp18-v2.0.3.uf2`.
UF2 SHA-256: `87841ff62a0d54ae051eb85a54d48c5abcdbdb77761ed58c477358c88674e4ee`.
UF2 size: 1,872,896 bytes; application payload: 936,176 bytes.

- Embeds the unchanged 524,288-byte `str8bat_factory_v2.0_user_v2.0.3.bin`.
- START stays GP18. GP4/GP6 remain unused; no translator control was restored.
- During connection, validates target/UID/size/RDP/WRP/PCROP, then clears only
  FLASH_OPTR.BFB2 if set. No option writes occur if BFB2 is already clear.
- Snapshots all nine option registers. Requires stable reads, unlocked control,
  fresh EOP with EOPIE, bounded completion, reset vector catch, a fresh reset
  indication, the same target UID, a halted CPU and exact reloaded options.
- No main-flash erase/upload follows an option failure. A standard programming
  retry never repeats option writes. Options are checked again before erase and
  after image verification. RDP/WRP/PCROP/DualBank and all other option bits remain
  unchanged; unsupported protection still stops the job.
- USB `OPTIONS` line reports OPTR before/after, attempted/cleared flags and phase.
  Phases: 1 read, 2 unlock, 3 option programming, 4 reload/reconnect, 5 verified,
  6 final check. `bfb2_cleared=1` means verified after reload, not just shadow write.
- Host programmer tests passed for preservation, already-clear skip, blocked
  protection, unlock failure, programming error, busy timeout, missing EOP,
  absent reload, reset-disrupted SWD acknowledgement, link/halt/UID failures,
  unchanged or altered nonvolatile options, unstable reads and final changes.
- SWD/CRC/button, local Wi-Fi/clock/CSV tests, SDK build and UF2 payload checks passed.
- Not uploaded or tested on physical hardware. Option reload/reset timing and
  supply integrity still require a bench test; this does not establish the cause
  of the user's earlier CubeProgrammer option-write failure.

Bench check: install this candidate, confirm report ID `local-ap-v4`, then START.
For an initial OPTR `FFFFF8AA`, success must report `after=ffeff8aa`,
`bfb2_attempted=1 bfb2_cleared=1`, followed by a successful full programming result.
An initially clear BFB2 reports both flags zero. If it fails, retain the main
result and OPTIONS lines; do not assume the option change succeeded.

## Previous: local-ap-v3 hardware timekeeping and POST /sync-time

Candidate: `build-local-hotspot/standalone_programmer-local-ap-v3-halt.uf2`.
SHA-256: `32b30eb5baa9b01eda5dd267460f620f6bb43a38b3086eac66bafb423e6be813`.
UF2: 1,870,336 bytes; application payload: 934,824 bytes.

- Uses RP2350 POWMAN hardware milliseconds with the crystal-derived tick;
  no software uptime offset supplies calendar timestamps now.
- POST /sync-time initializes/checks hardware; /time remains an alias.
- Browser auto-syncs only an unset clock; a valid clock survives phone
  disconnection/reconnection and advances without HTTP or software polling.
- CSV stays IST; elapsed flashing duration remains separately measured.
- SDK build, hardware-symbol link check and final UF2 verification passed.
- Host tests passed: six hours without Wi-Fi, next job's advanced timestamp,
  reconnect without reset, timer stop/start/readback faults, resync, 2038/date
  rollovers, token and busy rejection, legacy alias, CSV and download regression.
- These use a hardware-counter model, not a physical six-hour board test.
  Upload, actual radio/RTC operation and drift qualification remain unperformed.
- Same requested UF2 filename replaced; boot report ID remains local-ap-v3.

## Previous: local-ap-v3 with IST timestamps (software uptime offset)

Updated candidate: `build-local-hotspot/standalone_programmer-local-ap-v3-halt.uf2`.
SHA-256: `db4e103a07f2dd2e4e4053955ef0e1bdeaf35ddf995c42683f8bd5f02310f7cb`.
UF2: 1,869,312 bytes; application payload: 934,152 bytes.

- Displays IST (UTC+05:30); new CSV column is Date&Time(IST). Phone Unix input
  and Pico-measured elapsed seconds remain unchanged.
- Preserves previous UTC logs in a separate downloadable UTC archive.
- IST offset, date rollover, log migration/recovery and portal tests passed.
- SDK compilation and final UF2/embedded-image/partition validation passed.
- Replaced the same named v3 file at the user's explicit request. Build ID is
  still local-ap-v3; this SHA distinguishes it from the earlier UTC build.
- No hardware upload or phone test performed.

## Previous: local-ap-v3 UTC (Android captive-portal CSV download)

Candidate: `build-local-hotspot/standalone_programmer-local-ap-v3-halt.uf2`.
SHA-256: `a2127a12490921c3aff52186f61f8f28ba5561739bc5b732a3e67915335b4269`.
UF2: 1,868,288 bytes; application payload: 933,728 bytes.

- Replaces the browser-only blob URL with HEAD metadata plus a direct HTTP
  attachment bound to the CSV ETag. Normal GET /logs API remains available.
- Handles WebView-to-download-service connection handoff and final ACK+FIN.
- Tests passed: HEAD does not authorize clear, interrupted handoff releases
  storage, stale download is refused, final delivery authorizes only the correct
  file, exact streamed CSV, and existing clock/storage/association regressions.
- SDK compilation and UF2 validation passed; embedded production bytes match.
- Baseline SHA checks confirm unchanged LED, Wi-Fi, clock, CSV storage and
  flashing sources, and the prior named v2 UF2. No upload or phone test performed.

## Previous: local-ap-v2 (requested LED / Str8bat hotspot corrections)

Candidate: `build-local-hotspot/standalone_programmer-local-ap-v2-halt.uf2`.
SHA-256: `227c4a2c3665fccfd500c679aca9791b7607f868282e76b6a1102f3c214715b4`.
UF2: 1,866,752 bytes; application payload: 933,072 bytes.

- Exact WPA2 SSID `Str8bat_Pico`, password `str8bat@pico`.
- CRC phase off; startup failure blinking red; ready blinking green when a
  client is associated, alternating green/red otherwise; programming solid
  blue; completed success/failure solid green/red. Onboard LED unchanged.
- Common HTTP captive probes redirect to the local portal instead of 403.
  Actual automatic display still depends on the phone OS/settings.
- Phone-set UTC date and Pico-measured duration in seconds retained; opening
  the page supplies the phone clock. Native apps use the existing POST /time.
- SDK compilation and all local clock/CSV/portal tests passed, including new
  captive redirect, foreign-host protection, exact AP identity, live association,
  disconnect/query-error and phone-time propagation tests.
- Final UF2 validation passed; target binary matches the source exactly.
- SHA comparisons confirm SWD/programming/loader sources, local CSV storage,
  clock code and named v1 UF2 are unchanged. No upload or hardware test performed.

See [local-hotspot-programmer.md](local-hotspot-programmer.md) for operation.

## Previous: local-ap-v1 (always-on local hotspot)

See [local-hotspot-programmer.md](local-hotspot-programmer.md) for the current
LED behavior, phone workflow, six-column CSV, app API and build instructions.
This profile replaces STA fallback/Internet services with a permanent WPA2 AP.
Flashing works immediately; timestamps are UNSET until the phone sets UTC.
The named candidate is
`build-local-hotspot/standalone_programmer-local-ap-v1-halt.uf2`.
SHA-256: `e8fbd83256cdd3381d37a66fa2e9b8ffbbfc704139dc52b9b7bcb501518ab9c0`.
UF2 size: 1,864,192 bytes; application payload: 931,728 bytes.
SDK compilation, local HTTP/clock/LittleFS fault tests, existing programmer/SWD/
debounce regressions and the final UF2/stack/partition checks passed.
Its machine-readable payload checks and SHA-256 are in
`build-local-hotspot/uf2-validation.json`. Host tests and compilation do not
replace fixture qualification; no new UF2 has been uploaded by this work.

## Previous: network-v2-csv (Wi-Fi CSV download)

Candidate: `build-network/standalone_programmer-network-v2-csv-halt.uf2`

- SHA-256: `25f3cb4a878580d2d216783a8620d3e5eb152b1aa3ef8c4ec6eaba68ddd63f27`
- UF2 size: 1,859,584 bytes; executable payload: 929,304 bytes.
- Adds **Download programming logs** to the Pico hotspot page at
  `http://192.168.4.1`. Exports pending and current rows as one CSV.
- Flash partition layout, credential key and STM32 payload are unchanged.
- Keeps the target halted after programming, as before.
- SDK build and final UF2/payload/stack-layout validation passed.
- Host storage tests confirm empty and combined exports, arbitrary read
  boundaries, mutation locking and unchanged flash after downloading.
- Tests of the actual portal with simulated TCP confirm a complete 150 KB
  download, fragmented requests, backpressure, failed TCP close retry,
  host/token validation, busy refusal and disconnect/timeout/START cleanup.
- Earlier network-v1 named UF2 remains unchanged. No board was uploaded;
  actual phone/browser/radio testing remains to be done.

Use a full browser on a phone or laptop connected to the **Pico's hotspot**.
Install this candidate first; network-v1 has no CSV download link. Downloading
retains local records and does not acknowledge or clear cloud-upload batches.

## Previous: network-v1

Candidate: `build-network/standalone_programmer-network-v1-halt.uf2`

- UF2 SHA-256: `969a44174970a3146193144fe947c4c7cd17ca723b08e5e9af2b9aa8395a09cf`
- UF2 size: 1,853,952 bytes; executable flash payload: 926,616 bytes.
- Embedded STM32 image: 524,288 bytes, exactly matching the supplied binary.
- STM32 image SHA-256: `3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9`
- SRAM loader: 808 bytes, byte-identical to the earlier tested build.
- Keeps the STM32 halted after successful verification.
- OTA and cloud endpoints/CA are unconfigured in this candidate.
- Setup AP uses the documented demonstration password; change it before deployment.

Completed checks:

- SDK 2.3.1 Pico 2 W build, with compiler warnings treated as errors.
- Existing programmer tests: fast/standard, halt/restart, full production
  image verification, SWD error handling and the new UID reads.
- Strict manifest/HTTPS URL/HTTP header/form parsing and 20,000 malformed inputs.
- Actual storage and LittleFS code against a host NOR model: fragmented OTA,
  checksum rejection, revision checks, partial last flash page, interrupted
  erase/write/commit, credential tamper detection, log acknowledgement retention,
  capacity refusal and power loss during result/marker updates.
- Final UF2 decoding matches the generated Pico binary. The embedded source
  binary and loader match exactly; no UF2 payload enters reserved flash storage.
- Link map confirms separate core stacks and heap; executable ends before slot A.
- Earlier `standalone_programmer-fast-v3-halt.uf2` remains unchanged at SHA-256
  `c3af2eb625a5ace5c235ece31c728e8cb620c7d698605ad3af3e783768982e0b`.

No Pico was flashed and no target was programmed during this implementation.
Radio/AP/browser behaviour, real TLS/NTP exchanges, cloud ingestion and actual
power-cut recovery remain bench qualification tasks. Host crypto tests use an
OpenSSL adapter; the compiled device firmware uses SDK mbedTLS.
