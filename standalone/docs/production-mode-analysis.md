# Analysis of the eight zero bytes at 0x0807E800

Analyzed input: `production_firmware_v4.0.2.bin`, 524288 bytes.
SHA-256: `3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9`.

The supplied binary contains FF throughout 0x0807E800..0x0807EFFF. The screenshot
instead shows 00000000 at 0x0807E800 and 0x0807E804: one programmed double word.

Disassembly of this actual binary, rather than the older source tree, shows:

| Address | Behavior |
|---|---|
| 0x0800CCF6 | Calls the initializer at 0x0801204C |
| 0x08012050..0x08012058 | Loads the word at 0x0807E800 and compares it with FFFFFFFF |
| 0x0801205A..0x08012068 | On erased data, calls 0x08011F94 with argument zero |
| 0x08011FAA..0x08011FC2 | Selects low/high data words both zero; calls the flash driver with destination 0x0807E800 |
| 0x0802B9CA..0x0802B9D6 | Selects standard double-word programming for type zero |
| 0x08012218 | Literal containing the destination address 0x0807E800 |

`tests/inspect_production_mode.py` executes the actual initializer and wrapper
Thumb instructions in Unicorn with flash-driver calls stubbed. An erased input
produces exactly `(type=0, address=0x0807E800, data_low=0, data_high=0)`. An already
zero input produces no new programming call. This verifies the binary's intended
write; it does not capture a live execution trace from the user's board.

The normal standalone programmer verifies every image byte with the CPU halted,
then restarts the product. Consequently, a successful pre-boot comparison and a
different later memory comparison are compatible: the product can modify this
configuration field after starting. The screenshot matches that behavior.
Confirmation of solid green before the external comparison distinguishes this
case from a reported internal verification failure. Other differing addresses
must be assessed separately.

Do not change the embedded image or exclude this field from the programmer's
verification to hide the difference. A strict external comparison must be made
before application execution, with the target kept halted and no boot/reset
between programming and comparison. The older 3.6.15 source uses a different
factory marker (0x12345678), so it is not the authority for this 4.0.2 value.

No Pico firmware behavior or UF2 was changed as part of this analysis.
