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
| `lib/gmeter_core/` | Hardware-agnostic C++17 core: voxel frame, g-force conventions, IMU estimator, effects, engine, scripted scenarios. Shared by firmware and simulator. |
| `src/` | ESP32 firmware (PlatformIO + Arduino): SSD1309 panel-stack driver, MPU-6050 driver, dual-core render/transfer tasks, serial CLI, NVS storage. |
| `host/` | Native build: unit tests, the scenario-driven simulator that records frames, the browser viewer, Arduino stub headers for compile-checking the firmware on a PC. |
| `host/web/` | **Gmeter Studio**: a single-page tool with a 3D model of the panel stack, a pixel-art animation editor and a drivable g-force simulator, running the core compiled to WebAssembly. |
| `tools/` | `build_studio.py` (build Gmeter Studio), `make_viewer.py` (bundle a recording into the replay viewer), `img2sprite.py` (PNG/GIF to sprite header), `serial_gforce.py` (drive the display from a laptop). |
| `docs/` | [HARDWARE.md](docs/HARDWARE.md) wiring and panel facts, [ARCHITECTURE.md](docs/ARCHITECTURE.md) data flow and conventions, [EFFECTS.md](docs/EFFECTS.md) physics and tuning of each effect, [THIRD_PARTY.md](docs/THIRD_PARTY.md) font licence for the Japanese glyphs. |

## Gmeter Studio: design animations and watch them in 3D

```sh
pip install ziglang                       # once: a WebAssembly C++ compiler
python3 tools/build_studio.py -o gmeter_studio.html
```

Open `gmeter_studio.html` in a browser. It has three parts:

- **3D viewport.** The eight DFR0934 panels modelled to datasheet size (41.92 x
  27.08 mm glass, 0.274 mm pixels, the opaque strip and FPC). Orbit, pan and zoom.
  Panel gap, glass transmittance, glow and daylight are adjustable, so you can
  judge depth and brightness before cutting a spacer.
- **G-force simulator.** Drive a car with the keyboard or on-screen pedals
  (throttle, brake, steering, bumps, tyre grip and road roughness). Play the
  scripted launch/brake/slalom/corner/bump drives, or drag a g-vector by hand.
  Optionally route it through the simulated accelerometer and filter.
- **Animation studio.** Draw frames pixel by pixel (pencil, line, box, ellipse,
  fill, move, mirror, onion skin) or import PNG/GIF. Use up to eight depth
  slices for volumetric artwork. Assign frame ranges to the car states (idle,
  accelerate, brake, corner left/right, bump) and tune how the artwork moves
  with g. **Copy C++ header** produces `src/user_animation.hpp`; rebuild the
  firmware and the `anim` effect plays your artwork on the car.

Every effect in the viewport is the firmware's own C++ code (built with the
same 16 x 8 smoke grid as the ESP32), so tuning done in the Studio carries
over: the Effect tab prints the matching serial commands. The output is a
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
ninja firmware_hostcheck                         # compile-check src/ against Arduino stubs
./gmeter_tests                                   # 200+ checks on the core
./gmeter_sim --effect slosh --scenario corner --ascii    # terminal preview
./gmeter_sim --effect all --scenario drive --out frames.bin
python3 ../../tools/make_viewer.py frames.bin -o gmeter_viewer.html   # open in a browser
./gmeter_sim --bench                              # per-effect cost on the host
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

```sh
pio run -e esp32dev -t upload      # classic ESP32, 128x56 transparent panels
pio device monitor                 # 115200 baud, type `help`
```

Environments: `esp32dev` (default), `esp32-s3`, `esp32dev-128x64` (generic
SSD1309 modules). Pins live in `src/board_config.hpp`; panel geometry and
model are `-D` flags in `platformio.ini`. On the ESP32-S3-DevKitC-1 the serial
CLI is on the connector labelled UART; the native USB connector only flashes.

First power-up with an IMU connected:

1. Tell the firmware which sensor axis points at the windshield:
   `cal fwd <+x|-x|+y|-y|+z|-z>`. The default `+z` only fits a board standing
   upright with its Z axis forward. A GY-521 lying flat has +Z up, so use the
   in-plane axis that faces forward (for example `cal fwd +y`).
2. Park on level ground and type `cal rest` (hold still for 2 s). If the
   forward hint points up or down, the rest capture still sets "up" but
   reports that forward is unknown; step 3 then is required.
3. Recommended: drive straight, type `cal brake` and brake firmly for 1.5 s.
   This measures forward directly and removes any yaw in the mount. Run it
   again after any new `cal rest`.
4. `save`.

`save` stores the mode, contrast ramp, readout, invert, forward axis,
auto-zero, filter, auto-cycle, panel refresh interval and the calibration.
`set` changes to effect parameters are RAM-only and revert on reboot; to keep
tuned values, change the effect's defaults in its source (or export them from
the Studio's serial commands and apply after boot).

If the IMU drops out (loose connector, brownout), the display relaxes to 0 g
and the firmware retries every second; `status` shows the state. An IMU that
appears after boot is picked up automatically.

Without an IMU the firmware boots into a demo drive so the panels can be
checked on the bench. `tools/serial_gforce.py /dev/ttyUSB0 --scenario slalom`
pushes synthetic g over USB; `src serial` + `g 0.3 0 -0.5` does it by hand.

Serial commands worth knowing: `mode slosh`, `get`, `set damping 0.3`,
`contrast all 128`, `ramp 60 255`, `test 3`, `status`, `cycle 20`,
`anim brake` (pin an animation clip on the bench; `anim auto` releases it),
`reinit` (hard-reset the panels) and `reinit 10` (soft refresh every 10 min).
The CLI echoes what you type, accepts backspace and any line ending.
The BOOT button steps through effects; holding it two seconds starts a rest
calibration.

## Conventions in one paragraph

Car frame: x right, y up, z forward (deeper into the stack). This frame is
left-handed, so right = forward x up; the calibration builds it that way from
the right-handed sensor. The accelerometer
measures *proper acceleration*: at rest (0, +1, 0) g. Everything simulated
feels the body force **minus** that vector, so braking throws the liquid, the
ball and the smoke toward the windshield (higher layer index) and a right turn
throws them left. `gm::GForce` in `lib/gmeter_core/src/gmeter/gforce.hpp` is
the single source of truth; `docs/ARCHITECTURE.md` has the derivation.

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
- Firmware: compiles against host stubs; the ESP32 toolchain could not be run
  in the environment where this was written, so the first on-target build and
  the panel init sequence still need a real board. Register values, timing
  and init sequences come from the SSD1309, WiseChip and MPU-6050 datasheets
  (see `docs/HARDWARE.md` for sources and the open questions list).
- Next steps are listed at the end of `docs/ARCHITECTURE.md`.

MIT licensed, except the Japanese glyph bitmaps in
`lib/gmeter_core/src/gmeter/jp_glyphs.cpp`, which are derived from Noto Serif JP
and Noto Sans JP under the SIL Open Font License 1.1 (see
`docs/THIRD_PARTY.md`).
