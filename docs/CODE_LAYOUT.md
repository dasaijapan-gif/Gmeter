# Code layout

The ESP32 firmware is the ESP-IDF project in `idf/`, written in C: it is what
the supplier builds and flashes. The rest is the core's tests (run on a PC),
the release and emulator tools, and the docs.

```
gmeter/
├── README.md                 Project overview and quick start
├── LICENSE                   MIT licence
├── idf/                      ESP32 firmware: ESP-IDF v5 project in C (the supplier package's source)
│   ├── main/                 The application: tasks, drivers, serial commands, settings, menuconfig options
│   ├── components/gmeter_core/   Hardware-independent core in C: g-force maths and all effects
│   ├── configs/              sdkconfig overlays for the release variants (HUD edition, 128x64 panels)
│   └── test/                 PC tests of the core: behaviour tests and recorded reference outputs
├── tools/                    Helper scripts (release, QEMU checks, glyphs)
└── docs/                     Hardware, architecture, effects, supplier guides (standard and HUD), licences
```

## Firmware project: `idf/`

| File | Lines | What it does |
| --- | ---: | --- |
| `CMakeLists.txt` | 16 | The ESP-IDF project (name `gmeter`, so the app is `gmeter.bin`). |
| `sdkconfig.defaults`, `sdkconfig.defaults.esp32s3` | 32 / 5 | Project settings: 4 MB flash, partition table, 240 MHz, 1 ms tick, console UART0 at 115200, quiet ESP-IDF logs. |
| `configs/hud.defaults`, `configs/panel_128x64.defaults` | 3 / 3 | Overlays for the HUD edition and for generic SSD1309 128x64 panels. |
| `partitions.csv` | 5 | Flash layout: settings (NVS) at 0x9000, the app at 0x10000. |
| `README.md` | 184 | Building, flashing, the six variants, menuconfig options, calibration. |

### Application: `idf/main/` (2230 lines)

| File | Lines | What it does |
| --- | ---: | --- |
| `app_main.c` | 721 | Starts everything. Three FreeRTOS tasks: IMU sampling (core 0), rendering (core 1) and SPI display output (core 0), with a triple-buffered frame hand-off. Also calibration, demo/serial g sources, the HUD edition start-up and its `DEMO` / `SERIAL` / `NO SENSOR` tag, the BOOT button and the functions behind the serial commands. |
| `Kconfig.projbuild` | 115 | The **Gmeter** menu in `idf.py menuconfig`: edition, panel type and size, SPI clock, upside-down panels, chip-select order, frame rate. |
| `board_config.h` | 59 | Pin map for the classic ESP32 and the ESP32-S3. Edit this to change wiring. |
| `display_ssd1309.h/.c` | 72 / 285 | Driver for eight SSD1309 panels on one SPI bus (ESP-IDF `spi_master`) with separate chip selects: init sequence, frame transfer, contrast ramp, soft and hard refresh, test pattern. |
| `imu_mpu6050.h/.c` | 58 / 192 | Register-level MPU-6050 driver on the ESP-IDF I2C master driver: ±4 g, 44 Hz low-pass, 200 Hz; recovers a stuck I2C bus; accepts common clone chips. |
| `serial_cli.h/.c` | 67 / 405 | The serial command line (115200 baud): `help`, `status`, `mode`, `set`, `cal`, `test`, `save` and the rest. The HUD edition refuses `mode`, `cycle` and `anim`. |
| `console_io.h/.c` | 52 / 133 | Serial port input and output (UART console, or USB Serial/JTAG when chosen in menuconfig). |
| `storage.h/.c` | 46 / 139 | Saves settings and calibration in the ESP32's NVS flash. |
| `CMakeLists.txt` | 27 | The `main` component. |
| `user_animation.h` | (optional) | Artwork exported from Gmeter Studio. If present, it replaces the built-in demo animation. |

### Core: `idf/components/gmeter_core/` (3243 lines of C, 1507 of headers)

Plain C (C17) with no ESP-IDF dependency, so it also builds on a PC for the
tests. Headers in `include/`, sources in `src/`. Every name starts with `gm_`.

| File | What it does |
| --- | --- |
| `gmeter.h` | Includes the whole core. |
| `gm_config.h` | Display geometry: 128 x 56 (or 64) pixels x 8 layers, layer spacing (from menuconfig in the firmware). |
| `gm_math.h` | Small single-precision vector maths, a damped spring, a low-pass filter and a random number generator. |
| `gm_gforce.h` | The car frame used everywhere: x right, y up, z forward. |
| `gm_estimator.h/.c` | Turns raw accelerometer samples into filtered g-force in the car frame; rest and braking calibration. |
| `gm_frame.h/.c` | The 3D frame buffer (one bit per pixel per layer, in the panels' own memory layout) and drawing: lines, spheres, text, and a whole-frame pixel shift. |
| `gm_effect.h` | The interface every effect implements: a struct with a vtable (name, update, render, reset, deactivate, burn-in offset) and a table of named parameters. |
| `gm_engine.h/.c` | Runs the active effect on a sanitised g (finite, at most 16 g per axis), switches effects with a dithered crossfade, draws the optional g readout and the not-live-data tag, then applies the effect's burn-in offset. |
| `gm_params.c` | Named, range-checked parameters (declared in `gm_effect.h`), so every effect can be tuned over serial. |
| `gm_scenario.h/.c` | Recorded drives used as demo input: idle, launch, brake, slalom, corner, bump, and the combined `drive` used by default. |
| `gm_gstate.h/.c` | Classifies g-force into idle, accelerate, brake, left, right and bump. |
| `gm_jp_text.h/.c` | Draws Japanese text from built-in bitmap fonts. |
| `gm_jp_glyphs.c` | The Japanese glyph bitmaps (generated from Noto fonts by `tools/make_glyphs.py`, SIL Open Font License). |

**Effects** (`include/gm_<name>.h`, `src/effects/gm_<name>.c`):

| Effect | Files | What it shows |
| --- | --- | --- |
| `ball` | `gm_gball.*` | A ball thrown around the volume by g-force, with rings and a trail. |
| `slosh` | `gm_slosh.*` | Liquid in a tank, with real waves. |
| `particles` | `gm_particles.*` | A pile of grains that slides and splashes. |
| `smoke` | `gm_smoke.*` | A smoke plume blown by g-force (a small fluid solver). |
| `tunnel` | `gm_tunnel.*` | One ring per layer, bending with g. |
| `sprite` | `gm_sprite.*` | A built-in 2D sprite animation driven by g. |
| `anim` | `gm_anim.*` | Your own artwork from Gmeter Studio, with a clip per driving state. |
| `hud` | `gm_hud.*` | Neon Genesis Evangelion-style screen: frame, axis lines, a ball following the g line, kanji state titles; moves one pixel every 2 minutes against burn-in. The HUD edition shows only this effect. |
| `atfield` | `gm_atfield.*` | Octagon shockwaves through the stack. |
| `sync` | `gm_sync.*` | Sync-graph traces of each g axis. |

### Tests: `idf/test/`

A CMake project for the PC that builds the core with the firmware's geometry
and runs behaviour tests (`test_behaviour.c`, `test_properties.c`) and
reference-output cases (`test_golden.c`, digests in `golden_56.txt` and
`golden_64.txt`). `main.c` and `test.h` are its small test framework;
`fixtures/user_animation.h` is a real Gmeter Studio export. `README.md`
explains how to run, debug and re-record it. Not shipped to the supplier.

## Tools

| Path | What it does |
| --- | --- |
| `tools/make_glyphs.py` | Regenerates the Japanese glyph bitmaps (`gm_jp_glyphs.c`) from the Noto fonts. |
| `tools/qemu_boot_check.py` | Boots a firmware image in Espressif's QEMU emulator, types a script of serial commands and reports crashes, resets and missing `expect`ed output. |
| `tools/qemu_std_check.txt`, `tools/qemu_hud_check.txt` | Scripts for it: the standard edition answers every basic command and switches to every effect; the HUD edition boots into `hud`, stays there and refuses to switch. |
| `tools/make_release.py` | Builds the three firmware variants of an edition with ESP-IDF from the committed source and assembles the supplier package (`--edition hud` for the HUD edition). |
