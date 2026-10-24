# Recovery build after the repeated seven-blink failure

The first EOPIE correction was a real code fix but did not resolve the user's
hardware failure. Seven blinks alone does not identify its register-level cause.
Do not describe the fast programming path as hardware-qualified or fixed yet.

## Behavior

* Keep the 16 KiB transfer buffer and try the fast/sparse algorithm first.
* On a programming-stage flash-controller error or missing fast EOP only, save
  the original loader status, FLASH_SR detail and address.
* Reset/halt and reinitialize the target, upload the helper again, MASS ERASE
  the complete main flash again and blank-check it. Then retry once using only
  standard double-word writes. Never rewrite a partly programmed row directly.
* Standard mode uses the established busy/error polling completion sequence;
  it does not require EOP. Both modes still require full byte-for-byte readback.
* No retry for communication/CRC, protection, timeout, invalid helper or final
  verification failures. A failed recovery stays red with reset asserted.
* Green can mean successful standard recovery. It is not proof of fast-mode
  success. The report records `retry=1` so the distinction is visible.
* Each START is a new attempt, with at most one recovery. A recovery cycle can
  take longer than a successful standard-only cycle.

## Read-only USB report

The optional CDC interface initializes with zero host wait and prints only while
idle and a USB reader is connected. Normal standalone operation requires no PC.
The report repeats every two seconds, so it can be collected after a failure.
Programming still starts only from GP6. The serial interface reads no commands.

With the new UF2 installed, leave the Pico powered/USB-connected after one test:

```powershell
powershell -ExecutionPolicy Bypass -File .\read-standalone-report.ps1
```

If multiple Pico serial ports exist, specify `-Port COMx`. Close another terminal
that already owns that port. The reader does not flash or reset either device.

`PICO-PROGRAMMER recovery-v1` identifies the build. Report fields:

| Field | Meaning |
|---|---|
| result | Existing LED code: 0 success, 7 flash/helper error, 8 mismatch, etc. |
| stage | 0 connect, 1 erase/blank, 2 program, 3 verify, 4 restart |
| address/detail | Final operation's address and raw error/status value |
| loader/completed | Last helper return status and bytes completed in that call |
| retry | 1 if clean standard recovery was attempted |
| FAST-FIRST | Preserved fast failure address, detail and helper status |
| STAGES_MS | Aggregate time per stage, including recovery if used |

Loader values: 1 success, 2 invalid request, 3 timeout, 4 flash error, 5 not blank,
6 uploaded block CRC failure, 7 missing fast EOP, 8 invalid clock setup. For a
flash error, FLASH_SR detail bits include PROGERR=0x08, WRPERR=0x10,
PGAERR=0x20, SIZERR=0x40, PGSERR=0x80, MISERR=0x100, FASTERR=0x200.
Interpret these only when the helper status indicates a flash error; other
statuses can use detail for different data. The report is lost on power-off.

## Validation and remaining work

Host tests cover partial fast writes followed by a second erase, successful
standard recovery, both attempts failing, recovery blank-check/verify failures,
one-retry limit and no retry after a timeout. Actual compiled ARM loader tests
cover both modes and the supplied production image. USB delivery and physical
flash timing require testing on the user's board.

The artifact to upload is `build-standalone/standalone_programmer-recovery.uf2`.
The retained original working UF2 remains at
`build-standalone/previous-2kb-standard/standalone_programmer.uf2`.
