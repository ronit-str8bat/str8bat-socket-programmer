# Str8bat factory Master/Slave release v2

## Files

| File | LED wiring | Install on |
|---|---|---|
| `str8bat_factory_master_v2_common_anode.uf2` | Common anode | One Master Pico 2 W |
| `str8bat_factory_slave_v2_common_anode.uf2` | Common anode | Each Slave Pico 2 W |
| `str8bat_factory_master_v2_common_cathode.uf2` | Common cathode | One Master Pico 2 W |
| `str8bat_factory_slave_v2_common_cathode.uf2` | Common cathode | Each Slave Pico 2 W |

Choose one LED wiring type for all four Picos. RGB LED1 is on GP7/GP8/GP9 and LED2 is on GP5. The STM32 target BIN embedded in all builds is `str8bat_factory_v2.0_user_v2.0.3.bin` (512 KiB).

## First-time setup

1. Install the **Master** UF2 in exactly one Pico.
2. Install the same **Slave** UF2 in each of the other three Picos.
3. Power all four Picos. Each Pico derives its own suffix from its permanent Pico hardware ID:
   - Master: `Str8bat_Master_<unique_id>`
   - Slave: `Str8bat_Slave_<unique_id>`
4. Record the three slave IDs. They are the final 16 characters shown in their Wi-Fi names.
5. Connect the phone to the Master Wi-Fi network and open `http://192.168.4.1`.
6. Press **Set clock from this phone**. This gives the Master a date/time for the new CSV field.
7. Enter the exact three slave IDs, separated by commas, and press **Save slave IDs**. The Master will serve factory work only to these three Picos.

## Factory programming cycle

1. Connect the phone to the Master Wi-Fi and select a 512 KiB STM32 `.bin` file.
2. Press **Upload BIN to slaves**.
3. The Master erases its old factory-BIN slot, receives the replacement in chunks, and verifies the full CRC32 before making it available.
4. Each paired slave discovers the Master, requests the verified file, erases its own old factory-BIN slot, receives it, and verifies the full CRC32 before it starts SWD programming.
5. Every slave then programs and verifies the STM32 connected to that Pico. The slave reports its result to the Master.

## Automatic startup authentication

On every power-up, each online paired Slave reports the CRC32 checksum of its current active BIN to the Master. The Master compares that checksum against its current authoritative BIN:

- When the checksums match, the Slave continues normally and no BIN is transferred.
- When the checksums differ, the Master sends the full BIN only to that Slave. The Slave erases its old factory BIN, receives the replacement, verifies its CRC32, and then programs its STM32.
- A Master with no uploaded factory BIN uses its embedded 512 KiB production BIN as the authoritative image. After a successful phone upload, the uploaded and verified BIN becomes authoritative.
- An offline Slave does not block the Master, other Slaves, phone BIN uploads, or normal programming. When that Slave later reconnects, it performs the checksum comparison and receives the current BIN if needed.

## LED behavior during distribution

- LED1 rotates red, green, and blue while a Master is uploading or serving the factory BIN.
- LED1 rotates red, green, and blue while a Slave receives the factory BIN.
- After the transfer, the existing programming and status indications continue unchanged.

## CSV records

The CSV remains separate on every Pico and is downloaded individually from that Pico's own local web page. Each new programming record includes:

- `pico_ssid`: the Pico that programmed the STM32.
- `bin_received_date_time`: when that Pico received the factory BIN. It remains `UNAVAILABLE` until the relevant Pico has a valid clock.
- `Image CRC32`: CRC32 of the BIN used for the recorded SWD programming operation.
- The existing checksum and individual error columns.

## Storage separation and recovery

- Factory BIN storage: a dedicated 512 KiB BIN area plus a 4 KiB committed header.
- CSV storage: separate 1 MiB area.
- Wi-Fi credentials: separate sector.
- Master pairing list: separate 4 KiB sector.

The new header is written only after the complete 512 KiB file CRC has passed. A power loss, Wi-Fi loss, bad chunk, or CRC mismatch leaves no valid partial BIN available for programming. The slave clears an interrupted download and retries it from the Master.

## Required production acceptance test

Perform this test before releasing the firmware to production hardware:

1. Confirm all four SSIDs appear with their expected Master/Slave prefixes.
2. Pair the three intended slave IDs on the Master.
3. Upload one known 512 KiB BIN and confirm all three STM32 targets program successfully.
4. Download the CSV from every slave and confirm its SSID, receipt time, CRC32, and programming result.
5. Repeat with a deliberate power removal or Wi-Fi interruption during transfer. Confirm that no slave programs a partial BIN, then repeat the upload and confirm recovery.
