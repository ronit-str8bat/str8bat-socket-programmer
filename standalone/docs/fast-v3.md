# Fast-v3 throughput update

Baseline supplied by the user: fast-v2 completed on the target with `result=0`,
`retry=0`, `fast_rows=1650`, `elapsed_ms=18740`. Stage times were 183 ms connect,
187 ms erase/blank-check, 11852 ms program, 6497 ms verify, 20 ms restart.
The one-millisecond difference from the sum is independent stage rounding.

## Changes

- Sparse-pass uploads now trim leading/trailing rows that contain no sparse
  data needing programming. Start addresses remain row-aligned; final tails
  remain double-word aligned. Internal dense rows are skipped as before.
  This avoids retransmitting most of the data already programmed by fast mode.
- CRC uses a 16-entry, four-bit lookup table instead of eight bitwise steps
  per byte. Polynomial, initial/final XOR and expected CRC are identical.
  Both Pico and STM32 use it; the STM32 table resides with the loader in SRAM.
- Full flash verification reads 1024 bytes per chunk instead of 256, matching
  an SWD TAR window. A static 1 KiB buffer avoids growing the worker stack.
  CPU halt is checked before every chunk and once after the final comparison.

For the supplied image, the software tests measure:

| Work | fast-v2 | fast-v3 |
|---|---:|---:|
| Data uploads | 33 | 33 |
| Bytes uploaded for programming | 540672 | 460032 |
| Fast rows | 1650 | 1650 |
| Standard double words | 104 | 104 |
| Verification chunks | 2048 | 512 |
| Bytes read and compared | 524288 | 524288 |

The smaller transfers save 80640 uploaded bytes (about 14.9%). This percentage
is not a prediction of total time saved. Real cycle time must be measured on
the target. SWCLK, PIO waveform, translator settling, mass erase, fast sequence,
erased ECC preservation, recovery and LED behavior are unchanged.

## Install and measure

Use `build-standalone/standalone_programmer-fast-v3.uf2`.
SHA-256: `e4843ce452a3c1a684a65668471eca23a68a43f163cf57795ee0d9e6a54720f5`.
The build is ready; it has not been uploaded to the Pico.

1. Hold BOOTSEL while connecting Pico USB, then release it.
2. Copy the fast-v3 UF2 onto the RP2350 drive.
3. With the target connected and powered, wait for blinking green.
4. Connect GP6 to GND for at least 100 ms, then release. Wait for green/red.
5. Keep USB connected and run the following read-only diagnostic:

```powershell
powershell -ExecutionPolicy Bypass -File .\read-standalone-report.ps1
```

Expected successful fields: `PICO-PROGRAMMER fast-v3 result=0`, `retry=0`,
`fast_rows=1650`. Compare `elapsed_ms` and `STAGES_MS` with the baseline above.
Repeat under the same wiring/power conditions before accepting a production
timing. A result with `retry=1` is a successful recovery, not a fast-mode timing.
The optional diagnostic does not make standalone operation depend on a PC.

The working file `build-standalone/standalone_programmer-fast-v2.uf2` is retained,
SHA-256 `a9c95f654bb15ab7ddc7d07dbdd3624c1a93100f10a39eda70f2cdddbe70ad3d`.

## Validation completed

Pico SDK release build passed. The target loader is 808 bytes, below its 2048-byte
limit. Host tests exercised the actual production image in both programming
modes, trimmed tails, CRC corruption, reset/erase/recovery failures, and exact
mismatch addresses at verification boundaries and the last byte. Randomized
prefix CRC tests match an independent bitwise reference. Actual compiled ARM
loader instructions matched the production image in the flash/MMIO model with
independent Python/zlib CRCs; erased ECC was preserved. Previous loader
regressions also passed. UF2 payload and embedded production bytes were checked.

These tests establish software correctness for the modeled cases. No new
hardware speed or reliability result is claimed until the user tests fast-v3.
