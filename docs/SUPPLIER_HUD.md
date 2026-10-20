# Gmeter HUD edition: guide for the hardware supplier

Gmeter is a g-force meter for a car. It uses eight transparent OLED panels
stacked one behind the other, so the picture has depth. An ESP32 reads an
accelerometer and draws what the car is doing: braking, accelerating,
cornering and bumps.

This is the **HUD edition**. It always shows one display, a tactical screen in
the style of the NERV computers in *Neon Genesis Evangelion*, and stays on it
for as long as the unit has power. There is nothing to select: power it on and
it follows the g-force continuously.

This package contains:

| Folder | What it is |
| --- | --- |
| `firmware/` | Ready-to-flash files. You do not need to compile anything. |
| `source/gmeter/` | The complete source code (a PlatformIO project), if you want to build it yourself. |
| `docs/` | Hardware notes, effect descriptions, code layout and licences. |
| `studio/` | Gmeter Studio: open `gmeter_studio.html` in Chrome or Edge and choose the `hud` effect to see the display in 3D on a PC. The 3D view needs an internet connection; offline it shows a flat preview. |

## 1. What the HUD shows

Panel 0 is the one nearest the driver; panel 7 is nearest the windshield.

| Where | What |
| --- | --- |
| Panel 0 (front) | A border with cut corners and corner brackets. `横G` (lateral g) and `縦G` (longitudinal g) with their values at the top. Rulers along the bottom and left edges, with markers that follow the ball. A status column on the right: `正常` (normal), or `警告` (warning) blinking above 0.7 g, with moving hazard stripes along the bottom. A target bracket that follows the ball and gets smaller as the ball moves deeper. |
| Panel 3 (middle) | Lateral and vertical axis lines, with marks at 0.5 g and 1 g on the lateral line and at 0.5 g on the vertical line. A dotted depth axis runs through every panel. |
| Through the stack | A line from the centre out to the current g-force, and a ball that follows it with a short trail. Braking throws the ball toward the windshield, accelerating pulls it toward the driver, and a right turn throws it to the left, like a loose object in the car. |
| Panel 2 | The driving state in large characters: `待機` standby, `加速` accelerating, `制動` braking, `左旋回` / `右旋回` turning left / right, `衝撃` impact (a bump), `危険` danger (above 1 g). It flashes when it changes; the standby word fades after 3 seconds. |
| Panel 0, bottom right | A small tag while the picture is **not** live sensor data: `DEMO` (no sensor found, a recorded drive is playing), `NO SENSOR` (the sensor stopped answering; the display shows 0 g), or `SERIAL` (g-force sent from a PC). With a working sensor there is no tag. |

**Made to stay on.** The border and labels never move on their own, and OLED
pixels that are always lit age faster than the rest. Every 2 minutes the whole
picture, tag included, moves by one pixel (right, down, left, up, and round
again), starting from a random one of these four positions at each power-on.
This spreads the wear of the thin lines and of the edges of the characters
over neighbouring pixels. The inside of the thicker character strokes still
stays lit, so the orbit slows burn-in but cannot prevent it completely. This
small step is intentional.

## 2. Which file to flash

| Your board | Panels | File |
| --- | --- | --- |
| Classic ESP32 DevKit: ESP32-WROOM-32, WROOM-32E or WROVER, 4 MB flash or more | DFRobot DFR0934 or other WiseChip 128x56 transparent OLED | `firmware/esp32_128x56_hud/gmeter_esp32_128x56_hud_full.bin` |
| ESP32-S3 board with 4 MB or more of quad flash, with or without PSRAM (for example ESP32-S3-DevKitC-1 N8, N8R2, N8R8, N16R8) | DFRobot DFR0934 or other WiseChip 128x56 transparent OLED | `firmware/esp32s3_128x56_hud/gmeter_esp32s3_128x56_hud_full.bin` |
| Classic ESP32 DevKit, 4 MB flash or more | ordinary SSD1309 128x64 OLED modules | `firmware/esp32_128x64_hud/gmeter_esp32_128x64_hud_full.bin` |

Each `_full.bin` file is a complete image. Write it at address **0x0**. The
separate parts and their addresses are in each folder's `flash_args.txt`, for
tools that want them. `firmware/SHA256SUMS.txt` lets you check that the files
arrived intact.

ESP32-S3 modules with octal flash (names ending in V, such as N32R8V) are not
covered by these files.

## 3. How to flash

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
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash 0x0 firmware/esp32_128x56_hud/gmeter_esp32_128x56_hud_full.bin

# ESP32-S3
esptool.py --chip esp32s3 --port COM5 --baud 460800 write_flash 0x0 firmware/esp32s3_128x56_hud/gmeter_esp32s3_128x56_hud_full.bin

# Classic ESP32 with 128x64 panels
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash 0x0 firmware/esp32_128x64_hud/gmeter_esp32_128x64_hud_full.bin
```

On esptool version 5 the command is `esptool` and `write-flash` (with a dash).

**Option C, Windows Flash Download Tool.** Choose the chip type (ESP32 or
ESP32-S3), add the `_full.bin` file at `0x0`, tick it, then click START.

If flashing does not start, hold the board's BOOT button, press EN/RST once,
then release BOOT.

If the board ran other firmware before, erase it once before the first flash:
**Erase Flash** in the browser tool, **ERASE** in the Flash Download Tool, or
`esptool.py --chip esp32 --port COM5 erase_flash` (`--chip esp32s3` for the
S3; esptool 5: `erase-flash`). Otherwise old data left in the core-dump area
makes the board print a harmless line such as
`E (...) esp_core_dump_flash: Incorrect size of core dump image` at every
boot.

**Saved settings.** Writing a `_full.bin` also clears the saved settings and
calibration, because the image covers the settings area at 0x9000. After every
full flash, repeat the checks and calibration in section 6 and type `save`.

To update a unit that is already set up and keep its settings, write only
`firmware.bin` at `0x10000` (same address on both chips):

```sh
# Classic ESP32
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash 0x10000 firmware/esp32_128x56_hud/firmware.bin

# ESP32-S3
esptool.py --chip esp32s3 --port COM5 --baud 460800 write_flash 0x10000 firmware/esp32s3_128x56_hud/firmware.bin
```

Do this only if `partitions.bin` has the same SHA-256 in the old and the new
package (see `firmware/SHA256SUMS.txt`). Otherwise use the `_full.bin`.

## 4. First test: board only (no panels, no sensor)

Open a serial monitor at **115200 baud**, set to send a line ending (CR, LF
or both; not "No line ending"). Press EN/RST. After a few lines from the
chip's boot ROM (`ets` or `ESP-ROM`, `rst:`, `load:`, `entry`), you should see:

```
gmeter: 128x56x8 voxels, panel model 0, spi 10000000 Hz, target 60 fps
no saved settings; using defaults
single-effect build: always shows hud
imu: not found (WHO_AM_I 0x00); showing demo scenario 'drive' and retrying. Type: help
>
```

With the 128x64 file the first line reads
`gmeter: 128x64x8 voxels, panel model 1, ...`.

Without a sensor the firmware plays a recorded demo drive, so the HUD keeps
moving and shows the `DEMO` tag. Type `status`: the first line should say
`mode hud` and the `render` line about 60 fps. Type `help` to list every
command.

On the ESP32-S3-DevKitC-1 the serial monitor is on the USB connector labelled
**UART**. The other USB connector (native USB) is only used for flashing. If
your S3 board has only a native USB connector, build from source (section 8)
with `-DARDUINO_USB_CDC_ON_BOOT=1` added to the `esp32-s3` build flags.

## 5. Wiring

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

## 6. Panel and sensor checks

1. **Panel order.** Type `test 0`. Panel 0 (nearest the driver) shows a frame
   and the text `PANEL 0`. Repeat with `test 1` ... `test 7`. If the wrong panel
   lights, its CS wire is on the wrong GPIO. Type `test off` to return to the
   HUD.
2. **HUD layout.** With `test off`, check each panel against section 1: the
   border and readouts on panel 0, the axis cross on panel 3, the state word
   on panel 2, and the ball moving through the stack.
3. **Brightness.** The rear panels are seen through the front ones, so they
   are driven brighter. `ramp 80 255` sets the front and back contrast and is
   kept by `save`. `contrast 3 200` changes one panel for testing only: it is
   not saved, and the next `ramp` overwrites it.
4. **Sensor.** With the GY-521 connected and the board reset, the fourth line
   says `imu: MPU-6050 found` and the `DEMO` tag disappears. `status` shows
   the raw reading; at rest its size should be close to 1 g.

   Before calibration the firmware does not know how the sensor is mounted.
   Unless the module stands with the Y arrow printed on it pointing straight
   up, the HUD shows about 1 g while the board is still: `横G` or `縦G` near
   ±1.00, `警告` with hazard stripes, the ball at an edge and a large word
   such as `加速` or `制動`. This is expected. At this stage only check the
   size in `status`. For a bench test of the display, lay the module flat,
   type `cal fwd +y` and then `cal rest`: the ball moves to the centre and
   follows when you tilt the board. Calibrate again in the car (step 5).
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

   Holding BOOT for more than 2 seconds also starts a rest calibration (like
   `cal rest`), so only do that with the car parked level. A short press of
   BOOT does nothing in this edition. `source/gmeter/README.md` explains the
   calibration in more detail.
6. `save` keeps the contrast ramp, readout, invert, auto-zero (`zero on|off`),
   filter, panel refresh, forward axis and calibration after power-off.

For a unit that runs for hours, `zero on` (then `save`) slowly removes the
sensor's temperature drift while the car is standing still.

The commands `mode`, `cycle` and `anim` are disabled in this edition: it always
shows the HUD. They answer with a short note instead.

## 7. Adjusting the HUD (optional)

`get` lists the HUD's settings. `set <name> <value>` changes one while the unit
runs, for trying values on the bench:

| Setting | Default | What it does |
| --- | --- | --- |
| `scale_g` | 1.0 | lateral g that moves the ball to the side edge |
| `dscale_g` | 1.0 | longitudinal g that moves the ball to the front or back panel |
| `vscale_g` | 0.6 | vertical g at the top or bottom edge |
| `hz` | 2.5 | how quickly the ball follows (higher is quicker) |
| `warn_g` | 0.7 | g that shows `警告` and the hazard stripes |
| `danger_g` | 1.0 | g that shows `危険` |
| `title_z` | 2 | panel for the large state word (0 = front) |
| `ball_r` | 4.5 | ball size in pixels |
| `trail_s` | 0.8 | trail length in seconds (0 = none) |
| `inertia` | 1.0 | 1 = the ball moves like a loose object in the car; 0 = it moves the other way, with the g-force |
| `enter_g` | 0.25 | g needed to change the state word |
| `hold_s` | 0.4 | shortest time a state word stays up, in seconds |
| `bump_g` | 0.3 | vertical jolt that shows `衝撃` |
| `orbit_min` | 2.0 | minutes between the one-pixel moves (0 = off) |
| `title`, `axes`, `frame` | 1 | 0 hides the state word, the axes, or the border and readouts |

`set` values last until the next power-off. To make a value permanent, change
its default in `source/gmeter/lib/gmeter_core/src/gmeter/effects/hud.hpp` and
build the HUD edition from source (section 8).

## 8. Building from source (optional)

1. Install Visual Studio Code and its **PlatformIO IDE** extension (or the
   PlatformIO command line, `pip install platformio`).
2. Open the folder `source/gmeter`. PlatformIO reads `platformio.ini`.
3. Pick the HUD environment: `esp32dev-hud` (classic ESP32, 128x56 panels),
   `esp32-s3-hud`, or `esp32dev-128x64-hud`.
4. Click **Upload** (or run `pio run -e esp32dev-hud -t upload`). The first
   build downloads the ESP32 toolchain, which takes a few minutes.

The HUD environments are the standard firmware with one extra flag,
`-DGM_FIXED_EFFECT=hud`. Without it (environments `esp32dev`, `esp32-s3`,
`esp32dev-128x64`) the firmware offers all ten effects.

The platform is pinned to `espressif32 @ 7.1.3` (Arduino-ESP32 core 2.0.17,
GCC 8.4.0). This is the exact toolchain the files in `firmware/` were built
with. The Arduino IDE is not set up for this project; use PlatformIO.

Pins are in `source/gmeter/src/board_config.hpp`. Panel size, panel type and
SPI speed are `-D` flags in `platformio.ini`. To lower the SPI speed (for long
wires), change `-DGM_SPI_HZ=10000000` to `-DGM_SPI_HZ=8000000` in
`platformio.ini` and rebuild.

## 9. What has been tested

| Check | Result |
| --- | --- |
| Builds with the pinned toolchain (all three HUD variants) | Pass, no compiler warnings |
| Memory, classic ESP32 | 89 KB of the 124 KB static RAM segment, 372 KB of 1.3 MB app flash |
| Boots straight into the HUD in Espressif's ESP32 and ESP32-S3 emulator (QEMU), no sensor attached; `mode`, `cycle` and `anim` refused | Pass: no crash or reset once running, about 60 fps |
| Start-up in the emulator | About 1 boot in 10 on the classic ESP32 emulator (with this and earlier Gmeter images alike) stops with `Guru Meditation ... Cache disabled but cached memory region accessed` inside the ESP32 framework's own start-up code, before Gmeter runs; the chip restarts itself and then runs normally. It looks like a timing effect of the emulator, which runs the two cores in turns. If a real board ever prints this right after power-on, please report it. |
| Settings kept after a reboot | Pass on the ESP32 emulator. The ESP32-S3 emulator cannot keep settings for any program, so check `save` on a real S3 board. |
| The one-pixel move keeps the whole border on the glass; the warning blink still works after 8 simulated days | Pass (PC unit tests) |
| Pin set-up: every panel line is an output after start-up, no pin used twice or reserved | Pass (PC test against the Arduino core's pin handling, and a pin trace in the emulator) |
| Core library unit tests on a PC (381 checks, 128x56 and 128x64) | Pass |
| Real panels and a real MPU-6050 | **Not yet tested.** Please report the results of section 6. |

## 10. Troubleshooting

| Problem | What to check |
| --- | --- |
| No serial output | Baud rate 115200; on the S3 use the UART connector; press EN/RST. |
| The board restarts over and over and prints `assert failed` | The flash chip is smaller than the image expects, or it is octal flash. Use the file for your board in section 2, or build from source for that board. |
| `DEMO` tag on the HUD | No sensor was found at power-on. Check the sensor wiring; it is picked up automatically when it answers. |
| `NO SENSOR` tag on the HUD | The sensor stopped answering (loose wire, power dip). The HUD shows 0 g and retries every second. |
| `imu: not found (WHO_AM_I 0x00)` with the sensor connected | SDA/SCL wires, 5 V on the GY-521, address 0x68 (AD0 to GND). |
| `imu: not found (WHO_AM_I 0xNN)`, NN not 00 | The wiring works, but the sensor chip is not a supported type (supported: 0x68 MPU-6050, 0x70 MPU-6500, 0x71 MPU-9250, 0x72, 0x73 MPU-9255, 0x98). Report the value. |
| The ball leans to one side when parked | Calibrate (section 6); `zero on` removes slow drift. |
| A panel stays dark | Its CS wire, its 3.3 V and ground, and the shared RES wire. Try `test n` for that panel. |
| All panels dark | SCK, MOSI, D/C and RES wiring; 3.3 V supply current. Try `reinit`. |
| The whole picture moves by one pixel every 2 minutes | Intentional: it protects the OLEDs (section 1). `set orbit_min 0` stops it until the next power-off. |
| Before calibration: `警告`, a large `加速` or `制動` and the ball at an edge while the board is still | Expected: the firmware does not know the sensor's mounting yet (section 6, steps 4 and 5). |
| Picture is noisy or jumps | Shorter SPI wires, series resistors, or a lower SPI speed (section 8). |
| Rear panels too dim | `ramp 60 255`; check that all panels face the same way. |
| Settings lost after flashing | Expected after a `_full.bin`: calibrate again and `save` (section 3). |
| `E (...) esp_core_dump_flash: ...` at every boot | Harmless leftover from earlier firmware; erase the board once (section 3). |
| Board resets in the car | Power supply too weak or noisy: use a proper buck converter and short, thick ground wires. |
