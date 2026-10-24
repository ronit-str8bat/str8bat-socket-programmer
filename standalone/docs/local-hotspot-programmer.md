# Local hotspot programmer — local-ap-v9

This build embeds the STM32 production binary and creates an always-on WPA2
hotspot. Each Pico independently programs one socket. It does not attempt to
join another Wi-Fi network and does not start Internet OTA, NTP, or cloud upload.
USB power is sufficient for standalone operation; USB diagnostics remain optional.

## Use from a phone or laptop — no terminal required

1. Install `build-local-hotspot/standalone_programmer-gp6-oe-local-ap-v9-gp18.uf2` on the
   **Pico 2 W** using BOOTSEL. This replaces its current application. Do not use
   a flash-erase UF2: that would also remove stored logs.
2. After startup, connect to **Str8bat_Pico**. The name has no suffix. The
   password is **str8bat@pico**, unless overridden in your private config.
   Keep the connection
   when the phone warns that the network has no Internet.
3. Tap the phone's **Sign in to network** notification or its automatic portal
   page. If it does not open, browse to **http://192.168.4.1/**. The page initializes an unset hardware clock from the phone and offers
   **Download CSV** and **Clear downloaded logs**.
4. Flashing also works without opening the page. In that case dates are `UNSET`
   until the app supplies time. The clock loses its date on power loss; it is
   not a battery-backed RTC. Previously saved `UNSET` values are not rewritten.
5. Wait for programming to finish before downloading. Save the downloaded CSV
   successfully, then use **Clear downloaded logs** only if you want to remove
   those records. Downloading alone never deletes them.

Before main-flash erase, this build clears **BFB2 only** if enabled. It reloads
the options with reset vector catch enabled and checks all option settings before
continuing. RDP, DualBank, watchdog, BOR, write protection and PCROP settings are
preserved. Any failure stops programming; no option writes occur when BFB2 is
already disabled. A final options check also runs before success is reported.

Version v6 fixes the MSI clock during BFB2 handling: 4 MHz for option programming
and reload, followed by 24 MHz for main-flash programming. This follows the
STM32L476 ES0250 section 2.2.23 workaround for option loading at high MSI speeds.

Version v7 also reconnects SWD after temporary communication failures while
waiting for the reload halt. This wait is bounded and does not repeat option
writes. The options report adds `dhcsr` (last valid status) and `reconnects`
(phase-8 reconnection attempts). BFB2 is still checked before flash erase.

Version v8 restores debug enable and requests halt after reload/reconnection.
The v7 bench report showed a readable, running core with debugging disabled;
polling alone timed out. All options are still verified after regaining halt.

The USB report includes `OPTIONS before=... after=... bfb2_attempted=...
bfb2_cleared=... phase=...`. The cleared flag is set only after reload verification.
`after` is the last option-register read; by itself it does not prove reload.
Reload diagnostics use phase 4 for preparation, 7 for the reload command and
reconnection, 8 for waiting for halt, and 9 for checking the reloaded settings.
Phase 5 means reload verification finished; phase 6 is the final options check.
For example, `FFFFF8AA` becomes `FFEFF8AA`. This change is performed once per job
at most and is not retried during standard-programming recovery.

The supplied build leaves the STM32 **halted after verification**, retaining the
previous diagnostic behavior. For CubeProgrammer comparison, keep the target
powered and connect with Hot Plug without resetting it. A target reset or power
cycle runs its application, which may modify its own flash.

## LEDs and button

The external RGB LED is **common cathode**, connected to GND. Each color has its
own series resistor. Pins are Pico GPIO names, with physical header pins below.

| Connection | GPIO | Physical pin |
|---|---:|---:|
| STM32 SWCLK | GP2 | 4 |
| STM32 SWDIO | GP3 | 5 |
| STM32 NRST | GP1 | 2 |
| TXS0102 SWD translator OE | GP6 | 9 |
| START switch to GND | GP18 | 24 |
| Red LED anode through resistor | GP7 | 10 |
| Green LED anode through resistor | GP8 | 11 |
| Blue LED anode through resistor | GP9 | 12 |
| Common ground | GND | 3 or another GND pin |

GP6 drives the SWD translator OE: LOW at startup/idle, HIGH before SWD clocks,
and LOW when SWD is released on success or failure. It stays HIGH for both reads
and writes; reconnect initialization disables it until the next SWD exchange.
The code waits 2 microseconds after changing OE. Use an OE pulldown for the
period before Pico firmware starts, as specified by the TXS0102 datasheet.
Target power remains external. GPIO names are not ZIF contact numbers.

| State | External RGB |
|---|---|
| Boot image CRC calculation | All off |
| CRC passed, waiting for first START, phone connected | Blinking green |
| CRC passed, waiting for first START, no Wi-Fi client connected | Alternating green / red |
| Startup/checksum failed | Blinking red |
| Validated START; connecting, erasing, writing, verifying | Solid blue |
| Programming, full verification and log save passed | Solid green |
| Any failure, including inability to save the result | Solid red |

START is active-low with a pull-up and a strict 100 ms nonblocking debounce.
GP4 is unused; GP6 controls SWD OE and START remains GP18. GP13 power-enable control is not configured.
GP1 can still hold NRST LOW after a failure. Provide target power and a working
NRST path independently of GP4/GP6.
Release the button for at least 100 ms after boot and between attempts. Holding
it down does not repeatedly program. START cancels an active CSV download; retry
the download after the job. The onboard LED stays off during checksum and then
shows a heartbeat, or a repeating error count. Error 10 denotes logging failure
or full storage. Wi-Fi detection uses live AP association checks every 500 ms while idle, not
a stored DHCP lease. Any associated device counts as connected. After a job,
the solid green/red result remains until the next START; Wi-Fi does not
override it. The optional require-time setting uses error 11; it is **off**
in this build, as requested.

## CSV format and persistence

The file is `/programming_logs.csv` inside LittleFS on the Pico, not a Windows
drive. It has exactly these six columns:

```csv
Target UID,Date&Time(IST),Elapsed Time(sec),firmware_version,Binary Size(Bytes),status
000000010000000200000003,2026-09-29 05:30:00,15.588,v4.0.2,524288,SUCCESS
000000010000000200000003,UNSET,15.588,v4.0.2,524288,FAIL
```

These are illustrative rows, not measurements of the new Wi-Fi build. Time is
**IST (UTC+05:30) at START**, initialized from the phone and then read from the running hardware clock.
A native mobile app sends `POST /sync-time` once after Pico startup (Wi-Fi
association alone does not transmit the phone clock). Wi-Fi disconnects do not
stop or invalidate this clock. Duration is the SWD sequence duration in **seconds with three
decimal places**, taken from the programmer's monotonic elapsed milliseconds;
it excludes waiting for the button, boot, download and subsequent CSV storage.
Version is the build-time `TARGET_FIRMWARE_VERSION` label for the embedded binary,
not a version queried from the running STM32 application.

The 96-bit UID is read from `0x1FFF7590`, `0x1FFF7594`, and `0x1FFF7598` and
formatted as three eight-digit hexadecimal words in address order. It is
`UNAVAILABLE` if connection fails before the UID can be read. A durable job
marker records attempts before any target erase. An interrupted attempt is
recovered as `FAIL`; its UID is `UNAVAILABLE` and duration `0.000` because power
loss prevents measuring the actual finish time. That value means unavailable
for this recovery case, not a zero-time programming measurement.

LittleFS commits an internal last-job attribute with each CSV row so a reboot
after saving the row but before removing the marker cannot duplicate the row.
Writes use the Pico SDK's multicore flash-safe execution mechanism. No log
storage mutation occurs while the SWD worker is programming.

The active CSV is capped at 256 KiB. Once full, new attempts are refused before
erasing the target. Download and clear it to continue; older records are never
silently overwritten. Clearing replaces the file atomically with a header-only
CSV. It is a logical clear, not secure physical erasure of every old flash cell.

Older network-build CSVs are preserved as `programming_logs.v1.csv` and
`upload.v1.csv`, available through the two archive links. These have the old
schema. `DELETE /logs` clears only the current CSV, not archives. A legacy
interrupted-job marker is retained as `inflight.v1.job`; a corrupt existing
filesystem or conflicting archive is not automatically formatted or overwritten.

## Mobile app API

Use HTTP/1.1 with `Host: 192.168.4.1` (or `192.168.4.1:80`). The AP accepts one
HTTP session at a time, closes each response, and does not route Internet
traffic. Make requests sequentially and retry transport-busy failures. WPA2
protects the wireless link; use a private AP password. There is no TLS or
separate per-user authentication on this local API. No permissive CORS headers
are enabled. Native apps should enable their platform's local-network HTTP
access for this address.

| Request | Result |
|---|---|
| `GET /` | Browser download/clear/time page |
| `GET /status` | JSON build, Pico ID, firmware label, busy flag, IST clock, status and boot token |
| `POST /sync-time` | Initialize/resynchronize hardware clock with phone Unix seconds |
| `POST /time` | Compatibility alias for `/sync-time` |
| `GET /logs` | CSV attachment, exact Content-Length and quoted SHA-256 ETag |
| `DELETE /logs` | Clear the previously downloaded, unchanged current CSV |
| `GET /logs/legacy-current` | Old-schema archive, if present |
| `GET /logs/legacy-pending` | Old pending-upload archive, if present |

Clock request:

```http
POST /sync-time HTTP/1.1
Host: 192.168.4.1
X-Programmer-Token: <token from GET /status>
Content-Type: application/json
Content-Length: 27

{"unix_seconds":1790640000}
```

Set Content-Length to the actual encoded body's size (the example body above
is 27 bytes). Accepted years are 2025 through 2099. Success returns 204. Time
setting is refused with 409 while programming; a job's start timestamp is stable.

Safe download/delete sequence:

1. Fetch `/status` and retain its `token` (changes on Pico reboot).
2. `GET /logs`; save the **entire response body successfully** and retain the
   quoted `ETag` header. The Pico streams 1 KiB payload blocks; RAM use does
   not grow with the log file. It computes the ETag in bounded blocks too.
3. Only after the app confirms local save, send:

```http
DELETE /logs HTTP/1.1
Host: 192.168.4.1
X-Programmer-Token: <token from GET /status>
If-Match: "<64-character SHA-256 ETag from GET /logs>"
```

204 means cleared. Missing If-Match returns 428; a missing token returns 403.
No completed download returns 409. A new record after the download returns 412
and preserves all records: download the new file first. Storage failure returns
503. A broken or timed-out transfer never newly authorizes deletion. The Pico
can confirm TCP delivery but cannot confirm the phone saved a file; sending
DELETE is the app's explicit save confirmation. If delivery of the DELETE
response is uncertain, GET the CSV again to determine whether it was cleared.

No automatic deletion, cloud upload, or deletion of the embedded firmware occurs.

## Source layout and build

| File | Purpose |
|---|---|
| `include/wifi_config.h` | Exact AP name, password, allow-UNSET policy |
| `src/services/local_network.c` | Always-on AP and programming/log integration |
| `src/services/local_portal.c` | Bounded HTTP server, streaming, ETag/delete protocol |
| `src/services/local_clock.c` | RP2350 POWMAN hardware clock and IST display formatting |
| `src/services/local_storage.c` | LittleFS, six-column CSV, migration and atomic clear |
| `src/main.c` | CRC, LEDs, core ownership, 100 ms button-triggered jobs |
| `src/stm32_programmer.c`, `src/swd_pio.c`, `loader/stm32l476_loader.c` | Existing verified SWD/flash path |
| `CMakeLists.txt` | `STANDALONE_LOCAL_AP`, SDK/LittleFS linkage, generated version |
| `../build-local-hotspot.ps1` | Builds the local profile into a separate directory |

The standalone project uses `pico_cyw43_arch_lwip_poll`, `pico_flash`,
`hardware_flash`, `pico_multicore`, LittleFS and SHA-256. Local sources replace
the Internet OTA/provisioning/cloud implementation for this profile. The
embedded image remains a build-generated `const uint8_t` array; the network
cannot replace it. CMake checks the image and version inputs. The linker guards
the persistent partitions and reserves a 16 KiB core-0 stack for networking.

Flash layout is unchanged: application below `0x180000`, legacy OTA slots
reserved, LittleFS at `0x282000..0x3FEFFF`, legacy credentials at `0x3FF000`.
The new UF2 writes only application pages; existing storage stays intact.

Build from PowerShell in the project folder when source/configuration changes:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-local-hotspot.ps1
```

`-FirmwarePath` and `-FirmwareVersion` select another target image and label.
`-RunTargetAfterVerify` explicitly selects restarting the target instead of
leaving it halted. Do not use that switch during flash-comparison diagnostics.

## Qualification

Host regression tests exercise real LittleFS against a NOR model, torn physical
writes during append and clear, UTC/UNSET formatting, CSV fields, schema
migration, ETag guards, a 150 KB streamed HTTP response, backpressure, busy and
disconnection handling. Existing SWD/programmer tests cover 16 KiB transfers,
64-bit alignment, page boundaries including `0x0807E800`, fast-mode recovery,
WAIT/FAULT handling, 512 KiB verification and debounce.

The fast programmer path is retained: 16 KiB SRAM transfers, 256-byte fast rows,
8-byte aligned standard writes for sparse rows, skipping erased all-FF
doublewords, followed by **full byte-by-byte readback**. No region is excluded
from verification. Earlier application-written differences are not hidden.

This new Wi-Fi build still needs fixture tests: hotspot operation throughout
SWD, phone download/save/delete, clock setup, restart persistence, repeated jobs,
independent CubeProgrammer comparison while halted, and four-device coexistence.
It has not been uploaded or hardware-qualified by these host tests. Earlier
15.588-second timing is not a measured guarantee for the new network build.

## Captive-portal discovery in local-ap-v2

Android, Apple and Windows HTTP connectivity-probe paths now receive a 302
redirect to `http://192.168.4.1/`, including HTTP/1.0 Apple probes. The old
403 response on those discovery paths prevented portal detection. CSV, status
and time endpoints still require the local Host; redirects do not disclose logs
or access tokens under a foreign host. HTTPS is not intercepted.

Automatic display is controlled by the phone OS and its saved network settings.
A sign-in notification may appear instead of a browser popup; a no-Internet
warning is expected because this is still a local-only hotspot. The manual
address above remains available. See [Android captive-portal detection](https://developer.android.com/about/versions/11/features/captive-portal)
and [Apple captive-network behavior](https://support.apple.com/en-au/102554).

Changes from v1 are limited to the requested LED states, live connection
indication, exact SSID/password and captive discovery, with documentation/tests.
The phone-set clock, CSV schema, flash algorithm, erase/verification, pin map
and halt-after-verify behavior are retained.

## Android CSV download correction in local-ap-v3

Download CSV now uses `HEAD /logs` to read its size and ETag, then navigates to
`GET /logs?etag=<64-character hash>`. This is a real HTTP attachment named
`programming_logs.csv`; no browser-only blob URL or random filename is used.
Android's captive-portal download service can request this URL on the Pico
network. Select a save location if the phone asks. If the sign-in window still
blocks downloads, keep the Wi-Fi connection and open `http://192.168.4.1/` in
a full browser such as Chrome.

The page cannot observe whether the phone saved the attachment. Clear is offered
after starting the download, but the user must confirm the file was saved; the
server still requires delivery of the full CSV and the matching ETag. HEAD
alone, an interrupted download, or an outdated file cannot authorize clearing.
If records change between HEAD and GET, the server responds 412; return to the
page and download again. Native app `GET /logs` remains supported unchanged.

Only the HTTP download path and build identifier changed from v2. LED behavior,
SSID/password, phone clock, elapsed timing, CSV format and SWD programming are
unchanged. SDK build, host tests and artifact checks passed; phone download
verification is still required on the actual device.

[Android downloader implementation](https://android.googlesource.com/platform/packages/modules/CaptivePortalLogin/+/refs/tags/android-platform-15.0.0_r9/src/com/android/captiveportallogin/DownloadService.java)
uses a network URL connection, which is why a JavaScript-only `blob:` URL is
unsuitable for the captive-portal download service.

## IST update ? same local-ap-v3 UF2 filename

The current `standalone_programmer-local-ap-v3-halt.uf2` has been rebuilt with
Indian Standard Time (UTC+05:30) for the page, `/status` (`ist` and `timezone`
fields), and new CSV records (`Date&Time(IST)`). The phone/app continues sending
standard Unix seconds to `/time`: do not add a timezone offset on the phone.
The Pico applies +19,800 seconds only when formatting human-readable dates.
Elapsed flashing seconds and UNSET behavior are unchanged.

Existing UTC CSVs are preserved as `programming_logs.utc.csv`, available via
the **previous UTC logs** link (`GET /logs/utc-archive`). They retain their UTC
header/timestamps. The active CSV starts with an IST header; UTC and IST rows
are never mixed or silently relabelled. Other old archives are preserved too.
A migration interrupted by power loss resumes on reboot, and an already logged
job's saved ID prevents a duplicate recovery row across the UTC archive.

Host tests cover the +05:30 offset, midnight/leap-day/year rollover, CSV
formatting, archive coexistence/reboot recovery, and the HTTP IST status.
The latest SHA-256 in `network-validation.md` identifies this rebuilt file;
the basename and build report ID remain local-ap-v3 as requested.

## Hardware clock and offline timestamps

The Pico 2 W uses RP2350, whose hardware real-time counter is the **POWMAN
always-on timer**, rather than the RP2040 calendar RTC. The local profile now
links the official SDK `hardware_powman` library. `local_time_set()` stops the
timer, selects its crystal-derived 1 kHz source, writes Unix milliseconds to
the 64-bit hardware counter, starts it, and checks running state/readback.
This sets the time reference; it is not an oscillator-frequency calibration.

Every later timestamp is read from `powman_timer_get_ms()`, independently of
Wi-Fi, browser activity, CPU polling intervals, or the phone remaining nearby.
The CSV continues to format IST as `YYYY-MM-DD HH:MM:SS`. Elapsed programming
seconds still come from the separate monotonic timer and cannot jump when the
wall clock is resynchronized. No flash-programming algorithm or LED change is
part of this update.

App integration (same local Host and token rules as existing endpoints):

1. `GET /status` to retrieve `token` and `clock_set`.
2. If unset, `POST /sync-time` with `Content-Type: application/json`,
   `X-Programmer-Token: <token>`, and body `{"unix_seconds":1790640000}`. Send
   current Unix **seconds**, not milliseconds and not an IST-adjusted value.
3. HTTP 204 confirms initialization; `/status` reports `clock_set: true`,
   `clock_source: "RP2350_POWMAN"`, and current `ist`.
4. The phone may disconnect. Later button-triggered jobs still log advancing
   IST timestamps from the hardware. Reconnecting does not reset the clock.
   An explicit POST can resynchronize it while idle.

The browser page performs the same initial synchronization automatically,
but leaves a valid running clock alone when reopened. Its manual clock button
can resynchronize. `/time` remains a compatible alias. Invalid input returns
400, missing/invalid token 403, a busy programmer 409, and a hardware
start/readback failure 503 (rather than falsely reporting clock success).

The current board has no battery-backed supply for timekeeping. After a power
loss or application restart, sync again; until then the existing allowed-offline
policy produces `UNSET`. Previously saved rows are not rewritten. This firmware
does not enter deep sleep or switch off the crystal.

Host tests model hardware milliseconds separately from CPU uptime: six hours
advance without Wi-Fi or polling, subsequent job timestamps advance, and
reconnection leaves the counter unchanged. Tests also cover stopped timer,
start/readback failure, resynchronization, post-2038 dates, IST rollover, and
the new endpoint/legacy alias. Real board RTC drift and phone integration still
require hardware testing.

References: [official RP2350 POWMAN APIs](https://www.raspberrypi.com/documentation/pico-sdk/hardware.html#group_hardware_powman)
and [SDK AON timer implementation](https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/pico_aon_timer/aon_timer.c).
