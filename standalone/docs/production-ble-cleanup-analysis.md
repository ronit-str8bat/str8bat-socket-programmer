# Post-startup mismatch at 0x08040000..0x0806C7B0

The user reported a fast-v3 green result followed by a CubeProgrammer comparison
showing differences starting at 0x08040000 through the displayed 0x0806C7B0 row.
The values read from the device have not yet been supplied. This is a post-run
comparison: the programmer releases target reset after its full verification.

The analyzed production file is exactly 524288 bytes, SHA-256
`3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9`.
Its BLE staging signature is eight AB bytes at 0x08040000, followed by a payload.
The last non-FF byte in the staging area is **0x0806C7BE**, within the user's
reported final display row. The remainder up to 0x0807E7FF is already FF.

## Evidence from the actual binary

| Address | Observed instructions |
|---|---|
| 0x0800CD5E | Calls the staging check at 0x08014D70 |
| 0x08014D74..0x08014D84 | Reads 0x08040000 and tests both words for ABABABAB |
| 0x08014D8C | Calls update wrapper at 0x08014DA6 |
| 0x08014DA8 | Calls the BLE update routine at 0x0802E03A |
| 0x08014DB2..0x08014DC0 | Nonzero update result sets a failure flag; skips erase |
| 0x08014DC2..0x08014DCC | Success path performs BLE run-mode call/delay, then calls cleanup |
| 0x08014DD2..0x08014E16 | Constructs page erase request and calls the flash HAL |
| 0x08014DF2 | Sets page count to 125 |
| 0x08014E02 | Calls HAL_FLASHEx_Erase at 0x0802B220 |

`tests/inspect_production_ble.py` executes these actual Thumb instructions,
including the bank/page helpers, with 512 KiB capacity and bank swap disabled.
External BLE/I/O functions and the HAL erase operation are stubbed. It observes:

- Signature present, BLE update reports success: page erase request is type 0,
  bank 2, first page 0, count 125. Simulated erase spans 0x08040000..0x0807E7FF.
- Comparing the resulting simulated memory with the original file produces
  differences from **0x08040000 through 0x0806C7BE**. Already-FF locations do not
  show differences, explaining the shorter visible end address.
- BLE update reports failure: no staging-area erase.
- Signature absent: no update or staging-area erase.

The older source tree's `Software_uart/ble_stack_update.c` and
`blestack_config.h` give names and intent for this region. That source is not
assumed to match version 4.0.2: the conditional success/failure behavior above
comes from the supplied binary, not the older source.

## Interpretation and remaining confirmation

This is strong evidence for intentional application cleanup after the Pico's
pre-boot comparison. It does not prove that the physical board took this path:
the actual CubeProgrammer dump has not been inspected. Confirm whether the
reported differences are now FF. Other byte patterns require further analysis.
An independent comparison before application execution can distinguish a
programming defect from post-startup modification; attaching after startup
cannot recover the original pre-boot state.

No programmer code, production binary or UF2 was changed by this investigation.
No flash erase/write was performed on connected hardware. Full byte comparison
must remain enabled; do not mask this region to suppress comparison failures.
