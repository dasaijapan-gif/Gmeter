# Gmeter: a volumetric G-force meter

Firmware for a car G-force meter whose display is **eight transparent OLED
panels stacked front to back**. Lit pixels on different panels add up in
the viewer's eye, so the stack behaves as a 128 x 56 x 8 voxel volume: left/right
and up/down on each panel, forward/back through the stack.

An ESP32 or ESP32-S3 reads an MPU-6050 accelerometer and turns the measured
g-force into things that move through that volume like real matter would: a
sloshing liquid, a thrown ball, drifting smoke, falling particles, and an
authored animation that reacts to the car.

```
                 viewer (driver)                         windshield
                      |                                       |
  layer      0        1        2        3        4        5        6        7
          +------+ +------+ +------+ +------+ +------+ +------+ +------+ +------+
          |      | |      | |      | |      | |      | |      | |      | |      |
          |      | |  .   | | ..   | | ...  | | ...  | | :..  | |  :.  | |  ::  |   braking: liquid
          |______| |______| |______| |______| |______| |______| |______| |______|   surges forward (to windshield)
```

## What is in the repository

| Path | What it is |
| --- | --- |
| `idf/` | **The firmware**: an ESP-IDF v5 project in C. `idf/main/` holds the application (SSD1309 panel-stack driver, MPU-6050 driver, dual-core render/transfer tasks, serial CLI, NVS storage, menuconfig options); `idf/components/gmeter_core/` the hardware-independent core (voxel frame, g-force conventions, IMU estimator, effects, engine, demo drives); `idf/configs/` the sdkconfig overlays for the release variants. |
| `idf/test/` | C tests of the core, built and run on a PC: behaviour tests and recorded reference outputs of every frame, at 128x56 and 128x64. Not shipped to the supplier. |
| `tools/` | `make_release.py` (build every board and assemble the supplier package), `qemu_boot_check.py` with `qemu_std_check.txt` / `qemu_hud_check.txt` (boot a firmware image in Espressif's emulator and type serial commands), `make_glyphs.py` (regenerate the Japanese glyph bitmaps). |
| `docs/` | [SUPPLIER.md](docs/SUPPLIER.md) flashing, wiring and bring-up for whoever builds the hardware ([SUPPLIER_HUD.md](docs/SUPPLIER_HUD.md) for the HUD edition), [CODE_LAYOUT.md](docs/CODE_LAYOUT.md) what every file does, [HARDWARE.md](docs/HARDWARE.md) wiring and panel facts, [ARCHITECTURE.md](docs/ARCHITECTURE.md) data flow and conventions, [EFFECTS.md](docs/EFFECTS.md) physics and tuning of each effect, [THIRD_PARTY.md](docs/THIRD_PARTY.md) font licence for the Japanese glyphs. |

## Build and flash

The firmware needs ESP-IDF v5.2 or newer (built and tested with v5.5.6).
From an ESP-IDF shell:

```sh
cd idf
idf.py set-target esp32            # or esp32s3
idf.py menuconfig                  # optional: Gmeter > edition, panel type, SPI clock, ...
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # 115200 baud, type `help`
```

`idf/README.md` has the details: the six release variants (ESP32 and ESP32-S3,
WiseChip 128x56 or generic SSD1309 128x64, standard or HUD edition, combined
from `sdkconfig.defaults` and the overlays in `idf/configs/`), the menuconfig
options, flash addresses, custom artwork and the calibration steps. The HUD
edition always shows the `hud` effect and nothing else, for a unit that should
run the NERV-style screen continuously. Pins live in `idf/main/board_config.h`.
On the ESP32-S3-DevKitC-1 the serial CLI is on the connector labelled UART.

`tools/make_release.py` builds the three variants of an edition from the
committed source and assembles the supplier package (`--edition hud` for the
HUD edition); the supplier guides are `docs/SUPPLIER.md` and
`docs/SUPPLIER_HUD.md`.

## Test

The core's tests build with CMake and a C compiler on a PC; run them at both
panel heights before committing a change to the core:

```sh
cmake -S idf/test -B /tmp/gm56 -G Ninja -DGM_PANEL_H=56
cmake --build /tmp/gm56 && /tmp/gm56/gm_tests
cmake -S idf/test -B /tmp/gm64 -G Ninja -DGM_PANEL_H=64
cmake --build /tmp/gm64 && /tmp/gm64/gm_tests
```

A change that alters anything the unit shows or prints changes a reference
digest; `idf/test/README.md` explains how to find the first differing step
and when re-recording is justified.

Without hardware, a firmware image also runs in Espressif's QEMU: no IMU or
panels are emulated, so the firmware plays its demo drive.
`tools/qemu_boot_check.py --script tools/qemu_std_check.txt` boots an image,
answers the basic commands and switches to every effect;
`tools/qemu_hud_check.txt` checks that the HUD builds boot straight into `hud`
and stay there. The docstring of `qemu_boot_check.py` describes the image.

## Effects

`ball`, `slosh`, `particles`, `smoke`, `tunnel`, `sprite`, `anim`, and three
inspired by Neon Genesis Evangelion: `hud` (NERV-style frame, axis lines, a
ball that follows the g-vector line, state kanji), `atfield` (octagon
shockwaves through the stack) and `sync` (sync-graph traces).
`docs/EFFECTS.md` describes each one and its parameters. Tuning is done on the
unit: `get` lists the current effect's parameters and `set <name> <value>`
changes one until the next power-off.

The `anim` effect plays artwork drawn in Gmeter Studio, a separate web page
with a 3D preview of the stack (not part of this repository). Save the header
it exports as `idf/main/user_animation.h` and rebuild; without it the built-in
demo animation plays.

Serial commands worth knowing: `mode slosh`, `get`, `set damping 0.3`,
`contrast all 128`, `ramp 60 255`, `test 3` (then `test off`), `status`, `cycle 20`,
`anim brake` (pin an animation clip on the bench; `anim auto` releases it),
`reinit` (hard-reset the panels) and `reinit 10` (soft refresh every 10 min).
The CLI echoes what you type, accepts backspace and any line ending.
The BOOT button steps through effects; holding it two seconds starts a rest
calibration. Without a sensor the firmware plays a recorded demo drive
(`src demo slalom` picks another; `list` shows them); `src serial` followed by
`g 0.3 0 -0.5` sets the g-force by hand.

## Conventions in one paragraph

Car frame: x right, y up, z forward (deeper into the stack). This frame is
left-handed, so right = forward x up; the calibration builds it that way from
the right-handed sensor. The accelerometer
measures *proper acceleration*: at rest (0, +1, 0) g. Everything simulated
feels the body force **minus** that vector, so braking throws the liquid, the
ball and the smoke toward the windshield (higher layer index) and a right turn
throws them left. `gm_gforce_t` in `idf/components/gmeter_core/include/gm_gforce.h`
defines it; `docs/ARCHITECTURE.md` has the derivation.

## Status

- Core: tested on a PC at both 128x56 and 128x64 geometry (`idf/test`).
  Verified behaviours include volume conservation and the equilibrium tilt of
  the liquid, correct direction of every effect under braking and cornering,
  calibration against physically specified sensor readings (a flat GY-521
  with +Y forward must read a right turn as +lateral), recovery from a tilted,
  yawed mount, and frame byte layout matching the SSD130x page format.
- Reviewed by an adversarial multi-agent pass; the confirmed findings (among
  them a mirrored lateral axis in the calibration, a render/CLI race on the
  engine and an IMU dropout that froze the display) are fixed, with
  regression tests where they are testable on a PC.
- Firmware: all six variants build without compiler warnings; the classic
  ESP32 build uses about 74 KB of its 181 KB of static RAM and 313 KB of the
  2 MB app partition. Both chips boot in Espressif's emulator and run every
  effect with no IMU attached. The panel init sequence and the sensor still
  need a real board. Register values, timing and init sequences come from the
  SSD1309, WiseChip and MPU-6050 datasheets (see `docs/HARDWARE.md` for
  sources and the open questions list).
- Next steps are listed at the end of `docs/ARCHITECTURE.md`.

MIT licensed, except the Japanese glyph bitmaps in
`idf/components/gmeter_core/src/gm_jp_glyphs.c`, which are derived from Noto
Serif JP and Noto Sans JP under the SIL Open Font License 1.1 (see
`docs/THIRD_PARTY.md`).
