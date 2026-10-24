# 16 KiB transfers and STM32L476 fast programming

**Historical implementation notes.** The current fast sequence and upload file
are documented in [fast-v2](fast-v2.md). It replaces the per-row FSTPG toggling
described below with a continuous fast pass followed by a separate sparse pass.
The 28-upload count below applies to the older interleaved algorithm; the new
algorithm uses 33 uploads for the supplied image.

Implemented 28 September 2026. This revision changes the Pico programmer, not
`production_firmware_v4.0.2.bin`. It has compiled and passed the software tests
below. Its timing and reliability on the physical fixture have not yet been measured.

Update: the first fast build and its EOPIE correction both produced seven blinks
on the user's board. The current build adds one clean standard recovery attempt
and a USB result report; use [recovery instructions](recovery.md). The retained
EOP-fix artifact below is historical and is not the current recommended upload.

## Data path

1. Pico verifies the embedded image CRC and connects/reset-halts the target.
2. Configure STM32 MSI at 24 MHz and retain the established 2 MHz SWD request.
3. Upload/read back the RAM algorithm, mass-erase both banks, blank-check 512 KiB.
4. Upload up to 16 KiB to target SRAM with auto-incrementing SWD writes. Reload
   TAR at every 1 KiB boundary, checking ACK and sticky errors as before.
5. Execute the SRAM algorithm. It validates the block CRC before writing.
6. For each complete aligned 256-byte row with no erased double word, use FSTPG.
   For sparse rows and partial tails, use standard 8-byte programming and skip
   all-FF double words. Never write padding past the image end.
7. Read back and compare every image byte with the application stopped.
8. On success, reset/release the application and display solid green. On error,
   display solid red and keep reset asserted. Button, wiring and LEDs are unchanged.

The larger transfer buffer reduces loader launch/halt overhead; it does not
change the number of SWD bits needed for a given uploaded block. Fast mode
reduces flash programming overhead; it does not eliminate transfer, CRC, erase
or full-readback time. No claim of a particular new cycle time is made.

## Memory allocation

| STM32 SRAM use | Start | End (inclusive) |
|---|---|---|
| Loader code/literals (reserved) | 0x20000000 | 0x200007FF |
| Mailbox | 0x20000800 | 0x2000081F |
| 16 KiB input buffer | 0x20001000 | 0x20004FFF |
| 4 KiB stack reservation | 0x20005000 | 0x20005FFF |

Stack top is 0x20006000. All regions are in the STM32's 96 KiB SRAM1, with
compile-time non-overlap assertions. The Pico's 16 KiB transfer staging array
is static BSS; its worker still has a 4 KiB stack. The 512 KiB production image
remains in Pico XIP flash, not Pico RAM.

## Fast-write requirements and failure handling

The implementation follows [ST RM0351, Flash programming](https://www.st.com/resource/en/reference_manual/DM00083560.pdf).
FSTPG requires a preceding bank mass erase, a complete aligned row, uninterrupted
ordered data delivery and HCLK of at least 8 MHz. The setup establishes 24 MHz;
the loader rechecks the clock and low-power regulator state. Source data, code,
stack and literals are in SRAM; interrupts are masked. The host reads only
debug registers while the loader runs, avoiding same-bank flash reads.

The compiled row loop contains only SRAM loads, ordered 32-bit flash stores,
index/compare/branch instructions. It sends 64 words per row without an
intervening watchdog call, SWD transaction or status poll. Physical flash wait
states/stalls and electrical timing still require a hardware test.

EOPIE is enabled for fast writes because RM0351 makes EOP generation conditional
on this bit. PRIMASK remains set: no flash ISR runs. Standard recovery uses busy/
error polling with optional EOP, followed by full host verification.
EOP is cleared before each operation. Afterwards, BSY must clear, no error may
be present, and a fresh EOP must be set. FSTPG is cleared after completion.
Fast flash/completion errors now save the original evidence and cause one full
reset/mass-erase/blank-check before standard recovery. Partially programmed rows
are never rewritten directly. Other errors stop the session. Final byte-for-byte
verification is retained in every mode.

An all-FF double word must remain physically erased to retain its ECC state
for application configuration writes. Fast mode therefore never includes one.
This preserves the all-FF page at 0x0807E800 in the supplied image. Application
changes after reset remain possible; see [configuration analysis](production-mode-analysis.md).

## Files to read

| File | Change |
|---|---|
| `include/programmer_config.h` | 16 KiB block and default fast-mode selection |
| `include/loader_protocol.h` | SRAM map, fast command and result codes |
| `src/stm32_programmer.c` | Static Pico buffer, larger uploads and fast command |
| `loader/stm32l476_loader.c` | Fast rows, sparse handling, clock/EOP checks |
| `CMakeLists.txt` | `STANDALONE_FAST_PROGRAM` defaults ON |

Configure with `-DSTANDALONE_FAST_PROGRAM=OFF` for standard-only writes using
the new 16 KiB transfer size. Both modes are exercised by the host tests.

## Validation of this build

* Pico 2 W SDK Release build passed with warnings treated as errors.
* Host tests passed: CRC/debounce, full 512 KiB programming/verification, 16 KiB
  block boundaries, short final block, clock/protection/reset handling, failed
  erase, corrupt upload and verification mismatch.
* Both SWD backends passed a 4096-word burst through 16 TAR chunks, plus the
  existing ACK WAIT/FAULT/parity tests. Assembled PIO instruction tests passed.
* Actual compiled Cortex-M4 loader ran in Unicorn with modeled
  flash/MMIO: dense/sparse/partial rows, page/bank/flash-end boundaries, erased
  ECC preservation, bad CRC, lock/protection and fast-write error flags,
  missing/stale EOP, invalid clock and stuck-busy timeout tests passed.
* The compiled loader also processed every populated block of the actual
  production binary: **28 uploads, 1650 fast rows and 104 standard double words**.
  Programmed bytes matched; every all-FF double word remained unwritten.
* UF2 payload matches the built Pico BIN. Embedded production bytes match the
  supplied 524288-byte file exactly. SHA-256:
  `3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9`.

The flash model validates software sequences, not the STM32's physical flash
controller or translator timing. It is not a measurement of programming speed.

### Correction after the seven-blink failure

The first fast-mode build incorrectly required EOP without setting EOPIE. The
original emulator model also incorrectly generated EOP unconditionally, so its
tests missed this hardware requirement. With the corrected EOPIE gate, that
old compiled loader fails after its first successful write (LOADER_NO_EOP,
reported as seven onboard blinks). This is consistent with the user's red LED
after 1-5 seconds, although the live target status register was not captured.

Both PG and FSTPG now enable EOPIE while CPU interrupts stay masked. The model
checks this behavior, and still injects absent EOP to ensure genuine incomplete
operations stop. An archived copy of the old compiled loader reproduces the
failure using `--regression-loader build-standalone/regressions/loader-missing-eopie.bin`.
The block size, fast-row selection, erase, CRC and byte verification are unchanged.

## Install and compare

The current file is `build-standalone/standalone_programmer-recovery.uf2`.
Build SHA-256: `371527e810aad13c416a466cb2899fd5550f965f1e58b6996638a81535d5ef5c`.
The superseded EOP-only build remains at
`build-standalone/standalone_programmer-eop-fix.uf2` for diagnostic history.
The previous working UF2 is preserved as
`build-standalone/previous-2kb-standard/standalone_programmer.uf2`.
Its SHA-256: `f501510634b74aa23884bb7ff417c188d2abc4a80cf6feb17ce2c199890d46c6`.

1. Hold BOOTSEL while connecting the Pico USB, then release it.
2. Copy the new UF2 to its RP2350 drive. This has not been done by the build tools.
3. With the existing target wiring/power, wait for blinking green.
4. Connect GP6 to GND for at least 100 ms and release. Time blue-to-solid-green.
5. Repeat a few cycles and check product operation. Compare with the previous
   approximately 19-second measurement under the same conditions.
6. If red appears, record the onboard pulse count before restarting. The old
   UF2 can be restored with the same BOOTSEL procedure for comparison.

Do not compare post-boot configuration changes with a halted pre-boot verify as
if they were the same measurement. The application can modify its settings.
