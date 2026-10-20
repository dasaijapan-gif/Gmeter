# Code layout

The repository root is a PlatformIO project. Only `platformio.ini`, `src/` and
`lib/` go into the ESP32 firmware; everything else runs on a PC.

```
gmeter/
├── README.md                 Project overview and quick start
├── LICENSE                   MIT licence
├── platformio.ini            Firmware build settings: boards, HUD editions, pinned platform, panel size, SPI speed
├── src/                      ESP32 firmware (Arduino framework, FreeRTOS tasks)
├── lib/gmeter_core/          Hardware-independent core: g-force maths and all effects
├── host/                     PC builds: simulator, unit tests, browser Studio (WebAssembly)
├── tests/                    Unit tests for the core
├── tools/                    Helper scripts (Studio build, glyphs, sprites, serial g feed, QEMU check, release)
└── docs/                     Hardware, architecture, effects, supplier guides (standard and HUD), licences
```

## Firmware: `src/`

| File | Lines | What it does |
| --- | ---: | --- |
| `main.cpp` | 694 | Starts everything. Three FreeRTOS tasks: IMU sampling (core 0), rendering (core 1) and SPI display output (core 0), with a triple-buffered frame hand-off. Also calibration, demo/serial g sources, the single-effect (HUD edition) start-up and its `DEMO` / `SERIAL` / `NO SENSOR` tag, the BOOT button and the `App` object behind the serial commands. |
| `board_config.hpp` | 67 | Pin map for the classic ESP32 and the ESP32-S3, panel model, SPI speed. Edit this to change wiring. |
| `display_ssd1309.hpp/.cpp` | 61 / 245 | Driver for eight SSD1309 panels on one SPI bus with separate chip selects: init sequence, frame transfer, contrast ramp, soft and hard refresh, test pattern. |
| `imu_mpu6050.hpp/.cpp` | 51 / 125 | Register-level MPU-6050 driver: ±4 g, 44 Hz low-pass, 200 Hz; recovers a stuck I2C bus; accepts common clone chips. |
| `serial_cli.hpp/.cpp` | 73 / 313 | The serial command line (115200 baud): `help`, `status`, `mode`, `set`, `cal`, `test`, `save` and the rest. In a single-effect build it refuses `mode`, `cycle` and `anim`. |
| `storage.hpp/.cpp` | 32 / 127 | Saves settings and calibration in the ESP32's NVS flash. |
| `user_animation.hpp` | (optional) | Artwork exported from Gmeter Studio. If present, it replaces the built-in demo animation. |

## Build environments: `platformio.ini`

| Environment | Board | Panels | Shows |
| --- | --- | --- | --- |
| `esp32dev` | classic ESP32 | 128x56 transparent (DFR0934) | all ten effects |
| `esp32-s3` | ESP32-S3, 4 MB flash or more | 128x56 transparent (DFR0934) | all ten effects |
| `esp32dev-128x64` | classic ESP32 | 128x64 SSD1309 modules | all ten effects |
| `esp32dev-hud`, `esp32-s3-hud`, `esp32dev-128x64-hud` | as above | as above | only `hud` (`-DGM_FIXED_EFFECT=hud`) |

## Core library: `lib/gmeter_core/src/gmeter/`

Plain C++17 with no Arduino dependency, so the same code runs on the ESP32, in
the PC simulator and in the browser Studio. `lib/gmeter_core/library.json` is its PlatformIO
library manifest.

| File | What it does |
| --- | --- |
| `gmeter.hpp` | Includes the whole core. |
| `config.hpp` | Display geometry: 128 x 56 (or 64) pixels x 8 layers, layer spacing. |
| `math.hpp` | Small single-precision vector maths, a damped spring and a low-pass filter. |
| `gforce.hpp` | The car frame used everywhere: x right, y up, z forward. |
| `gforce_estimator.hpp/.cpp` | Turns raw accelerometer samples into filtered g-force in the car frame; rest and braking calibration. |
| `voxel_frame.hpp/.cpp` | The 3D frame buffer (one bit per pixel per layer, in the panels' own memory layout) and drawing: lines, spheres, text, and a whole-frame pixel shift. |
| `effect.hpp` | The interface every effect implements (including an optional whole-picture burn-in offset). |
| `engine.hpp/.cpp` | Runs the active effect, switches effects with a dithered crossfade, draws the optional g readout and the not-live-data tag, then applies the effect's burn-in offset. |
| `params.hpp` | Named, range-checked parameters, so every effect can be tuned over serial. |
| `scenario.hpp/.cpp` | Recorded drives used as demo input: idle, launch, brake, slalom, corner, bump, and the combined `drive` used by default. |
| `gstate.hpp` | Classifies g-force into idle, accelerate, brake, left, right and bump. |
| `jp_text.hpp/.cpp` | Draws Japanese text from built-in bitmap fonts. |
| `jp_glyphs.cpp` | The Japanese glyph bitmaps (generated from Noto fonts, SIL Open Font License). |

**Effects** (`effects/`):

| Effect | Files | What it shows |
| --- | --- | --- |
| `ball` | `gball.*` | A ball thrown around the volume by g-force, with rings and a trail. |
| `slosh` | `slosh.*` | Liquid in a tank, with real waves. |
| `particles` | `particles.*` | A pile of grains that slides and splashes. |
| `smoke` | `smoke.*` | A smoke plume blown by g-force (a small fluid solver). |
| `tunnel` | `tunnel.*` | One ring per layer, bending with g. |
| `sprite` | `sprite.*` | A built-in 2D sprite animation driven by g. |
| `anim` | `anim.*` | Your own artwork from Gmeter Studio, with a clip per driving state. |
| `hud` | `hud.*` | Neon Genesis Evangelion-style screen: frame, axis lines, a ball following the g line, kanji state titles; moves one pixel every 2 minutes against burn-in. The `*-hud` builds show only this effect. |
| `atfield` | `atfield.*` | Octagon shockwaves through the stack. |
| `sync` | `sync.*` | Sync-graph traces of each g axis. |

## PC side

| Path | What it does |
| --- | --- |
| `host/CMakeLists.txt` | Builds the core, the unit tests, the simulator and the firmware pin test; `ninja firmware_hostcheck` also compile-checks all of `src/` against the Arduino stand-ins. |
| `host/sim_main.cpp` | Simulator: feeds recorded drives through a simulated sensor and the real effects; prints or records frames. |
| `host/arduino_stubs/` | Minimal Arduino and ESP32 headers so `src/` can be compile-checked on a PC. They mirror the real core's macros, and `SPI.h` mirrors how the core claims pins in `SPI.begin()`. |
| `host/viewer/index.html` | Replay viewer for recorded frames. |
| `host/web/studio.html`, `host/web/wasm_api.cpp` | Gmeter Studio: the core compiled to WebAssembly, a 3D view of the stack, a drive simulator and an animation editor. |
| `tests/test_core.cpp` | Unit tests for the core (381 checks). |
| `tests/test_firmware_pins.cpp` | Runs the panel driver's start-up against the stubs and checks every panel line ends up an output, no GPIO is used twice and no reserved pin is used (classic ESP32 and ESP32-S3 maps). |
| `tests/fixtures/studio_export.hpp` | A real Gmeter Studio export that the tests compile and play. |
| `tools/build_studio.py` | Compiles the core to WebAssembly and bundles Gmeter Studio. |
| `tools/make_viewer.py` | Wraps a recording into a standalone viewer page. |
| `tools/img2sprite.py` | Converts images into 1-bit sprite data. |
| `tools/make_glyphs.py` | Regenerates the Japanese glyph bitmaps. |
| `tools/serial_gforce.py` | Sends recorded g-force to the ESP32 over USB for bench tests. |
| `tools/qemu_boot_check.py` | Boots a firmware image in Espressif's QEMU emulator, types a script of serial commands and reports crashes, resets and missing `expect`ed output. |
| `tools/qemu_hud_check.txt` | Script for it that checks a HUD-edition image boots into `hud`, stays there and refuses to switch. |
| `tools/make_release.py` | Builds the firmware for every board and assembles the supplier package (`--envs esp32dev-hud esp32-s3-hud esp32dev-128x64-hud --out dist/gmeter_hud` for the HUD edition; it picks the matching guide). |
