# Recovery-v1: observed result on the user's STM32 board

Read from Pico USB COM10 after the user reported blue for about 20 seconds and
then solid green. The reader sent no programming commands and did not reset
either device.

```text
PICO-PROGRAMMER recovery-v1 result=0 stage=4 address=e000edf0 detail=00000000 loader=1 completed=16384 retry=1 elapsed_ms=19939
FAST-FIRST address=08000100 detail=000002a0 loader=4 STAGES_MS=364,352,12679,6521,20
```

The repeated reports were identical. Result 0 confirms the final programming
and full byte verification passed. `retry=1` confirms standard-only recovery
was used: this run does NOT demonstrate successful fast programming.

| Stage (aggregated across attempts) | Milliseconds |
|---|---:|
| Connect/setup | 364 |
| Erase/blank check | 352 |
| Program | 12679 |
| Full verification | 6521 |
| Restart | 20 |
| Total reported | 19939 |

Individual stage totals sum to 19936 ms; separate integer rounding accounts
for the 3 ms difference. The program stage includes the failed fast attempt
and the standard retry; it is not a standalone fast-programming benchmark.

The first failure was at 0x08000100, loader status 4 (FLASH_ERROR), detail
0x2A0 = PGAERR (0x20) | PGSERR (0x80) | FASTERR (0x200). MISERR (0x100) was
not set. These bits identify an alignment/sequence failure, not the earlier
missing-EOP condition. See [RM0351 programming errors](https://www.st.com/resource/en/reference_manual/dm00083560-stm32l4x5-and-stm32l4x6-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf).

Inspection of the actual source binary shows BOTH rows at 0x08000000 and
0x08000100 are dense and eligible for fast programming. The failure is at the
SECOND fast row, not a standard-to-fast transition. The first row completed
before reaching that address. The latter address is 256-byte aligned.
The current code clears and re-enables FSTPG between rows; that sequence needs
investigation against ST's multi-row flow. The register report alone does not
establish the precise controller constraint or prove a particular fix.

The recovery UF2 SHA-256 is
`371527e810aad13c416a466cb2899fd5550f965f1e58b6996638a81535d5ef5c`.
It remains the working artifact. No firmware was changed during this diagnostic
read. One observed success is not four-socket fixture qualification.
