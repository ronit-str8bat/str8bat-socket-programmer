# Network-enabled standalone programmer

This is an additional Pico 2 W build. It retains the PIO SWD engine, 16 KiB
STM32 RAM staging buffer, STM32 fast row loader and full byte-for-byte readback
verification. Networking does not remove or shorten target verification.
Each of the four Picos runs independently and includes its Pico identity in logs.

The new image is `build-network/standalone_programmer.uf2`. Earlier named
`fast-v3-halt` and other tested UF2s in `build-standalone` are preserved.
This file has **not** been installed or tested on the physical fixture.

## Build and configure

1. Optionally copy `standalone/include/wifi_config.local.h.example` to
   `standalone/include/wifi_config.local.h` and edit it locally.
2. Set a private `WIFI_SETUP_PASSWORD` before deployment. The unconfigured
   bench build uses **PicoSetup-ChangeMe**. This is a shared demonstration
   password, not a production secret.
3. Factory SSID/password may be left empty to provision with a phone. Saved
   credentials override factory credentials on subsequent boots.
4. To enable remote services, configure `OTA_MANIFEST_URL`, `LOG_UPLOAD_URL`,
   `SERVER_CA_PEM` and, when required by your server, `SERVER_BEARER_TOKEN`.
   No actual URLs, CA certificate or authentication token were supplied for
   this implementation. Empty settings disable remote requests.
5. Run from the project directory:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build-network.ps1
   ```

   This compiles the network image in its own directory. It does not program
   either board. Default: keep the STM32 halted after verification, matching
   the last successful external comparison test.

   To build a production image that starts the STM32 application after a
   verified write:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build-network.ps1 -RunTargetAfterVerify
   ```

6. Install the UF2 using Pico BOOTSEL when ready for bench testing. Initial
   installation needs a PC; normal programming, provisioning, logging and
   target-image OTA operate without one.

The automatically generated `standalone/private/device_key.h` is excluded
from Git. Back it up securely with your release configuration and reuse it
for subsequent builds; replacing it makes previously saved credentials
unreadable and returns that device to factory credentials/AP provisioning.
Never publish this header or the local Wi-Fi configuration.

## Phone provisioning: 15 seconds, save, reboot

- After startup checks, the Pico tries its saved network (or factory network)
  for **15 seconds**, without blocking the programming button.
- If it cannot obtain a connected Wi-Fi interface, it opens a WPA2 hotspot:
  **Pico_Programmer_Setup_XXXX**, where `XXXX` is the end of that Pico's ID.
  The suffix distinguishes your four sockets.
- Connect your phone to that hotspot with the configured setup password.
  Accept “stay connected without Internet” if prompted.
- Open **http://192.168.4.1**. Captive DNS is provided; an automatic popup
  depends on the phone, so opening this address manually is supported.
- Type the Wi-Fi name and password. The page currently supports manual entry,
  not a scan/dropdown. Use a **2.4 GHz WPA2 personal** network, password
  8–63 characters, SSID up to 32 bytes. Enterprise and open networks are not
  supported by this provisioning policy.
- Press **Save and restart**. The device encrypts and reads back the saved
  record, replies to the browser, then reboots after about three seconds.
  If saving fails it displays an error and does not reboot. If a programming
  job is active, it asks you to try again when the job finishes.
- Wrong credentials lead back to the setup hotspot after another 15 seconds.
  A lost connection likewise falls back to the hotspot after 15 seconds.

The page is HTTP inside the password-protected local AP. A random form token
guards the credential POST. Credential requests and passwords are not logged.
Credentials are stored with AES-256-GCM and a random nonce, using a build key
bound to the Pico ID. This detects record tampering and avoids plaintext in
that sector; it **does not** protect against an attacker extracting both the
firmware key and QSPI contents. Factory passwords compiled into code are also
extractable. Hardware secure storage is outside this implementation.

## Download the CSV onto a phone or laptop over Wi-Fi

Install the **network-v2-csv** UF2 to enable this feature; the earlier network-v1
image does not have a download link. The partition layout and credential key
are unchanged, so a normal UF2 installation only writes the executable area
and preserves the logs already stored in LittleFS.

1. Connect your phone/laptop to **Pico_Programmer_Setup_XXXX**. Choose to stay
   connected if it reports no Internet access.
2. Open **http://192.168.4.1** in Chrome, Safari or another full browser. If the
   phone's automatic Wi-Fi login popup cannot download files, use the full browser.
3. Tap **Download programming logs** at the top of the page.
4. Save **programming_logs.csv**. Find it in the browser's Downloads or your
   phone's Files app. You can share this file from your phone to your laptop,
   or connect the laptop directly to the Pico hotspot and repeat these steps.

The download includes one CSV header followed by pending `upload.csv` rows and
current `programming_logs.csv` rows. It copies remaining local records; it does
not delete them or acknowledge a cloud upload. If no jobs are recorded, the
download contains only the column header. Records already acknowledged and
removed by cloud synchronization must be retrieved from your server.

The download is available on the **Pico's own hotspot**. When the Pico is a
client of a router or of your phone's hotspot, this AP page is not running.
The existing 15-second fallback behaviour is unchanged.

Download while the programmer is idle. A request during programming returns
“Programmer busy.” Pressing START during a download cancels that transfer so
the programming job can proceed; download again afterwards. Disconnects and
the two-minute download timeout release the read lock and leave records intact.
CSV content is streamed in small blocks, without loading the entire file into
RAM. Download links carry a boot-specific token and accept the AP's literal
address as Host. The hotspot password controls local access to the records.

## Flash partition layout (Pico's 4 MiB, not STM32 addresses)

| Area | Offset | Size / limit |
| --- | --- | --- |
| Executable + immutable factory STM32 image | `0x000000` | up to `0x180000` |
| OTA A commit header | `0x180000` | 4 KiB |
| OTA A image | `0x181000` | 512 KiB |
| OTA B commit header | `0x201000` | 4 KiB |
| OTA B image | `0x202000` | 512 KiB |
| LittleFS logs | `0x282000` | through `0x3FEFFF` |
| Encrypted Wi-Fi record | `0x3FF000` | 4 KiB |

A linker assertion rejects an executable that crosses into slot A. LittleFS
formats automatically only when its complete partition is erased; an existing
corrupt filesystem is preserved and blocks programming, allowing investigation
instead of automatically destroying records. This first deployment requires
unused/erased space in those reserved sectors. Do not erase them casually:
they hold retained logs, saved credentials and OTA images.

The compiled `firmware_image.c` remains a recovery image. Downloaded images
use replaceable slots rather than modifying a C const array in the executable.
At boot, a slot needs a valid header CRC, length, vectors, image CRC32 and
SHA-256. The newest valid revision newer than the factory revision is selected.

OTA erases the **inactive** slot's header first, writes its image in 256-byte
Pico flash pages, validates the complete stored image, then writes its commit
header last. Interrupted or invalid downloads never replace the active image.
The previous slot and compiled factory image remain available. Saved credential
replacement uses one sector: a power loss while replacing it may require
reprovisioning; it does not affect logs or target images.

## OTA server contract

The Pico checks when online with NTP time and a configured trusted CA, then
hourly while idle. It uses **TLS 1.2 HTTPS on port 443**, validates the CA chain,
certificate dates and hostname, and has no insecure HTTP fallback for downloads
or cloud logs. The separate provisioning AP uses local HTTP.

Host `version.json` and the binary on the **same DNS hostname**. Requests use
`Connection: close` and `Accept-Encoding: identity`. Responses must be HTTP 200
with a correct **Content-Length**, no chunked transfer, content compression or
redirect. Manifest maximum is 1535 bytes. Example:

```json
{
  "revision": 40003,
  "size": 524288,
  "sha256": "3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9",
  "url": "https://YOUR-SERVER/firmware/production_firmware_v4.0.2.bin",
  "target": "STM32L476JE",
  "address": "0x08000000"
}
```

`revision` is an integer release counter, separate from your product's text
version. The factory default is 40002, so an update must use a larger value.
The SHA above matches the supplied production binary; regenerate it for each
new binary. Unknown or duplicate JSON fields are rejected. Strings use plain
ASCII without JSON escape sequences. The binary must be 8–524288 bytes, a
multiple of 8, with valid STM32 stack and reset vectors.

Generate the manifest without editing hashes manually:

```powershell
python standalone/tools/make_ota_manifest.py --binary "D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin" --url "https://YOUR-SERVER/firmware/production_firmware_v4.0.2.bin" --revision 40003 --output version.json
```

TLS authenticates the manifest and SHA-256 binds its downloaded image. This
is not an independently signed update package: compromise of the trusted
server/CA can authorize a malicious higher revision. Remote updates modify
the **STM32 payload only**, not the Pico executable. Executable updates still
use UF2. This implementation does not claim secure boot or physical anti-rollback.

## Programming logs and cloud acknowledgement

Before each erase, a durable `inflight.job` marker reserves the job. The UID
is read over SWD from `0x1FFF7590`, `0x1FFF7594`, `0x1FFF7598` after target
identification and before erasing. A failed connection records `UNAVAILABLE`.
No UID is fabricated.

CSV columns:

```text
job_id,pico_id,stm32_uid,time_kind,timestamp,elapsed_ms,status,image_bytes,result,stage,address,detail
```

- `stm32_uid`: three 32-bit UID words in ascending register-address order,
  formatted as one 24-hex-digit string.
- `time_kind`: `UTC_UNIX` with Unix seconds if NTP has succeeded, otherwise
  `UPTIME_MS` with time since Pico boot. No battery-backed clock is assumed.
- `elapsed_ms`: target programming report duration, including verification;
  it excludes the subsequent local log commit/cloud transfer.
- Status: `SUCCESS`, `ERASE_FAIL`, `WRITE_FAIL`, `VERIFY_MISMATCH`,
  `CONNECT_FAIL`, `IMAGE_FAIL`, `RESTART_FAIL`, or `INTERRUPTED`.
  Numeric result/stage preserve extra detail.
- `image_bytes`: actual selected image size, not just non-FF bytes written.

NTP uses the configured server (default `pool.ntp.org`), checks the replying
server and echoed random request timestamp. NTP itself is not authenticated;
hostile time manipulation is outside this implementation's security guarantee.
Without NTP, target programming/logging continue offline, but HTTPS requests
wait for time so certificate-date checks are not bypassed.

`programming_logs.csv` is atomically renamed to `upload.csv` for a batch.
The Pico POSTs it as `text/csv` with an `Idempotency-Key` equal to its SHA-256.
The server must:

1. Authenticate the request, if your endpoint requires authentication.
2. Validate and durably store the batch, deduplicating by `Idempotency-Key`.
3. Reply **HTTP 200**, **Content-Length: 64**, body equal to that exact
   64-character lowercase key, with **no newline**.

Only that authenticated acknowledgement permits deletion of `upload.csv`.
Failed requests, lost replies and unexpected acknowledgements retain the batch
for retry (once per minute while connected). A repeated POST is expected after
an uncertain network outcome; server-side idempotency is required. The code
does not delete logs merely because Wi-Fi connects or any HTTP response arrives.
Up to 256 KiB of CSV data is retained; when full, START is blocked until upload
frees space. No unacknowledged row is intentionally overwritten.

Power loss during a job produces an `INTERRUPTED` record on the next boot.
If the completed CSV row survived but marker removal did not, job-ID checking
avoids recording a second interrupted row. A storage error prevents a misleading
green success indication: RGB becomes red and the onboard LED gives **10 pulses**.

## Scheduling and LEDs

Core 1 exclusively drives SWD. Core 0 owns the button, LEDs, Wi-Fi and storage.
The network build uses SDK 2.3's memory-location overrides to reserve a
16 KiB core-0 stack below `0x20080000`; the heap ends below that stack and
core 1 keeps its separate 4 KiB stack. Linker assertions check separation.
Semaphores replace application FIFO messages so SDK flash lockout can safely
park the other core. All Pico flash mutations use `flash_safe_execute`, with
the core-1 victim initialized before mounting storage. No filesystem/OTA flash
mutation occurs during a target job. Network receive callbacks only buffer
data; storage writes run from the idle main loop.

START takes priority: it cancels any in-progress download/upload, retains
pending logs, snapshots the active image, then starts the job. The STM32 image
cannot change during programming or byte verification.

- Startup checks: external RGB off, then blinking green when ready.
- Programming: solid blue.
- Verified programming and successful local log commit: solid green.
- Programming failure: solid red, existing onboard diagnostic pulse codes.
- Storage failure/full log/provisioning reboot pending at START: red, 10 pulses.
- Wi-Fi/AP activity does not change these target-programming LED meanings.

USB diagnostics add `NETWORK status=... pico=... image_revision=... image_bytes=...`.
They remain optional and never wait for a host. Your read-report PowerShell
command can read these lines; no terminal is required for normal operation.

## Source map

| File | Responsibility |
| --- | --- |
| `include/wifi_config.h` | Factory defaults, endpoint and certificate configuration |
| `src/services/network.c` | Wi-Fi state machine, NTP, OTA/log orchestration |
| `src/services/portal.c` | Phone form, credential save and delayed reboot |
| `src/services/http_client.c` | Bounded asynchronous, certificate-verified HTTPS client |
| `src/services/protocol.c` | Strict manifest, URL and form parsers |
| `src/services/storage.c` | Safe flash access, slots, AES-GCM credentials, LittleFS logs |
| `src/main.c` | Core handoff, immutable job image, LED/log integration |
| `src/stm32_programmer.c` | Existing programmer plus target UID extraction |
| `include/lwipopts.h`, `include/mbedtls_config.h` | Networking/TLS build configuration |
| `CMakeLists.txt`, `storage_guard.ld` | SDK/LittleFS links and partition overlap guard |

LittleFS is vendored at v2.11.2 with its license. DHCP/DNS originate from
Raspberry Pi pico-examples (commit recorded in `third_party/pico_ap/UPSTREAM.txt`);
local changes bound received options/DNS labels and restrict requests to the AP.

## Validation and bench qualification

Host tests exercise the actual storage module and LittleFS against a NOR model:
complete/fragmented OTA, invalid digests, revision rejection, power cuts at
erase/program/commit boundaries, encrypted credential tampering, log recovery,
acknowledgement retention and storage saturation. Host cryptography uses an
OpenSSL adapter; the Pico build uses SDK mbedTLS. Existing programmer tests
cover fast/standard paths, full image verification, failures and UID reads.

```powershell
python standalone/tests/run_network_tests.py
python standalone/tests/run_tests.py --cc C:\msys64\mingw64\bin\gcc.exe --firmware "D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin"
```

These do not validate radio reception, a real HTTPS server or power interruption
on actual flash hardware. Before production, bench-test:

1. Phone setup after 15 seconds; correct/wrong credentials and reboot recovery.
2. A normal program/verify and UID/log contents on your physical STM32 board.
3. Independent CubeProgrammer Hot plug comparison while the target remains powered/halted.
4. Valid HTTPS, wrong CA/hostname/expired certificate, malformed manifest and bad hash.
5. Unplug power during a download; verify the previous image still programs.
6. Successful cloud acknowledgement and lost replies; confirm server deduplication.
7. Offline log accumulation and full-log refusal without losing earlier records.
8. Concurrent independent operation of all four sockets.

Reference: [Pico SDK flash safety](https://www.raspberrypi.com/documentation/pico-sdk/high_level.html#group_pico_flash),
[LittleFS](https://github.com/littlefs-project/littlefs),
[Pico Wi-Fi examples](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi).
