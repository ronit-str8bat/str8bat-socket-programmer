# Four independent standalone STM32 programmers

For a comparison before application startup, use the
[halt-after-verification diagnostic UF2 and handoff instructions](docs/halt-after-verify.md).

Latest update: [fast-v3 throughput changes and installation](docs/fast-v3.md).
Fast-v2 completed on the user's board in 18.740 seconds with no recovery.
Fast-v3 has passed software tests and awaits a hardware timing trial.

One **Pico 2 W / RP2350 per socket**. Load the same standalone UF2 onto all
four Picos. Each has its own START button, RGB LED, SWD lines and target.
There is no shared programming bus, mux, host service, OpenOCD, runtime file
transfer, or network dependency. A PC is used only to build/update the Pico
UF2 during development. Field operation needs power and the START button.

**Status: implemented and successfully compiled with Pico SDK 2.3.1. Host
protocol/state-machine tests pass. The four-socket 1.8 V fixture has not been
tested, and the translator part number is unknown. This is a qualification
candidate, not a factory-qualified release. No new firmware has been loaded
onto the connected Pico by this work.**

The earlier `debugprobe_on_pico2_w.uf2` exposes a USB CMSIS-DAP device. This
application is different: `standalone_programmer.uf2` is the SWD **host** and
does not enumerate as a USB debug probe. Reinstall the old UF2 to return to
the laptop/OpenOCD workflow.

## GPIO and physical header pin names

Physical pin numbers are for the **Pico 2 W 40-pin header**, not the ZIF socket.
The following assignments are provided as requested and are provisional until
the fixture is wired to match. Edit `include/programmer_config.h` to change them.

| Function / macro | Pico GPIO name | Physical pin | Connection |
|---|---|---:|---|
| `SWCLK_PIN` | GP2 | 4 | Translator -> target PA14 / SWCLK |
| `SWDIO_PIN` | GP3 | 5 | Bidirectional translator <-> target PA13 / SWDIO |
| `NRST_PIN` | GP1 | 2 | Reset circuit -> target NRST, low or released |
| `START_PIN` | GP6 | 9 | Normally-open push button to GND; internal pull-up |
| `RED_LED_PIN` | GP7 | 10 | Red anode through its own resistor |
| `GREEN_LED_PIN` | GP8 | 11 | Green anode through its own resistor |
| `BLUE_LED_PIN` | GP9 | 12 | Blue anode through its own resistor |
| Ground | GND | 3 (or another GND pin) | Common logic ground |
| RGB common cathode | GND | 13 (or another GND pin) | LED common cathode, not a GPIO |
| Onboard LED | CYW43 GPIO 0 | No header pin | Controlled through SDK CYW43 API |
| Optional `SWDIO_DIR_PIN` | Suggested GP10 | 14 | Only if translator exposes SWDIO DIR |
| Optional `SWDIO_OE_PIN` | Suggested GP11 | 15 | Only if translator exposes separate SWDIO OE |

Optional DIR/OE defaults are **-1 (not wired)**. They are not silently enabled.
If used, configure polarity with `SWDIO_DIR_TX_LEVEL` and
`SWDIO_OE_ACTIVE_LEVEL`. Do not share this OE/DIR with SWCLK or NRST.
The RGB LED is common-cathode: GPIO HIGH turns a colour on, LOW turns it off.
Provision each RGB resistor from LED forward voltage and a modest GPIO source
current (e.g. a design target of 2 mA, using R=(3.3V-Vf)/I). Do not connect
LED anodes without resistors. Connect the common cathode to GND, not 3.3 V.
For guaranteed dark LEDs before software starts, provide suitable external
pull-downs on the anode control nets; firmware cannot control reset-time pins.

## Electrical points to resolve before first target insertion

* The supplied drawing says RP2040; the specified Pico **2 W uses RP2350**.
  This build explicitly selects `pico2_w`, and GP25 is not used as an LED.
* These four controllers are functionally independent, **not galvanically
  isolated**: the supplied drawing has common power/ground.
* A BGA-4518/4515 / “48-pin ZIF” name alone does not prove that STM32L476JEY
  WLCSP72 balls reach the correct contacts. Obtain the adapter schematic and
  continuity-check PA13, PA14, NRST, all required supply/ground connections,
  boot straps and package-specific power pins. No ZIF contact numbers are
  invented in this project. A generic adapter is not a complete MCU carrier.
* Target 1.8 V must be regulated and meet the device's power/decoupling and
  sequencing requirements. Supply all required domains for the exact suffix.
  The 512 KiB JE part is different from the earlier reported JG part.
* Never connect a 3.3 V SWD signal directly to the 1.8 V target fixture.
  Confirm translator type, propagation delay, allowed loads and power-off
  behavior. An auto-direction translator intended for another bus is not
  automatically suitable for push-pull SWD. Bit-banging cannot fix contention
  inside an unsuitable translator. The default unconnected DIR/OE configuration
  assumes a translator proven to handle this handoff electrically.
* NRST is **open-drain emulated on the Pico**: output-low to assert, input to
  release. The target side needs a pull-up to its own 1.8 V rail. A fixed
  push-pull buffer with a floating input is not an acceptable reset translator;
  use a qualified low-or-release reset path. Check NRST on both sides with a
  scope. Firmware does not drive NRST high or change RDP/option bytes.
* The drawing's external 3.3 V regulator is not an instruction to parallel it
  with a Pico's onboard regulator. Choose the Pico power-input arrangement
  from the Pico 2 W datasheet (VSYS/USB versus externally supplied 3V3), with
  proper regulator disable/backfeed control as appropriate. USB power from a
  standalone supply is sufficient; no PC data connection is needed.
* Hot insertion/removal is not managed: this firmware has no target-power
  switch or socket-presence input. Provide fixture power controls/procedure.
  Never remove a part while BLUE is on (programming in progress).

## State sequence

1. Configure safe pin states and the CYW43 onboard LED interface. Keep RGB and
   onboard LEDs off while checking the embedded image's CRC-32/ISO-HDLC.
2. A CRC or CYW43 initialization failure blinks RED until power-cycle and blocks
   START. Good self-test blinks GREEN until the first START. Both use 250 ms on /
   250 ms off. The onboard LED then provides an idle heartbeat after success.
3. A stable release of START for 100 ms arms it. A subsequent continuously
   low input for 100 ms starts one job. Chatter restarts the timer. Holding
   START at boot does not start a job. Holding START after a job never repeats.
4. Core 0 holds BLUE solid throughout the job. Core 1 rechecks image
   CRC, asserts NRST, establishes SWD and catches reset with the CPU halted.
5. Validate device ID/512 KiB capacity, RDP0, no write/PCROP protection and no
   bank swap. Enable/read back the PWR interface and exit low-power run if
   necessary. With the CPU halted, raise flash latency, switch temporarily to
   HSI16, stop PLLs, configure MSI at 24 MHz, select MSI/AHB divide-by-one, then
   use three flash wait states. Every ready/source transition is checked.
   Existing PLL/HSE/HSI clock configurations are not rejected merely for
   differing from reset defaults. Invalid regulator voltage-range bits still
   fail before erase; regulator/oscillator/switch readiness waits are bounded.
6. Upload and read back a small SRAM programming routine. **Mass erase main
   flash**, blank-check all 512 KiB, upload 16 KiB data blocks to SRAM, program
   aligned 256-byte fast rows in one continuous FSTPG pass, then use a separate
   standard 64-bit pass for sparse/tail data. Finally halt and read
   **every byte of the embedded image** back and compare it to the source.
   All-FF double words are left physically erased to preserve their ECC state;
   they are still read and verified. The image is neither truncated nor padded.
7. On success, reset/release the target and show solid GREEN with RED/BLUE off.
   On failure, show solid RED with GREEN/BLUE off and keep target NRST asserted
   so a partially written application does not execute. A new release/press
   cycle permits another attempt. Busy presses are discarded, not queued.

Onboard LED uses `cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, ...)`: slow idle
heartbeat, faster busy heartbeat. It remains off during initial CRC. It is
independent of the result RGB. The CYW43 interface is initialized solely to
drive the onboard LED; no access point, Wi-Fi network or Bluetooth link is used.

After a programming failure, the external RGB remains solid RED and the
**Pico onboard LED** repeats an error count: 250 ms on / 250 ms off per pulse,
then an additional 2-second off gap. Count pulses between the long gaps:

| Pulses | Reported failure category |
|---:|---|
| 1 | Embedded image validation / checksum |
| 2 | SWD transaction or debug-link initialization |
| 3 | Unexpected target identity, capacity or bank mapping |
| 4 | Target protection / unsupported boot-bank configuration |
| 5 | CPU did not halt, halted unexpectedly, or escaped halt during verification |
| 6 | Timed out waiting for the target or session deadline |
| 7 | Flash erase/write, blank-check or RAM-loader completion error |
| 8 | Readback did not match the image |
| 9 | Target clock configuration check failed |

These identify the software-reported category, not necessarily the physical
cause. A SWD failure can occur at any programming stage; debugger globals
`last_report.stage`, `.address` and `.detail` contain the finer diagnosis.
The count clears on the next START. No laptop is required to read this code.

All SWD WAIT, target-halt, flash-busy and session waits have time bounds. A
session is limited to 300 seconds. Protection changes are not attempted; a
protected part fails RED before mass erase. This programmer only writes main
flash, not OTP, option bytes, external memory or a separate BLE chip.

## Native SWD implementation and performance

`src/swd.c` implements LSB-first SWJ selection, parity, ACK/WAIT/FAULT handling,
one-cycle direction turnaround, debug power-up, AP0 AHB-AP posted reads and
explicit 1 KiB TAR rollover. Write completion/sticky errors are checked.
The Pico releases SWDIO before target ACK/data and does not re-enable its
output until after the target's turnaround cycle. Optional OE is disabled
while DIR/Pico output direction changes, with configurable settling time.
PIO is now the default: `src/swd_wire.pio` clocks each bit in four state-machine
cycles without software delay loops. Input is captured at the rising edge,
before the target's next-bit propagation; the bit-bang fallback samples just
before that edge. Completion is returned only with SWCLK low. FIFO waits are
bounded. Redundant direction changes are removed; actual turnarounds retain
the configurable 1 us settling waits and optional DIR/OE control.

Clock requests in `include/programmer_config.h` are 100 kHz for connection and
2000 kHz for programming. A 250 ns minimum half-period and integer PIO divider
round the clock DOWN: at a 150 MHz Pico system clock, these yield 100 kHz and
about 1.974 MHz respectively. These are computed settings, not scope readings.
The translator part remains unknown: do not increase `SWD_PROGRAM_KHZ` or
reduce `SWD_MIN_HALF_PERIOD_NS` / `TRANSLATOR_SETTLE_US` without measuring the
actual signal path. Its maximum reliable speed cannot be inferred from software.

`swd_write_words` performs consecutive AHB-AP DRW/WDATA writes into SRAM,
reloading TAR every 1 KiB and flushing/checking completion at each boundary.
Every word checks ACK. WAIT retries the unaccepted transfer within 50 ms;
FAULT/parity/bad ACK stops without blindly repeating an accepted flash write.
There is no fictitious SWD bulk opcode: these are auto-incrementing AP transfers.

The Cortex-M4 routine is separately compiled from `loader/stm32l476_loader.c`.
Code lives at 0x20000000 (max 2 KiB), mailbox at 0x20000800, 16 KiB buffer at
0x20001000..0x20004FFF and stack at 0x20005000..0x20005FFF (top 0x20006000).
Compile-time assertions prevent overlap. Loader code is read back before execution;
each data block has a target-computed CRC before any flash write. The Pico staging
buffer is static BSS, not on the 4 KiB core-1 stack.

Fast programming is enabled by default. After mass erase, the loader uses FSTPG
only for complete, 256-byte-aligned rows containing no all-FF double words. It
delivers 64 ordered 32-bit stores from SRAM in a tight loop, with interrupts
masked and no flash reads or watchdog/status calls between data stores. It checks
24 MHz MSI/AHB configuration before fast mode. The host polls debug registers,
never flash, during loader execution. After each row it waits for BSY to clear,
checks errors and requires a fresh EOP flag. FSTPG stays enabled across all fast
rows and block uploads; the host clears it once at the end of the fast pass.
Sparse rows are then programmed in a separate standard pass. EOPIE is enabled
to generate EOP; CPU interrupts remain masked throughout the loader. A fast flash
error saves the original failure and permits one reset/mass-erase/blank-check
followed by a standard-only retry. No writes are replayed over a partial row.
SWD/CRC, protection, timeout and verification failures never trigger this retry.

Sparse rows and incomplete tails use standard 64-bit programming, skipping
all-FF double words to preserve their erased ECC. Thus 0x0807E800 and other
erased configuration fields are not accidentally consumed by a full-row write.
Every byte is still verified. Standard writes use the previously working BSY/error
polling sequence, with no mandatory EOP flag. Both modes return to BKPT.
IWDG is fed during CRC, status waits and between completed rows/double words.
The product application never executes during programming/verify.

Use `-DSTANDALONE_FAST_PROGRAM=OFF` to build the standard-only 16 KiB variant.
The known working 2 KiB/standard UF2 from before this change is preserved at
`build-standalone/previous-2kb-standard/standalone_programmer.uf2` with its manifest.
See [current fast-programming implementation and checks](docs/fast-v2.md).

Use `-DSTANDALONE_SWD_PIO=OFF` at CMake configuration for the diagnostic GPIO
fallback; its software overhead and 1 us minimum half-period cap the rate.
The RAM loader remains enabled. Previous OpenOCD timings do not predict the new
cycle time. Measure on the fixture; sub-10-second cycles are not claimed.
`last_report.stage_ms[0..4]` records connect/setup, erase/blank-check, program,
verify and restart durations; `elapsed_ms` records total programmer time.
`initial_rcc_cr`, `initial_rcc_cfgr` and `initial_pwr_cr1` retain the starting
clock/power configuration in the same report. Code 9 indicates invalid clock/
regulator configuration; code 6 can indicate a readiness timeout, distinguished
by `last_report.address`. Both abort before erase when raised in clock setup.

The SWD engine runs on core 1 while the responsive UI and radio LED run on core
0. The product image is `const` XIP flash data, not copied to Pico SRAM. No
runtime file system, heap buffer for the image, terminal, or host command is
needed. Qualification of the new PIO/loader path on the fixture is still required.

## Investigation of 0x0807E800

The byte offset is 0x7E800 = 518144, aligned to 8 bytes, 1 KiB TAR boundaries
and 2 KiB STM32L476 pages. It is global page 253; with the detected 512 KiB
dual-bank arrangement it is bank 2 page 125 (zero-based). Page size is 2 KiB
also in this part's single-bank mode. Mass erase includes MER1 and MER2.

The supplied image contains only FF in 0x0807E800..0x0807EFFF. Available product
source names this address `FIRMWARE_MODE_ADDRESS`, and `fw_mode_reset()` writes
a factory/user-mode value there after boot. That source's version is 3.6.15,
not a verified source match for the 4.0.2 binary. Subsequent inspection of the
actual 4.0.2 binary found an initializer that programs exactly eight zero bytes
at this address when it is erased. Emulating that initializer reproduces the
write request seen in the user's later memory-comparison screenshot. See
[binary analysis](docs/production-mode-analysis.md). This explains a possible
post-boot difference; it does not establish that a pre-boot verify failure occurred.

No page is excluded from verification and the binary is not altered. Every
main-flash word is now checked erased before any programming, including FF-only
pages. Bounds use 32-bit addresses/size_t with explicit limits; target writes
are aligned 64-bit pairs or 256-byte rows and transfer boundaries are 16 KiB. Non-multiple-of-8
images are rejected, not silently padded. Both 1 KiB TAR and 512 KiB-end cases
are regression-tested. CPU halt is checked during all verification chunks;
NRST is asserted BEFORE debug is disabled at the final restart, removing the
old opportunity to execute application code briefly before the reset pulse.

If verification fails, `last_report.address` identifies the first differing
byte and `.detail` encodes expected byte in bits 15:8, actual in bits 7:0.
An incomplete erase fails in STAGE_ERASE before any data write. A mismatch
after the application has restarted must be distinguished from this halted
verification; the application may intentionally modify its configuration page.

## Source files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Standalone SDK build, CYW43 LED support and embedding |
| `include/programmer_config.h` | Pins, translator controls, timing and target policy |
| `src/main.c` | Two-core UI/worker, RGB and onboard LED, CRC gating |
| `src/ui_logic.c`, `include/ui_logic.h` | Non-blocking 100 ms debounce/rearm |
| `src/crc32.c`, `include/crc32.h` | Independent runtime CRC implementation |
| `src/swd.c`, `include/swd.h` | Native SWD transport and AHB memory access |
| `src/swd_pio.c`, `src/swd_wire.pio` | PIO clock, FIFO, direction and bit engine |
| `loader/stm32l476_loader.c`, `loader/loader.ld` | Freestanding STM32 RAM algorithm |
| `include/loader_protocol.h`, `tools/embed_loader.py` | Loader mailbox ABI and build embedding |
| `src/stm32_programmer.c`, `include/stm32_programmer.h` | Connect/erase/write/verify/restart |
| `tools/embed_firmware.py` | Binary validation and const-array generation |
| `tests/` | Host tests with fake GPIO and target registers |
| `../build-standalone.ps1` | Build helper for this Windows laptop |

Generated `firmware_image.h` exposes:

```c
extern const uint8_t firmware_image[];
extern const size_t firmware_image_size;
extern const uint32_t firmware_Image CRC32;
```

CRC protects against accidental corruption; it is not a signature. The build
manifest records SHA-256, and CRC is generated by Python/zlib independently
of the C runtime implementation. No fake placeholder image is programmed: a
missing, oversized, misaligned or invalid-vector binary stops the build.

## Build and one-time installation

From the parent `debugprobe` folder on this laptop:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-standalone.ps1
```

Or supply another approved binary linked at `0x08000000`:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-standalone.ps1 -FirmwarePath "D:\path\production_firmware_v4.0.2.bin"
```

Portable CMake equivalent, with Pico SDK, Python 3, Ninja, Arm GCC and host GCC
available in PATH:

```text
cmake -S standalone -B build-standalone -G Ninja -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=/path/to/pico-sdk -DTARGET_FIRMWARE=/path/to/production_firmware_v4.0.2.bin -DCMAKE_BUILD_TYPE=Release
cmake --build build-standalone
```

The generated files are:

* `build-standalone/standalone_programmer.uf2` — install on **each Pico**, not STM32.
* `build-standalone/standalone_programmer.elf` — symbols for development debugging.
* `build-standalone/generated/firmware_image.c` and `.h` — embedded target array.
* `build-standalone/generated/firmware_manifest.json` — image identity.

Build input used here: `D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin`,
524288 bytes, CRC32 `0e60efc5`, SHA-256
`3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9`.

To install, connect each Pico in BOOTSEL mode and copy the standalone UF2 onto
its RP2350 drive. Install only after confirming the pin/translator configuration.
Then power it from the standalone supply, wait for blinking GREEN, and press START for
at least 100 ms. All four sockets can be started independently. A firmware
revision requires rebuilding and updating all four Picos' UF2s.

## Validation performed and required

Host tests:

```powershell
python .\standalone\tests\run_tests.py --cc C:\msys64\mingw64\bin\gcc.exe
python .\standalone\tests\test_pio.py
python .\standalone\tests\test_loader.py
python .\standalone\tests\test_loader.py --firmware D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin
python .\standalone\tools\validate_artifact.py --firmware D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin
```

Passed: standard CRC vector, short/chattering/held button and timer wrap,
valid/invalid embedded-image validation, SWD request bit order and posted reads,
target-driven clock-edge timing and debug-port connection,
TAR rollover, parity/FAULT/WAIT/invalid-ACK errors, mass-erase/program/readback
flow, deliberate mismatch, RDP protection, wrong capacity, failed link and
flash-busy timeout. Full-size tests cover 0x0807E800, page/bank/end boundaries,
an incomplete erase, corrupted SRAM upload, lost halt and block-transfer errors.
Clock tests cover initial PLL/HSE/HSI, AHB division, voltage range 2, low-power
run, invalid voltage-range bits and regulator/oscillator/source-ready failures.
The PIO test models assembled instructions and clock edges. The loader test
executes the actual compiled Cortex-M4 machine code in Unicorn 2.1.4 with
modeled flash/MMIO. Install that optional test dependency with
`python -m pip install --target build-standalone/test-deps unicorn==2.1.4`.
These are simulated software tests, not physical proof.

The 16 KiB/fast-row revision additionally tests all 16 TAR chunks in a full
transfer, 32 uploads for a dense 512 KiB image, short final blocks, dense/sparse
row transitions, preserved erased ECC, fast errors, missing/stale EOP, stuck
BSY and invalid fast-mode clock setup. The optional production-image test runs
the actual Cortex-M4 loader against each populated block of the supplied binary.

Before production, qualify at least: all four sockets simultaneously at 1.8 V;
SWDIO turnaround/NRST waveforms through the actual translator; blank and
previously programmed parts; missing/bad-contact target; protected device;
power loss during erase/write; repeated START chatter/holds; corrupt image;
independent external readback of all 512 KiB; LEDs and supply behavior during
boot and failures; repeated cycles and worst-case temperature/supply margins.
The onboard debug globals `last_report`, `active_stage`, `bytes_completed`
provide stage, error, failing address and duration through a development probe.
They are not required for normal operation and are not persistent batch records.

The `fast-v3` build also exposes an optional USB CDC diagnostic report while
idle. It never waits for a PC and accepts no programming commands. After a job,
connect/read without power-cycling to obtain `result`, `stage`, `address`, `detail`,
`loader`, `completed`, `retry`, `fast_rows`, elapsed/stage times and the preserved `FAST-FIRST`
failure. `retry=1` with result 0 means standard recovery passed full verification;
it does not mean fast programming worked. Use root `read-standalone-report.ps1`
to read it. Configure `-DSTANDALONE_USB_DIAGNOSTICS=OFF` to omit USB diagnostics.
For this production image, `result=0 retry=0 fast_rows=1650` confirms a verified
fast-mode run. See [fast-v2 notes](docs/fast-v2.md) for the remaining hardware check.

## Primary references

* [STM32L476JE device information](https://www.st.com/en/microcontrollers-microprocessors/stm32l476je.html)
* [STM32L476 datasheet / WLCSP pinout](https://www.st.com/resource/en/datasheet/stm32l476je.pdf)
* [STM32L4 reference manual RM0351](https://www.st.com/resource/en/reference_manual/dm00083560.pdf)
* [Arm ADIv5 specification](https://documentation-service.arm.com/static/5f900abff86e16515cdc063b)
* [ST flash programming implementation](https://github.com/STMicroelectronics/stm32l4xx-hal-driver/blob/master/Src/stm32l4xx_hal_flash.c)
* [OpenOCD STM32L4 reset clock configuration](https://github.com/openocd-org/openocd/blob/master/tcl/target/stm32l4x.cfg)
* [Pico 2 W datasheet](https://datasheets.raspberrypi.com/picow/pico-2-w-datasheet.pdf)
* [Official onboard LED example](https://github.com/raspberrypi/pico-examples/blob/master/pico_w/wifi/blink/picow_blink.c)
