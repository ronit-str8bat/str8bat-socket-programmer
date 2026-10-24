# Str8bat factory Master/Slave release v21

## Fixed hardware pairing

This V21 release has fixed factory pairing. No Slave ID is entered on the web page.

| Role | Fixed Pico board ID | Wi-Fi name |
|---|---|---|
| Master | `882078473D7590B2` | `Str8bat_Master_882078473D7590B2` |
| Slave 1 | `16AB9FF57CE41A89` | `Str8bat_Slave_16AB9FF57CE41A89` |

The Master serves a factory BIN only to the listed Slave. The Slave searches for and connects only to the listed Master Wi-Fi network.

## Release files

| File | LED wiring | Install on |
|---|---|---|
| `str8bat_factory_master_v21_common_cathode.uf2` | Common cathode | The Pico with ID `882078473D7590B2` |
| `str8bat_factory_slave_v21_common_cathode.uf2` | Common cathode | The Pico with ID `16AB9FF57CE41A89` |

RGB LED1 uses GP7, GP8 and GP9. LED2 uses GP5. The STM32 BIN embedded in these two builds is `production_firmware_v4.0.2.bin`, version `v4.0.2`.

## Add the next two Slaves

Open `standalone/include/wifi_config.h` and replace the empty strings below with the 16-character IDs shown in the future Slave Wi-Fi names:

```c
#define FACTORY_SLAVE_ID_2 ""
#define FACTORY_SLAVE_ID_3 ""
```

Rebuild and install the **Master** UF2 after changing these values. The existing Slave UF2 does not need to change, because every Slave already knows the fixed Master ID.

## Programming cycle

1. Power the Master and Slave Picos.
2. Connect the phone to the Master Wi-Fi network and open `http://192.168.4.1`.
3. Press **Set clock from this phone**.
4. Select an STM32 BIN file and press **Upload BIN to slaves**.
5. The Master stores and CRC-verifies the received BIN, then supplies it to each allowed online Slave.
6. Each Slave CRC-verifies and stores its downloaded BIN. Connect its STM32, then press its GP18 button to program the stored BIN and create its CSV record.

A missing Slave does not stop the Master or the other Slaves. When that Slave later connects, it compares its BIN CRC with the Master and receives the current BIN if needed.

## Storage and LED behavior

- The factory BIN, CSV records and Wi-Fi settings use separate Pico flash areas.
- LED1 rotates red, green and blue during Master upload/distribution and while a Slave receives a factory BIN.
- Each Pico stores its own CSV. Connect to the individual Pico Wi-Fi page to download that Pico's CSV.
