# Debugprobe

Firmware source for the Raspberry Pi Debug Probe SWD/UART accessory. Can also be run on a Raspberry Pi Pico or Pico 2.

[Raspberry Pi Debug Probe product page](https://www.raspberrypi.com/products/debug-probe/)

[Raspberry Pi Pico product page](https://www.raspberrypi.com/products/raspberry-pi-pico/)

[Raspberry Pi Pico 2 product page](https://www.raspberrypi.com/products/raspberry-pi-pico-2/)

## Documentation

Debug Probe documentation can be found at the [Raspberry Pi documentation](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html#about-the-debug-probe) and in the [Getting Started with Pico PDF](https://pip.raspberrypi.com/documents/RP-008276-DS).

## Hacking

For the purpose of making changes or studying of the code, you may want to compile the code yourself.

First, clone the repository:
```bash
git clone https://github.com/raspberrypi/debugprobe
cd debugprobe
```

Initialize and update the submodules:
```bash
 git submodule update --init --recursive
```

Then create and switch to the build directory:
```bash
 mkdir build
 cd build
```

If your environment doesn't contain `PICO_SDK_PATH`, then either add it to your environment variables with `export PICO_SDK_PATH=/path/to/sdk` or add `-DPICO_SDK_PATH=/path/to/sdk` to the arguments to CMake below.

Run cmake and build the code:
```bash
 cmake ..
 make
```

Done! You should now have a `debugprobe.uf2` that you can upload to your Debug Probe via the UF2 bootloader.

## Building for the Pico 1

If you want to create the version that runs on the Pico, then you need to invoke `cmake` in the sequence above with the `DEBUG_ON_PICO=ON` option:
```bash
cmake -DDEBUG_ON_PICO=ON ..
```

This will build with the configuration for the Pico and call the output program `debugprobe_on_pico.uf2`, as opposed to `debugprobe.uf2` for the accessory hardware.

Note that if you first ran through the whole sequence to compile for the Debug Probe, then you don't need to start back at the top. You can just go back to the `cmake` step and start from there.

## Building for the Pico 2

If using an existing debugprobe clone:
- You must completely regenerate your build directory, or use a different one.
- You must also sync and update submodules.
- `PICO_SDK_PATH` must point to a version 2.0.0 or greater install.

```bash
git submodule sync
git submodule update --init --recursive
mkdir build-pico2
cd build-pico2
cmake -DDEBUG_ON_PICO=1 -DPICO_BOARD=pico2 ../
```

This will build with the configuration for the Pico 2 and call the output program `debugprobe_on_pico2.uf2`.

## Building for the Pico 2 W

Use a Pico SDK version that provides the `pico2_w` board (2.1.0 or newer).
With CMake, Ninja and the Arm GNU toolchain available in your terminal, run:

```bash
git submodule update --init --recursive
cmake -S . -B build-pico2w -G Ninja -DDEBUG_ON_PICO=ON -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="/path/to/pico-sdk"
cmake --build build-pico2w
```

Copy `build-pico2w/debugprobe_on_pico2_w.uf2` to the Pico 2 W's `RP2350`
drive while it is in BOOTSEL mode. The USB connection LED is disabled on
this board because its LED is controlled through the wireless chip; GP25
is the wireless chip-select signal, not an LED. Wi-Fi is not used.

The target connections are GP2 for SWCLK, GP3 for SWDIO, common GND, and
optionally GP1 for target reset. Power the target separately with compatible
3.3 V debug signals.

### Programming the custom STM32L476 board on Windows

`flash-stm32l476.ps1` uses the OpenOCD installation bundled with this
laptop's STM32CubeIDE 2.2.0. The tested configuration uses a software system
reset over SWD at 100 kHz; no target reset button is required.

Check the connection (restarts the target, without erasing or writing flash):

```powershell
.\flash-stm32l476.ps1 -SpeedKHz 100 -SoftwareReset
```

Program the selected production image, verify it, then restart the target:

```powershell
.\flash-stm32l476.ps1 -FirmwarePath "D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin" -Address 0x08000000 -SpeedKHz 100 -SoftwareReset
```

Use the actual linked address for other raw binaries. `-SoftwareReset` and
`-ConnectUnderReset` are alternative reset methods; do not combine them.
The helper preserves the target clock setup and clears the observed stale
flash error flags before programming. It does not change protection options.

### Faster STM32L476 programming (optional)

Use `-FastClock -SoftwareReset` to connect at at most 500 kHz, validate the
reset clock state, set three flash wait states, switch MSI to 24 MHz, then
raise SWD to the requested speed. The application configures its own clocks
after the final reset. Keep `stm32l476-fast-clock.cfg` beside the helper.
No Pico UF2 rebuild is required; omit `-FastClock` for the earlier behavior.

```powershell
powershell -ExecutionPolicy Bypass -File .\flash-stm32l476.ps1 -ClockInfo -SoftwareReset -SpeedKHz 2000 -FastClock
powershell -ExecutionPolicy Bypass -File .\flash-stm32l476.ps1 -FirmwarePath "D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin" -Address 0x08000000 -SpeedKHz 2000 -SoftwareReset -FastClock
```

A full 512 KiB flash/verify run passed in 18.57 seconds, versus 31.95 seconds
at the reset CPU clock. Verification fell from about 15.52 to 2.65 seconds.
A subsequent 4000 kHz trial failed during initial connection at 500 kHz,
before clock changes or erase/write. Reconnection remains intermittent.

## AutoBaud Mode

Mode which automatically detects and sets the UART baud rate as data arrives.

To enable AutoBaud, configure the USB CDC port to the following custom baud rate:
```
9728 (0x2600)
```
> **Note:** Some Linux serial tools cannot set custom baud values. PuTTY on Windows and any terminal that supports arbitrary baud rates works.

Changing the baud rate to any other value disables AutoBaud.

