# Fast-v2: continuous STM32L476 fast programming

Built 28 September 2026 for Pico 2 W / RP2350 and the supplied 512 KiB image.
Software validation passed. This revision has not yet been installed or tested
on the user's target. The four-socket/translator fixture is also unqualified.

## Why change the sequence?

The USB report from the working recovery build recorded a fast-write failure
at **0x08000100**, the second 256-byte row, with FLASH_SR **0x000002A0**:
PGAERR, PGSERR and FASTERR. Standard recovery then passed full verification in
19.939 seconds overall. See [the captured hardware result](hardware-result-2026-09-28.md).

The old loader cleared FSTPG after each row. ST's multi-row programming flow
retains FSTPG until the last fast request. Changing this is a targeted fix for
the observed second-row failure; hardware confirmation of the cause is pending.
References: [RM0351 flash programming](https://www.st.com/resource/en/reference_manual/DM00083560.pdf)
and [ST HAL FLASH_TYPEPROGRAM_FAST / FAST_AND_LAST](https://github.com/STMicroelectronics/stm32l4xx-hal-driver/blob/master/Src/stm32l4xx_hal_flash.c).

## Implemented flow

1. Connect/reset-halt; normalize STM32 clocks to MSI 24 MHz; upload the SRAM
   loader; mass-erase both banks and check all 512 KiB are blank.
2. **Fast pass:** upload 16 KiB blocks, validate their CRC in target SRAM, and
   program complete aligned 256-byte rows in ascending address order. FSTPG
   stays enabled across rows, skipped erased/sparse regions and block uploads.
   Each row uses 64 consecutive 32-bit stores, followed by busy/error and fresh
   EOP checks. EOPIE is enabled while CPU interrupts remain masked.
3. End the fast pass explicitly by clearing FLASH_CR while the loader is halted
   and the flash controller is no longer busy.
4. **Sparse pass:** write only populated double words in the remaining rows or
   partial tail. Skip every row already written by the fast pass. All-FF double
   words remain physically erased, including their ECC bits.
5. Lock flash and compare every image byte while the product CPU remains halted.
   Only a successful comparison permits target restart and solid green.

For this binary, the modeled sequence uses **33 RAM uploads, 1,650 fast rows
and 104 standard double words**. The extra uploads handle sparse rows separately.
No bytes are changed, omitted from verification or padded. The all-FF settings
page at 0x0807E800 remains erased until the product chooses to change it on boot.

One clean reset/mass-erase/blank-check/standard-only retry is retained for flash
controller or missing-fast-completion errors. It never writes over a partial
fast row. Other failures stop. Green alone therefore does not prove fast mode
worked: the report identifies whether recovery was used.

PIO settings, GPIO assignments, LEDs and button behavior are unchanged. SWD is
still requested at 2 MHz, with the existing translator settling limits. Flash
fast mode is separate from increasing SWCLK; no electrical timing assumptions
were relaxed. Transfer, CRC, erase and full verification still take time.

## Install and confirm

Use `build-standalone/standalone_programmer-fast-v2.uf2`.
SHA-256: `a9c95f654bb15ab7ddc7d07dbdd3624c1a93100f10a39eda70f2cdddbe70ad3d`.
The ordinary `standalone_programmer.uf2` contains the same build at this point.

1. Hold Pico BOOTSEL while connecting USB; release it after the RP2350 drive appears.
2. Copy the fast-v2 UF2 onto that drive. No additional build command is needed.
3. Keep the STM32 powered with the existing SWD/reset/common-ground connections.
4. Wait for blinking green. Connect GP6 to GND for at least 100 ms and release.
5. After blue changes to green/red, keep Pico USB connected and read:

```powershell
powershell -ExecutionPolicy Bypass -File .\read-standalone-report.ps1
```

This optional diagnostic command reads an existing result; it does not initiate
programming. The programmer still works without a PC.

Expected successful fast-mode fields are `PICO-PROGRAMMER fast-v2`, `result=0`,
`retry=0`, and `fast_rows=1650`. `elapsed_ms` records measured total time.
`retry=1` means the standard recovery path was used; send both the main report
and `FAST-FIRST` line. If red appears, also record the onboard LED pulse count.

The proven recovery build is preserved as
`build-standalone/standalone_programmer-recovery.uf2`, SHA-256
`371527e810aad13c416a466cb2899fd5550f965f1e58b6996638a81535d5ef5c`.

## Validation

- Pico SDK release build passed; generated target loader is 736 bytes within
  its 2 KiB reservation. SRAM code/mailbox/buffer/stack bounds remain checked.
- Host tests exercised both modes, persistent FSTPG across RAM blocks, sparse
  pass ordering, full readback, fault injection and the single clean recovery.
- Actual compiled Cortex-M4 instructions passed the flash/MMIO model, including
  multiple rows, multiple calls, bank/page/end boundaries, CRC/errors/timeouts,
  EOP gating and preserved erased ECC. The complete supplied binary matched.
- The archived per-row loader and new loader were compared in the model: the
  old sequence enabled/disabled FSTPG twice for two rows; the new sequence
  enables it once and leaves it active for the host to finish the fast pass.
- The model now uses native Unicorn MMIO callbacks. Writing register backing
  memory from a read hook invalidated translation/IT state and produced a false
  repeated-row failure. Both old and new loader regressions run with this fix.
- PIO instruction tests passed. UF2 payload matches the Pico BIN, and the
  embedded production bytes match the supplied file exactly. Detailed hashes
  are in `build-standalone/uf2-validation.json`.

These checks validate software behavior, not physical flash timing. No shorter
cycle time is claimed until the new USB report is captured on the target.
