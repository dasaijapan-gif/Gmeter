# Gmeter: a volumetric G-force meter

Firmware and tooling for a car G-force meter whose display is **eight transparent
OLED panels stacked front to back**. Lit pixels on different panels add up in
the viewer's eye, so the stack behaves as a 128 x 56 x 8 voxel volume: left/right
and up/down on each panel, forward/back through the stack.

The software turns the measured g-force into things that move through that
volume like real matter would: a sloshing liquid, a thrown ball, drifting
smoke, falling particles, and any authored animation you want to make react to
the car.

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
| `idf/` | **The ESP32 firmware**: an ESP-IDF v5 project in C. `idf/main/` holds the application (SSD1309 panel-stack driver, MPU-6050 driver, dual-core render/transfer tasks, serial CLI, NVS storage, menuconfig options); `idf/components/gmeter_core/` the hardware-agnostic core (voxel frame, g-force conventions, IMU estimator, effects, engine, scripted scenarios); `idf/host_test/` the tests that compare that C core with the C++ core. |
| `lib/gmeter_core/` | The same core in C++17, used by Gmeter Studio and the PC simulator. |
| `host/` | Native build: C++ unit tests, the scenario-driven simulator that records frames, the browser viewer. |
| `host/web/` | **Gmeter Studio**: a single-page tool with a 3D model of the panel stack, a pixel-art animation editor and a drivable g-force simulator, running the core compiled to WebAssembly. |
| `tools/` | `build_studio.py` (build Gmeter Studio), `make_viewer.py` (bundle a recording into the replay viewer), `img2sprite.py` (PNG/GIF to sprite header), `make_glyphs.py` (Japanese glyph bitmaps, C and C++), `serial_gforce.py` (drive the display from a laptop), `qemu_boot_check.py` with `qemu_std_check.txt` / `qemu_hud_check.txt` (boot a firmware image in Espressif's emulator), `make_release.py` (build every board and assemble the supplier package). |
| `docs/` | [SUPPLIER.md](docs/SUPPLIER.md) flashing, wiring and bring-up for whoever builds the hardware ([SUPPLIER_HUD.md](docs/SUPPLIER_HUD.md) for the HUD edition), [CODE_LAYOUT.md](docs/CODE_LAYOUT.md) what every file does, [HARDWARE.md](docs/HARDWARE.md) wiring and panel facts, [ARCHITECTURE.md](docs/ARCHITECTURE.md) data flow and conventions, [EFFECTS.md](docs/EFFECTS.md) physics and tuning of each effect, [THIRD_PARTY.md](docs/THIRD_PARTY.md) font licence for the Japanese glyphs. |

## Gmeter Studio: design animations and watch them in 3D

```sh
pip install ziglang                       # once: a WebAssembly C++ compiler
python3 tools/build_studio.py -o gmeter_studio.html
```

Open `gmeter_studio.html` in a browser. It has three parts:

- **3D viewport.** The eight DFR0934 panels modelled to datasheet size (41.92 x
  27.08 mm glass, 0.274 mm pixels, the opaque strip and FPC). Orbit, pan and zoom
  with the mouse or touch; with the view focused, Shift+arrows orbit and +/- zoom.
  Panel gap, glass transmittance, glow and daylight are adjustable, so you can
  judge depth and brightness before cutting a spacer.
- **G-force simulator.** Drive a car with the keyboard (while the 3D view or the
  simulator has focus) or on-screen pedals
  (throttle, brake, steering, bumps, tyre grip and road roughness). Play the
  scripted launch/brake/slalom/corner/bump drives, or drag a g-vector by hand.
  Optionally route it through the simulated accelerometer and filter.
- **Animation studio.** Draw frames pixel by pixel (pencil, line, box, ellipse,
  fill, move, mirror, onion skin) or import PNG/GIF. Use up to eight depth
  slices for volumetric artwork. Assign frame ranges to the car states (idle,
  accelerate, brake, corner left/right, bump) and tune how the artwork moves
  with g. **Copy C header** produces `main/user_animation.h` for the firmware
  (`idf/main/` in the repository); rebuild the firmware and the `anim` effect
  plays your artwork on the car.

Every effect in the viewport is the C++ core (built with the same 16 x 8 smoke
grid as the ESP32), which the firmware's C core matches bit for bit, so tuning
done in the Studio carries over: the Effect tab prints the matching serial
commands. The output is a
standalone page; `--bare` leaves out the document skeleton for hosts that add
their own.

## Quick start without hardware

Everything runs on a PC first. The simulator feeds scripted drives through a
simulated, misaligned, noisy accelerometer, the real calibration (rest plus
braking capture, as on the car) and filtering code, and the real effects, then
records the voxel frames. `--no-brake-cal` shows what the mount's yaw error
does without the braking capture.

```sh
cd host && mkdir -p build && cd build
cmake -G Ninja -DGM_PANEL_H=56 .. && ninja
./gmeter_tests                                   # unit tests of the C++ core
./gmeter_sim --effect slosh --scenario corner --ascii    # terminal preview
./gmeter_sim --effect all --scenario drive --out frames.bin
python3 ../../tools/make_viewer.py frames.bin -o gmeter_viewer.html   # open in a browser
./gmeter_sim --bench                              # per-effect cost on the host
```

The firmware's C core has its own host tests, which compare it with the C++
core frame by frame:

```sh
cmake -S idf/host_test -B build_c56 -G Ninja -DGM_PANEL_H=56 -DGM_SMOKE_NX=16 -DGM_SMOKE_NY=8
cmake --build build_c56 && ./build_c56/gm_c_tests     # add -DGM_SANITIZE=ON for ASan/UBSan
```

The viewer replays the recording as the glass stack (drag to shift your
viewpoint), with top and side views so you can see how content spreads through
the depth, plus a classic g-meter dial.

Scenarios: `idle`, `launch`, `brake`, `slalom`, `corner`, `bump`, `drive`
(a 21 s mix). Effects: `ball`, `slosh`, `particles`, `smoke`, `tunnel`,
`sprite`, `anim`, and three inspired by Neon Genesis Evangelion: `hud` (NERV-style
frame, axis lines, a ball that follows the g-vector line, state kanji),
`atfield` (octagon shockwaves through the stack) and `sync` (sync-graph traces). Any effect parameter can be overridden with `--param name=value`
(see `docs/EFFECTS.md`).

## Firmware

The ESP32 firmware is an ESP-IDF v5 project written in C, in `idf/`
(ESP-IDF v5.2 or newer; built and tested with v5.5.6). From an ESP-IDF shell:

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

All six variants build without compiler warnings; the classic ESP32 build uses
about 81 KB of its 181 KB of static RAM and 326 KB of the 2 MB app partition.
Both chips boot in Espressif's QEMU and run every effect with no IMU attached
(`tools/qemu_boot_check.py --script tools/qemu_std_check.txt`), and the HUD
builds boot straight into `hud` and stay there (`tools/qemu_hud_check.txt`).
The C core in `idf/components/gmeter_core` is a statement-by-statement port of
the C++ core in `lib/gmeter_core`, which Gmeter Studio and the simulator use;
`idf/host_test` runs both on the same input and checks that every frame and
number is bit-identical (153 tests, about 19 million checks per panel size).
Panel and IMU behaviour still has to be confirmed on real hardware.

`tools/make_release.py` builds the three variants of an edition from the
committed source and assembles the supplier package (`--edition hud` for the
HUD edition); the supplier guides are `docs/SUPPLIER.md` and
`docs/SUPPLIER_HUD.md`.

Serial commands worth knowing: `mode slosh`, `get`, `set damping 0.3`,
`contrast all 128`, `ramp 60 255`, `test 3` (then `test off`), `status`, `cycle 20`,
`anim brake` (pin an animation clip on the bench; `anim auto` releases it),
`reinit` (hard-reset the panels) and `reinit 10` (soft refresh every 10 min).
The CLI echoes what you type, accepts backspace and any line ending.
The BOOT button steps through effects; holding it two seconds starts a rest
calibration. `tools/serial_gforce.py /dev/ttyUSB0 --scenario slalom` pushes
synthetic g over USB; `src serial` + `g 0.3 0 -0.5` does it by hand.

## Conventions in one paragraph

Car frame: x right, y up, z forward (deeper into the stack). This frame is
left-handed, so right = forward x up; the calibration builds it that way from
the right-handed sensor. The accelerometer
measures *proper acceleration*: at rest (0, +1, 0) g. Everything simulated
feels the body force **minus** that vector, so braking throws the liquid, the
ball and the smoke toward the windshield (higher layer index) and a right turn
throws them left. `gm_gforce_t` in `idf/components/gmeter_core/include/gm_gforce.h`
(and `gm::GForce` in the C++ core) define it; `docs/ARCHITECTURE.md` has the
derivation.

## Status

- Core library: unit-tested on the host at both 128x56 and 128x64 geometry.
  Verified behaviours include volume conservation and the equilibrium tilt of
  the liquid, correct direction of every effect under braking and cornering,
  calibration against physically specified sensor readings (a flat GY-521
  with +Y forward must read a right turn as +lateral), recovery from a tilted,
  yawed mount, and frame byte layout matching the SSD130x page format.
- Reviewed by an adversarial multi-agent pass; the confirmed findings (among
  them a mirrored lateral axis in the calibration, a render/CLI race on the
  engine and an IMU dropout that froze the display) are fixed, with
  regression tests where they are testable on a PC. A calibration saved by
  firmware before that fix is corrected automatically on boot.
- Firmware: ESP-IDF v5 in C; builds for every variant, runs in Espressif's
  emulator, and its core matches the C++ core bit for bit. The panel init
  sequence and the sensor still need a real board. Register values, timing
  and init sequences come from the SSD1309, WiseChip and MPU-6050 datasheets
  (see `docs/HARDWARE.md` for sources and the open questions list).
- Next steps are listed at the end of `docs/ARCHITECTURE.md`.

MIT licensed, except the Japanese glyph bitmaps in
`idf/components/gmeter_core/src/gm_jp_glyphs.c` and
`lib/gmeter_core/src/gmeter/jp_glyphs.cpp`, which are derived from Noto Serif JP
and Noto Sans JP under the SIL Open Font License 1.1 (see
`docs/THIRD_PARTY.md`).
