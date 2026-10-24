# Diagnostic: keep the STM32 halted after verification

Use `build-standalone/standalone_programmer-fast-v3-halt.uf2`.
SHA-256: `c3af2eb625a5ace5c235ece31c728e8cb620c7d698605ad3af3e783768982e0b`.
This build is ready but has not been installed on the Pico by the build tools.

## Behavior

Programming, fast rows, sparse writes, CRC and the complete 524288-byte readback
comparison are unchanged. After comparison, this diagnostic build explicitly
sets C_DEBUGEN/C_HALT and checks S_HALT. It does not pulse NRST, disable debugging
or resume the application. It releases SWCLK/SWDIO to inputs for another probe.
NRST remains released (input), rather than being held low. Existing watchdog
debug-freeze and reset-vector catch remain enabled.

Solid green now means **verified and left halted**. It does not mean the product
has started. If the final halt confirmation fails, the job fails red and holds
NRST low as with other failures. A new START still begins a new programming job.

The USB report identifies `PICO-PROGRAMMER fast-v3-halt` and includes `halted=1`
on success. This records the condition when the job ended; it is not continuously
monitored after handoff to another debugger.

## Compare without starting the application

1. Install this UF2 on Pico using BOOTSEL, then power/connect the STM32 as usual.
2. Wait for blinking green, ground GP6 for at least 100 ms, then release it.
3. Wait for solid green. Optionally read the USB report and confirm `result=0`
   and `halted=1` using `read-standalone-report.ps1`.
4. **Keep STM32 power uninterrupted.** Do not reset the board, power-cycle it,
   press START again, or let a second tool start execution during the comparison.
5. Hand over SWD to the probe used by CubeProgrammer. Disconnect Pico SWCLK,
   SWDIO and NRST from the target when swapping probes; retain target power
   and the appropriate common ground. Do not let two active probes drive SWD.
6. Select SWD **Hot plug** connection mode in CubeProgrammer before connecting.
   ST documents this mode as connecting without halt/reset. The processor is
   already halted by Pico. Avoid reset/Run/Start execution/erase/program actions.
7. Refresh/read 0x08000000 for 0x80000 bytes and compare against the original
   production .bin, including 0x08040000..0x0806C7BF and 0x0807E800.

Reference: [ST CubeProgrammer connection modes](https://dev.st.com/stm32cube-docs/prog/2.23.0/en/docs/markup/CubeProg_UserManual/Main_window.html).

Do not use Run to boot the product from the halted helper's SRAM PC. Once the
comparison is finished and any dump saved, disconnect the debugger and power-cycle
the target to boot normally. The application may then update BLE and erase its
staging payload; that is precisely what this pre-boot comparison isolates.

## Rebuild and restore

No rebuild is needed to use the supplied UF2. To reproduce it:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-standalone.ps1 -HaltAfterVerify
```

The script explicitly selects ON with the switch and OFF without it. Direct
CMake users can set `-DSTANDALONE_HALT_AFTER_VERIFY=ON` (default OFF). After a
diagnostic build the ordinary build directory's primary UF2 is also diagnostic;
use the named files to distinguish them.

The previously tested normal fast-v3 UF2 is preserved unchanged at
`build-standalone/standalone_programmer-fast-v3.uf2`, SHA-256
`e4843ce452a3c1a684a65668471eca23a68a43f163cf57795ee0d9e6a54720f5`.
Reinstall it to restore automatic application startup after programming.

## Checks completed

Pico SDK build and artifact validation passed. The loader and embedded product
bytes remain unchanged. Host tests cover fast and standard programming, both
normal and halt exits, full production-image comparison, recovery, mismatches,
and a lost final halt. Successful diagnostic exits make no post-verification
reset/resume calls and release the SWD lines. A physical CubeProgrammer handoff
and comparison remain to be performed on the user's board.
