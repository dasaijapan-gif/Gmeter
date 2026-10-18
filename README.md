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
          |  ::  | |  :.  | | :..  | | ...  | | ...  | | ..   | | .    | |      |   braking: liquid
          |______| |______| |______| |______| |______| |______| |______| |______|   surges forward
```

## What is in the repository

| Path | What it is |
| --- | --- |
| `lib/gmeter_core/` | Hardware-agnostic C++17 core: voxel frame, g-force conventions, IMU estimator, effects, engine, scripted scenarios. Shared by firmware and simulator. |
| `src/` | ESP32 firmware (PlatformIO + Arduino): SSD1309 panel-stack driver, MPU-6050 driver, dual-core render/transfer tasks, serial CLI, NVS storage. |
| `host/` | Native build: unit tests, the scenario-driven simulator that records frames, the browser viewer, Arduino stub headers for compile-checking the firmware on a PC. |
| `tools/` | `make_viewer.py` (bundle a recording into the viewer), `img2sprite.py` (PNG/GIF to sprite header), `serial_gforce.py` (drive the display from a laptop). |
| `docs/` | [HARDWARE.md](docs/HARDWARE.md) wiring and panel facts, [ARCHITECTURE.md](docs/ARCHITECTURE.md) data flow and conventions, [EFFECTS.md](docs/EFFECTS.md) physics and tuning of each effect. |

## Quick start without hardware

Everything runs on a PC first. The simulator feeds scripted drives through a
simulated, misaligned, noisy accelerometer, the real calibration and filtering
code, and the real effects, then records the voxel frames.

```sh
cd host && mkdir -p build && cd build
cmake -G Ninja -DGM_PANEL_H=56 .. && ninja
./gmeter_tests                                   # 130+ checks on the core
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
`sprite`. Any effect parameter can be overridden with `--param name=value`
(see `docs/EFFECTS.md`).

## Firmware

```sh
pio run -e esp32dev -t upload      # classic ESP32, 128x56 transparent panels
pio device monitor                 # 115200 baud, type `help`
```

Environments: `esp32dev` (default), `esp32-s3`, `esp32dev-128x64` (generic
SSD1309 modules). Pins live in `src/board_config.hpp`; panel geometry and
model are `-D` flags in `platformio.ini`.

First power-up with an IMU connected:

1. Park on level ground, type `cal rest` (holds still for 2 s).
2. Optional but recommended: drive straight, type `cal brake` and brake
   firmly for 1.5 s. This measures which way is forward instead of trusting
   the mounting hint (`cal fwd +z` style).
3. `save`.

Without an IMU the firmware boots into a demo drive so the panels can be
checked on the bench. `tools/serial_gforce.py /dev/ttyUSB0 --scenario slalom`
pushes synthetic g over USB; `src serial` + `g 0.3 0 -0.5` does it by hand.

Serial commands worth knowing: `mode slosh`, `get`, `set damping 0.3`,
`contrast all 128`, `ramp 60 255`, `test 3`, `status`, `cycle 20`.
The BOOT button steps through effects; holding it two seconds starts a rest
calibration.

## Conventions in one paragraph

Car frame: x right, y up, z forward (deeper into the stack). The accelerometer
measures *proper acceleration*: at rest (0, +1, 0) g. Everything simulated
feels the body force **minus** that vector, so braking throws the liquid, the
ball and the smoke toward the windshield (higher layer index) and a right turn
throws them left. `gm::GForce` in `lib/gmeter_core/src/gmeter/gforce.hpp` is
the single source of truth; `docs/ARCHITECTURE.md` has the derivation.

## Status

- Core library: unit-tested on the host at both 128x56 and 128x64 geometry.
  Verified behaviours include volume conservation and the equilibrium tilt of
  the liquid, correct direction of every effect under braking and cornering,
  calibration recovery from a tilted, yawed sensor mount, and frame byte layout
  matching the SSD130x page format.
- Firmware: compiles against host stubs; the ESP32 toolchain could not be run
  in the environment where this was written, so the first on-target build and
  the panel init sequence still need a real board. Register values, timing
  and init sequences come from the SSD1309, WiseChip and MPU-6050 datasheets
  (see `docs/HARDWARE.md` for sources and the open questions list).
- Next steps are listed at the end of `docs/ARCHITECTURE.md`.

MIT licensed.
