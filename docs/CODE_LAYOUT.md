# Code layout

This repository holds the firmware of the Gmeter unit: the ESP-IDF project in
`idf/`, written in C, which the supplier builds and flashes. Next to it are the
C tests of the firmware's core (`idf/test/`, run on a PC and never shipped),
the release and emulator tools, and the docs. Line counts are of the files as
committed.

```
gmeter/
├── README.md                 Project overview: build, flash, test, effects, conventions
├── LICENSE                   MIT licence
├── idf/                      The firmware: an ESP-IDF v5 project in C (the supplier package's source)
│   ├── main/                 The application: tasks, drivers, serial commands, settings, menuconfig options
│   ├── components/gmeter_core/   Hardware-independent core: g-force maths, the voxel frame, the ten effects
│   ├── configs/              sdkconfig overlays for the release variants (HUD edition, 128x64 panels)
│   └── test/                 C tests of the core, built and run on a PC (not shipped)
├── tools/                    Release build, emulator checks, glyph generator
└── docs/                     Supplier guides (standard and HUD), hardware, architecture, effects, licences
```

## Firmware project: `idf/`

| File | Lines | What it does |
| --- | ---: | --- |
| `CMakeLists.txt` | 16 | The ESP-IDF project (name `gmeter`, so the app is `gmeter.bin`); builds only `main` and the components it needs. |
| `sdkconfig.defaults`, `sdkconfig.defaults.esp32s3` | 32 / 5 | Project settings: 4 MB flash and the partition table, `-O2`, 240 MHz, 1 ms tick, quiet ESP-IDF logs, the main task on core 1 with 8 KB of stack; on the ESP32-S3 no second console on the native USB port. The console stays ESP-IDF's default, UART0 at 115200. |
| `configs/hud.defaults`, `configs/panel_128x64.defaults` | 3 / 3 | Overlays for the HUD edition and for generic SSD1309 128x64 panels. |
| `partitions.csv` | 5 | Flash layout: settings (NVS) at 0x9000, the 2 MB app at 0x10000. |
| `README.md` | 188 | Building, flashing, the six variants, menuconfig options, flash addresses, custom artwork, start-up output, calibration. It is the README of the supplier package's source folder. |
| `.gitignore` | 5 | Build folders and the generated `sdkconfig`. |

### Application: `idf/main/` (2228 lines)

| File | Lines | What it does |
| --- | ---: | --- |
| `app_main.c` | 720 | Starts everything. Three FreeRTOS tasks: IMU sampling (core 0, 200 Hz), rendering (core 1) and SPI display output (core 0), with a triple-buffered frame hand-off. Also the calibration captures, the demo and serial g sources, the HUD edition start-up and its `DEMO` / `SERIAL` / `NO SENSOR` tag, the BOOT button, housekeeping and the functions behind the serial commands. |
| `Kconfig.projbuild` | 115 | The **Gmeter** menu in `idf.py menuconfig`: edition, panel type (and so the panel height), SPI clock, upside-down panels, chip-select order, frame rate. |
| `board_config.h` | 59 | Pin map for the classic ESP32 and the ESP32-S3, and the checks that the geometry suits the driver (128 columns, at most 8 layers). Edit this to change wiring. |
| `display_ssd1309.h/.c` | 72 / 285 | Driver for eight SSD1309 (or SSD1306) panels on one SPI bus (ESP-IDF `spi_master`) with one chip select each: init sequence, frame transfer, contrast ramp, invert, soft and hard refresh, test pattern. |
| `imu_mpu6050.h/.c` | 58 / 192 | Register-level MPU-6050 driver on the ESP-IDF I2C master driver: ±4 g, 44 Hz low-pass, 200 Hz; frees a stuck I2C bus, counts errors, recovers a lost sensor, accepts MPU-6500-family and common clone chips. |
| `serial_cli.h/.c` | 67 / 405 | The serial command line (115200 baud): line editing and echo, then `help`, `status`, `mode`, `get`, `set`, `cal`, `test`, `save` and the rest; commands that touch the effects run under the engine lock. The HUD edition refuses `mode`, `cycle` and `anim`. |
| `console_io.h/.c` | 52 / 133 | Serial port input and output: UART0 by default, or the ESP32-S3's USB Serial/JTAG or USB-OTG CDC port when chosen in menuconfig; `print_t`, the byte sink the replies are written to. |
| `storage.h/.c` | 46 / 139 | Saves settings and calibration in NVS (namespace `gmeter`, one CRC-checked blob in the layout earlier firmware used) and checks the values it loads. |
| `CMakeLists.txt` | 27 | The `main` component: its sources, the driver components for the ESP-IDF version, the no-double-promotion flag, and the check for `user_animation.h`. |
| `user_animation.h` | (optional) | Not in the repository: custom artwork for the `anim` effect, a header that defines `kUserAnimation`, a `gm_anim_asset_t` (format in `docs/EFFECTS.md`). If present, it replaces the built-in demo animation. |

### Core: `idf/components/gmeter_core/` (3243 lines of C, 1501 of headers)

Plain C (C17) with no ESP-IDF dependency, so the same code also builds on a
PC for the tests. Headers in `include/`, sources in `src/` (the effects in
`src/effects/`). Every name starts with `gm_`. The component's
`CMakeLists.txt` (7 lines) registers both source
folders and turns double promotion into an error.

| File | Lines | What it does |
| --- | ---: | --- |
| `gmeter.h` | 23 | Includes the whole core; the application includes only this. |
| `gm_config.h` | 47 | Display geometry: 128 x 56 (or 64) pixels x 8 layers and the layer spacing. In the firmware the height comes from menuconfig; the tests pass it with `-D`. |
| `gm_math.h` | 214 | Single-precision vector and 3x3 matrix maths, a random number generator, the Butterworth low-pass filter and a damped spring. |
| `gm_gforce.h` | 73 | The car frame used everywhere (x right, y up, z forward) and the views of the measured g the effects use: dynamic, inertial, effective gravity, lateral/vertical/longitudinal. |
| `gm_estimator.h/.c` | 106 / 124 | Raw accelerometer samples to filtered g-force in the car frame: the mounting calibration (rest, braking, forward hint), the low-pass filter and the optional auto re-zero. |
| `gm_frame.h/.c` | 110 / 310 | The voxel frame (one bit per pixel per layer, in the panels' own memory layout) and drawing: dithered plot, lines, rectangles, circles, discs, spheres, 3D lines, the 3x5 font, a whole-frame pixel shift and the dithered crossfade. |
| `gm_effect.h` | 113 | The interface every effect implements: a struct with a vtable (name, update, render, and optionally reset, deactivate, burn-in offset) and its table of named parameters. |
| `gm_params.c` | 34 | The parameter tables declared in `gm_effect.h`: named, range-checked tunables, so every effect can be listed and tuned over serial (`get`, `set`). |
| `gm_engine.h/.c` | 68 / 118 | Runs the active effect on a sanitised g (finite, at most 16 g per axis), switches effects with a 0.4 s dithered crossfade, auto-cycles, draws the mode name, the optional g readout and the not-live-data tag, then applies the effect's burn-in offset. |
| `gm_scenario.h/.c` | 38 / 69 | Recorded drives played as demo input when there is no sensor: idle, launch, brake, slalom, corner, bump and the combined `drive` used by default. |
| `gm_gstate.h/.c` | 45 / 65 | The driving state from g-force (idle, accelerate, brake, left, right) with hysteresis and a minimum hold time, plus bump detection. |
| `gm_jp_text.h/.c` | 44 / 136 | Japanese text: glyph lookup, UTF-8 decoding, horizontal and vertical drawing with the 24 px and 14 px bitmap fonts. |
| `gm_jp_glyphs.c` | 251 | The glyph bitmaps, only the characters the effects draw (generated by `tools/make_glyphs.py` from Noto Serif JP and Noto Sans JP, SIL Open Font License). |

**Effects** (`include/gm_<name>.h`, `src/effects/gm_<name>.c`), in the order
the firmware lists them:

| Effect | Files | Lines | What it shows |
| --- | --- | ---: | --- |
| `ball` | `gm_gball.h/.c` | 44 / 162 | A ball thrown around the volume by g-force, with a reticle, 0.5 g / 1 g rings and a fading trail. |
| `slosh` | `gm_slosh.h/.c` | 57 / 282 | Liquid in a tank, with real waves (shallow-water equations on a grid across the panel and through the stack). |
| `particles` | `gm_particles.h/.c` | 46 / 218 | A few hundred grains that pile up, slide and splash (a small particle-in-cell fluid). |
| `smoke` | `gm_smoke.h/.c` | 73 / 305 | A smoke plume blown by g-force (a 3D stable-fluids solver, 16 x 8 cells per layer); its grids are on the heap only while it is shown. |
| `tunnel` | `gm_tunnel.h/.c` | 32 / 90 | One ring per layer, bending toward where a loose object would fly; dashes turn with lateral g, pulses run through the depth with longitudinal g. |
| `sprite` | `gm_sprite.h/.c` | 50 / 133 | A 2D sprite sheet moved through the volume and played or scrubbed by g; the firmware uses the built-in 16 x 16 demo sheet. |
| `anim` | `gm_anim.h/.c` | 148 / 349 | Authored artwork, optionally several slices deep, with a clip per driving state: `main/user_animation.h` when present, otherwise a built-in demo. |
| `hud` | `gm_hud.h/.c` | 88 / 319 | Neon Genesis Evangelion-style screen: frame, axis lines, a ball following the g line, kanji state titles; moves one pixel every 2 minutes against burn-in. The HUD edition shows only this effect. |
| `atfield` | `gm_atfield.h/.c` | 50 / 168 | Octagon shockwaves through the stack; sustained g and bumps fire ripples. |
| `sync` | `gm_sync.h/.c` | 32 / 110 | Sync-graph traces of the three g axes on three panels, with a 同期率 readout. |

### Tests: `idf/test/` (3365 lines of C)

A CMake project for the PC that builds the core with the firmware's geometry
and fixed floating-point flags, and checks it two ways: behaviour tests that
state properties with tolerances, and reference cases that fold every frame
and value the core produces into a digest and compare it with the recorded
one. Not shipped to the supplier (`tools/make_release.py` leaves it out).

| File | Lines | What it does |
| --- | ---: | --- |
| `CMakeLists.txt` | 48 | Builds the core and `gm_tests` for one panel height (`-DGM_PANEL_H=56` or `64`); `-DGM_SANITIZE=ON` adds AddressSanitizer and UndefinedBehaviorSanitizer. |
| `README.md` | 78 | How to run the tests, find the first step where a reference case changed, and when to re-record. |
| `main.c`, `test.h` | 311 / 141 | The small test framework: `TEST` and `CHECK`, the digest helpers, and the runner (name filter, `--record`, `--dump` with `--against`, `--list`). |
| `test_behaviour.c` | 1405 | 43 behaviour tests: maths and filters, calibration physics, what each effect does under braking, cornering and bumps, the engine's overlays and its sanitising of bad g, the Japanese text. |
| `test_properties.c` | 409 | 17 tests of the core's own rules: the effect contract (names, parameter tables, burn-in offsets), smoke's grid lifecycle, the built-in artwork, the HUD's orbit, the scenario and glyph registries. |
| `test_golden.c` | 1099 | The 110 reference cases: every effect on every demo drive through the engine, fuzzing through the effect vtables, non-finite g, mode switching in both editions, the estimator and its calibrations, the driving state, text and frame drawing, the parameter tables, the demo drives and the built-in artwork. |
| `golden_56.txt`, `golden_64.txt` | 112 / 112 | The recorded digests for 128x56 and 128x64 panels. |
| `fixtures/user_animation.h` | 624 | An animation header in the `main/user_animation.h` format (33 frames of 32 x 24 pixels, 3 slices, as Gmeter Studio exported it); the tests compile and play it, so the format stays fixed. |

## Tools: `tools/`

| File | Lines | What it does |
| --- | ---: | --- |
| `make_release.py` | 257 | Builds the three firmware variants of an edition with ESP-IDF from the committed source (`git archive` of HEAD) and assembles the supplier package: images, checksums, build info with memory use, the source without `idf/test`, the guide and docs (`--edition hud` for the HUD edition). |
| `qemu_boot_check.py` | 102 | Boots a firmware image in Espressif's QEMU emulator, types a script of serial commands and reports crashes, resets and missing `expect`ed output. |
| `qemu_std_check.txt`, `qemu_hud_check.txt` | 53 / 23 | Scripts for it: the standard edition answers every basic command and switches to every effect; the HUD edition boots into `hud`, stays there and refuses to switch. |
| `make_glyphs.py` | 192 | Regenerates `gm_jp_glyphs.c` from the Fontsource packages of the Noto fonts; the characters it renders are listed in the script. |

## Docs: `docs/` and the top level

"Shipped" means `tools/make_release.py` puts the file in the supplier package.

| File | Lines | What it is |
| --- | ---: | --- |
| `README.md` | 142 | Project overview: what is in the repository, build and flash, tests and the emulator, the effects, the g-force conventions, status. |
| `docs/SUPPLIER.md` | 257 | Guide for the hardware supplier, standard edition: which image to flash, wiring, bring-up checks, calibration, building from source, what has been tested. Shipped as `README_FIRST.md` (and in `docs/`). |
| `docs/SUPPLIER_HUD.md` | 333 | The same for the HUD edition, plus what the HUD shows and how to adjust it. Shipped as `README_FIRST.md` of the HUD package (and in its `docs/`). |
| `docs/HARDWARE.md` | 159 | Panel facts, wiring, frame transfer budget, power, the IMU, open questions for the builder. Shipped. |
| `docs/EFFECTS.md` | 318 | Physics and parameters of every effect, the animation header format, how to write a new effect. Shipped. |
| `docs/ARCHITECTURE.md` | 173 | Data flow, frames and conventions, the engine, firmware tasks, memory, timing, compile-time geometry, next steps. |
| `docs/THIRD_PARTY.md` | 115 | Licence of the Japanese glyphs (SIL Open Font License). Shipped. |
| `docs/CODE_LAYOUT.md` | (this file) | What every file does. |
| `LICENSE` | 21 | MIT licence. Shipped. |
| `.gitignore` | 9 | Build output, Python caches, `dist/`. |
