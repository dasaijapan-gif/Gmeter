# Gmeter: guide for the hardware supplier

Gmeter is a g-force meter for a car. It uses eight transparent OLED panels
stacked one behind the other, so the picture has depth. An ESP32 reads an
accelerometer and draws animations that react to braking, acceleration,
corners and bumps.

This package contains:

| Folder | What it is |
| --- | --- |
| `firmware/` | Ready-to-flash files. You do not need to compile anything. |
| `source/gmeter/` | The complete source code (a PlatformIO project), if you want to build it yourself. |
| `docs/` | Hardware notes, effect descriptions, code layout and licences. |
| `studio/` | Gmeter Studio: open `gmeter_studio.html` in Chrome or Edge to see the animations in 3D on a PC. The 3D view needs an internet connection; offline it shows a flat preview. |

## 1. Which file to flash

| Your board | Panels | File |
| --- | --- | --- |
| Classic ESP32 DevKit: ESP32-WROOM-32, WROOM-32E or WROVER, 4 MB flash or more | DFRobot DFR0934 or other WiseChip 128x56 transparent OLED | `firmware/esp32_128x56/gmeter_esp32_128x56_full.bin` |
| ESP32-S3 board with 4 MB or more of quad flash, with or without PSRAM (for example ESP32-S3-DevKitC-1 N8, N8R2, N8R8, N16R8) | DFRobot DFR0934 or other WiseChip 128x56 transparent OLED | `firmware/esp32s3_128x56/gmeter_esp32s3_128x56_full.bin` |
| Classic ESP32 DevKit, 4 MB flash or more | ordinary SSD1309 128x64 OLED modules | `firmware/esp32_128x64/gmeter_esp32_128x64_full.bin` |

Each `_full.bin` file is a complete image. Write it at address **0x0**. The
separate parts and their addresses are in each folder's `flash_args.txt`, for
tools that want them. `firmware/SHA256SUMS.txt` lets you check that the files
arrived intact.

ESP32-S3 modules with octal flash (names ending in V, such as N32R8V) are not
covered by these files.

## 2. How to flash

Connect the board by USB. Install the USB driver if the PC does not see a
serial port (CP210x or CH340, depending on the board).

**Option A, in the browser (easiest).** Open
<https://espressif.github.io/esptool-js/> in Chrome or Edge. Click
**Connect** and pick the board's port. Set **Flash Address** to `0x0`, choose
the `_full.bin` file, then click **Program**. When it finishes, press the
board's EN/RST button.

**Option B, command line.** Install esptool (`pip install esptool`). Open a
terminal in this package's folder (the one with `README_FIRST.md`), then:

```sh
# Classic ESP32 (replace COM5 with your port, e.g. /dev/ttyUSB0 on Linux)
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash 0x0 firmware/esp32_128x56/gmeter_esp32_128x56_full.bin

# ESP32-S3
esptool.py --chip esp32s3 --port COM5 --baud 460800 write_flash 0x0 firmware/esp32s3_128x56/gmeter_esp32s3_128x56_full.bin

# Classic ESP32 with 128x64 panels
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash 0x0 firmware/esp32_128x64/gmeter_esp32_128x64_full.bin
```

On esptool version 5 the command is `esptool` and `write-flash` (with a dash).

**Option C, Windows Flash Download Tool.** Choose the chip type (ESP32 or
ESP32-S3), add the `_full.bin` file at `0x0`, tick it, then click START.

If flashing does not start, hold the board's BOOT button, press EN/RST once,
then release BOOT.

**Saved settings.** Writing a `_full.bin` also clears the saved settings and
calibration, because the image covers the settings area at 0x9000. After every
full flash, repeat the checks and calibration in section 5 and type `save`.

To update a unit that is already set up and keep its settings, write only
`firmware.bin` at `0x10000` (same address on both chips), for example:

```sh
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash 0x10000 firmware/esp32_128x56/firmware.bin
```

Do this only if `partitions.bin` has the same SHA-256 in the old and the new
package (see `firmware/SHA256SUMS.txt`). Otherwise use the `_full.bin`. To wipe
a board completely first: `esptool.py --chip esp32 --port COM5 erase_flash`
(esptool 5: `esptool --chip esp32 --port COM5 erase-flash`).

## 3. First test: board only (no panels, no sensor)

Open a serial monitor at **115200 baud**. Any line ending works. Press
EN/RST. After a few lines from the chip's boot ROM (`ets` or `ESP-ROM`,
`rst:`, `load:`, `entry`), you should see:

```
gmeter: 128x56x8 voxels, panel model 0, spi 10000000 Hz, target 60 fps
no saved settings; using defaults
imu: not found (WHO_AM_I 0x00); showing demo scenario 'drive' and retrying. Type: help
>
```

With the 128x64 file the first line reads
`gmeter: 128x64x8 voxels, panel model 1, ...`.

Without a sensor the firmware plays a recorded demo drive, so the animations
keep moving. Type `status`. The `render` line should show about 60 fps. Type
`help` to list every command.

On the ESP32-S3-DevKitC-1 the serial monitor is on the USB connector labelled
**UART**. The other USB connector (native USB) is only used for flashing. If
your S3 board has only a native USB connector, build from source (section 6)
with `-DARDUINO_USB_CDC_ON_BOOT=1` added to the `esp32-s3` build flags.

## 4. Wiring

All eight panels share the clock, data, D/C and reset lines. Each panel has
its own chip-select (CS) wire. Panel 0 is the one nearest the driver; panel 7
is nearest the windshield.

**Classic ESP32 (WROOM-32, WROOM-32E or WROVER DevKit)**

| Signal | ESP32 GPIO | Connects to |
| --- | --- | --- |
| SCK (clock) | 18 | SCK/CLK on all 8 panels |
| MOSI (data) | 23 | MOSI/DIN on all 8 panels |
| D/C | 19 | DC on all 8 panels |
| RES (reset) | 13 | RES/RST on all 8 panels, plus a 10 kΩ pull-up to 3.3 V |
| CS panel 0 ... 7 | 5, 32, 14, 4, 27, 26, 25, 33 | CS of panel 0, 1, 2, 3, 4, 5, 6, 7 |
| SDA | 21 | MPU-6050 (GY-521) SDA |
| SCL | 22 | MPU-6050 (GY-521) SCL |
| Button | 0 | the board's BOOT button (already on the board) |

**ESP32-S3**

| Signal | ESP32-S3 GPIO | Connects to |
| --- | --- | --- |
| SCK (clock) | 12 | SCK/CLK on all 8 panels |
| MOSI (data) | 11 | MOSI/DIN on all 8 panels |
| D/C | 13 | DC on all 8 panels |
| RES (reset) | 14 | RES/RST on all 8 panels, plus a 10 kΩ pull-up to 3.3 V |
| CS panel 0 ... 7 | 10, 4, 5, 6, 7, 15, 16, 17 | CS of panel 0, 1, 2, 3, 4, 5, 6, 7 |
| SDA | 8 | MPU-6050 (GY-521) SDA |
| SCL | 9 | MPU-6050 (GY-521) SCL |
| Button | 0 | the board's BOOT button (already on the board) |

**Power**

- Panels and ESP32 use 3.3 V logic. Join all grounds.
- Each DFR0934 makes its own 12 V for the glass from its supply pin.
- For a bench test, one or two panels can run from the board's 3V3 pin. For
  all eight, use a separate 3.3 V buck converter rated 2 A or more.
- The GY-521 sensor module takes 5 V on its VCC pin (it has its own regulator).
- In the car: 12 V to a 5 V buck converter, then 3.3 V. Never connect the car's
  12 V directly to the panels or the board.
- Keep the SPI wires short (under about 10 cm). For longer wires, add 22-33 Ω
  series resistors on SCK and MOSI near the ESP32.

More detail is in `docs/HARDWARE.md`.

## 5. Panel and sensor checks

1. **Panel order.** Type `test 0`. Panel 0 (nearest the driver) shows a frame
   and the text `PANEL 0`. Repeat with `test 1` ... `test 7`. If the wrong panel
   lights, its CS wire is on the wrong GPIO. Type `test off` to return to the
   animation.
2. **Brightness.** The rear panels are seen through the front ones, so they
   are driven brighter. `ramp 80 255` sets the front and back contrast and is
   kept by `save`. `contrast 3 200` changes one panel for testing only: it is
   not saved, and the next `ramp` overwrites it.
3. **Effects.** `list` shows them. `mode hud`, `mode atfield`, `mode sync`,
   `mode slosh`, `mode smoke` and so on switch between them. `cycle 10` shows
   each for 10 seconds (`cycle 0` stops). A short press of the BOOT button also
   switches to the next effect. Holding BOOT for more than 2 seconds starts a
   rest calibration (like `cal rest`), so only do that with the car parked
   level.
4. **Sensor.** With the GY-521 connected and the board reset, the third line
   says `imu: MPU-6050 found`. `status` shows the raw reading; at rest its
   size should be close to 1 g.
5. **Calibration (in the car).**
   1. `cal fwd +y`: tell the firmware which sensor axis points to the
      windshield (`+x`, `-x`, `+y`, `-y`, `+z` or `-z`; `+y` is an example).
   2. Park on level ground, type `cal rest`, keep still and wait for
      `rest calibration ok`. If it says the forward axis is unknown, step 3
      is required.
   3. Recommended, for best accuracy: on a straight, empty road, a passenger
      types `cal brake` and the driver brakes firmly at that moment for 1.5
      seconds (the capture starts when Enter is pressed). Wait for
      `forward axis set from braking`. Repeat if it reports the braking was too
      weak.
   4. Type `save`. Always do this last.

   `source/gmeter/README.md` explains the calibration in more detail.
6. `save` keeps the mode, the contrast ramp, readout, invert, filter, auto
   cycle, panel refresh, forward axis and calibration after power-off.

## 6. Building from source (optional)

1. Install Visual Studio Code and its **PlatformIO IDE** extension (or the
   PlatformIO command line, `pip install platformio`).
2. Open the folder `source/gmeter`. PlatformIO reads `platformio.ini`.
3. Pick the environment: `esp32dev` (classic ESP32, 128x56 panels, default),
   `esp32-s3`, or `esp32dev-128x64`.
4. Click **Upload** (or run `pio run -e esp32dev -t upload`). The first build
   downloads the ESP32 toolchain, which takes a few minutes.

The platform is pinned to `espressif32 @ 7.1.3` (Arduino-ESP32 core 2.0.17,
GCC 8.4.0). This is the exact toolchain the files in `firmware/` were built
with. The Arduino IDE is not set up for this project; use PlatformIO.

Pins are in `source/gmeter/src/board_config.hpp`. Panel size, panel type and
SPI speed are `-D` flags in `platformio.ini`. To lower the SPI speed (for long
wires), change `-DGM_SPI_HZ=10000000` to `-DGM_SPI_HZ=8000000` in
`platformio.ini` and rebuild. To add custom artwork, draw it in Gmeter Studio,
copy the C++ header it makes, save it as `src/user_animation.hpp` and rebuild.

## 7. What has been tested

| Check | Result |
| --- | --- |
| Builds with the pinned toolchain (all three variants) | Pass, no compiler warnings |
| Memory, classic ESP32 | 87 KB of the 124 KB static RAM segment, 370 KB of 1.3 MB app flash |
| Boots and runs every effect in Espressif's ESP32 and ESP32-S3 emulator (QEMU), no sensor attached | Pass: no crash, no reset, about 60 fps, no memory leak |
| Pin set-up: every panel line is an output after start-up, no pin used twice or reserved | Pass (PC test against the Arduino core's pin handling, and a pin trace in the emulator) |
| Core library unit tests on a PC (288 checks, 128x56 and 128x64) | Pass |
| Real panels and a real MPU-6050 | **Not yet tested.** Please report the results of section 5. |

## 8. Troubleshooting

| Problem | What to check |
| --- | --- |
| No serial output | Baud rate 115200; on the S3 use the UART connector; press EN/RST. |
| The board restarts over and over and prints `assert failed` | The flash chip is smaller than the image expects, or it is octal flash. Use the file for your board in section 1, or build from source for that board. |
| `imu: not found` with the sensor connected | SDA/SCL wires, 5 V on the GY-521, address 0x68 (AD0 to GND). |
| A panel stays dark | Its CS wire, its 3.3 V and ground, and the shared RES wire. Try `test n` for that panel. |
| All panels dark | SCK, MOSI, D/C and RES wiring; 3.3 V supply current. Try `reinit`. |
| Picture is noisy or shifts | Shorter SPI wires, series resistors, or a lower SPI speed (section 6). |
| Rear panels too dim | `ramp 60 255`; check that all panels face the same way. |
| Settings lost after flashing | Expected after a `_full.bin`: calibrate again and `save` (section 2). |
| Board resets in the car | Power supply too weak or noisy: use a proper buck converter and short, thick ground wires. |
