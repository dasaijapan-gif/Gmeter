# Architecture

## Data flow

```
 MPU-6050 ──200 Hz──> GForceEstimator ──> GForce ──60 fps──> Engine ──> VoxelFrame ──SPI──> 8 panels
 (sensor frame, g)     rotate to car frame   car frame        active effect     128x56x8 bits
                       2nd-order low-pass    proper accel     update()+render()
                       auto re-zero (opt.)
```

The same chain runs in the browser: `host/web/wasm_api.cpp` exposes the
engine, effects, scenarios, sensor model and animation loader as a small C API,
`tools/build_studio.py` compiles it to WebAssembly (Zig's clang with libc++
for wasm32-wasi, about 80 KB) and embeds it in `host/web/studio.html`, which
renders the frames on a Three.js model of the panels.

The same chain also runs on the PC: `host/sim_main.cpp` replaces the sensor with a
`Scenario` (keyframed dynamic g) passed through `simulateRawSensor()`, which
applies the inverse of a mounting rotation plus noise, so the estimator and
calibration code are exercised exactly as on the car.

## Frames and conventions

**Car frame**: x right, y up, z forward. Forward is "deeper into the stack":
layer 0 is the panel nearest the driver, layer 7 nearest the windshield. Like
screen coordinates with depth, this frame is left-handed: right = forward x up
(up x forward points left). The accelerometer is right-handed, so the
sensor-to-car matrix has determinant -1; `storage::load` converts calibrations
saved by firmware that got this wrong.

**Voxel frame**: x = column 0..127 (left to right), y = row 0..H-1 (top to
bottom, screen convention), z = layer 0..7. `VoxelFrame::worldToVoxel` maps a
car-frame position in pixel units (origin at the volume centre, layers
`kLayerPitchPx` apart) to voxel coordinates, so effects can do physics in one
consistent unit (pixels) and never think about panel indices.

**Proper acceleration**: an accelerometer measures the specific force
f = a_car - g_vec. At rest on level ground f = (0, +1, 0) g in the car frame.

| Quantity | Formula | At rest | Braking | Right turn |
| --- | --- | --- | --- | --- |
| `proper` | f | (0, 1, 0) | z < 0 | x > 0 |
| `dynamic()` | f - (0,1,0) | 0 | z < 0 | x > 0 |
| `inertial()` | -(f - (0,1,0)) | 0 | z > 0 (to windshield) | x < 0 (to the left) |
| `effectiveGravity()` | -f | (0, -1, 0) | tilts toward +z | tilts toward -x |

Every effect uses `inertial()` (where a loose object goes) or
`effectiveGravity()` (the body force on a fluid). A liquid surface settles
perpendicular to `effectiveGravity()`, so its slope is a_horizontal / g_vertical.
Note that on a hill the liquid tilts relative to the car; that is physically
right for liquid and arguably wrong for a g-ball, and separating road grade
from braking would need a gyro-based attitude estimate.

**Mounting calibration** (`GForceEstimator`): average the rest vector to get
"up" in the sensor frame; take a nominal forward axis (sensor +z by default,
`cal fwd`), remove its component along "up" (Gram-Schmidt), and build the
matrix with rows right = forward x up, up, forward. A hint within 30 degrees
of vertical is rejected: the rest capture then fixes only "up" and reports
that forward is unknown. A braking capture replaces the hint with minus the
horizontal part of the raw braking vector, which fixes any yaw error of the
mount and works after a reboot (it needs no stored rest vector). After a
reboot the calibrated forward axis becomes the hint, so a later `cal rest`
keeps a braking-refined forward.

## Voxel frame layout

Each layer is stored exactly as SSD1306/SSD1309 GDDRAM in horizontal
addressing mode: `kPages = H/8` rows of 128 bytes, each byte holding eight
vertically stacked pixels with bit 0 at the top. `VoxelFrame::layer(z)` is
therefore DMA-ready: the display driver sends a window command and streams the
bytes with no repacking. The host viewer decodes the same layout.

Primitives: dithered `plot(x, y, z, intensity)` (ordered Bayer 8x8 with a
per-layer offset so adjacent layers do not moire), lines, rectangles, discs,
3D spheres rendered as per-layer discs, 3D lines, a 3x5 HUD font, and a
dithered `crossfade` used for mode transitions.

## Effects

`Effect` has `update(ctx)` (physics; may substep) and `render(frame)`;
`activate()/deactivate()` let heavy effects own their buffers only while shown.
Every tunable is registered in a `ParamTable` so the serial CLI (`get`, `set`),
the simulator (`--param`) and a future web page can tune it by name.
`docs/EFFECTS.md` describes each effect and its parameters.

The `Engine` owns the active effect, runs a 0.4 s dithered crossfade on mode
changes, optionally auto-cycles effects, and overlays the mode name or a
numeric readout on the front layer. While the picture is not live sensor data,
the firmware also has it draw a small tag at the front layer's bottom right:
`DEMO`, `SERIAL`, or `NO SENSOR` when the IMU stops answering.

A single-effect build (`-DGM_FIXED_EFFECT=hud`, the `*-hud` environments)
boots into that effect whatever was saved, turns auto-cycle off, and refuses
`mode`, `cycle` and `anim`; a short press of the BOOT button only prints a
note.

## Firmware tasks

| Task | Core | Rate | Job |
| --- | --- | --- | --- |
| `imuTask` | 0 | 200 Hz | read sensor, estimator update, publish `GForce`; calibration captures |
| `renderTask` | 1 | 60 fps target | `engine.step()`, copy frame into a free buffer |
| `displayTask` | 0 | on frame ready | SPI transfer (~6 ms blocking), periodic re-init, test patterns |
| `loop()` | 1 | idle | serial CLI, mode button, housekeeping |

Frames are triple-buffered: the renderer writes to the buffer that is neither
displaying nor queued; if the display falls behind, the newest frame wins and
`status` counts the drop. The g vector, source and buffer indices are shared
under a spinlock (`gMux`). The Engine and its effects are guarded by a FreeRTOS
mutex: `renderTask` holds it while stepping, and the CLI and mode button hold
it while they change modes or parameters, so a mode switch can never race a
step (smoke allocates and frees its buffers on activate/deactivate). The
estimator has its own mutex between `imuTask` and the CLI. Floats are never
touched in an ISR.

Memory (128x56x8): about 96 KB of static RAM. Three engine frames, three
transfer buffers and two display-driver scratch frames (clear and test
pattern) at 7 KB each; slosh ~11.5 KB and particles ~23 KB as static objects;
the other effects ~4 KB together. Smoke needs 57.6 KB of heap only while active
(16x8x8 grid on classic ESP32). On an S3, 32x8x8 (~109 KB) is the next grid
that tiles a 128x56 panel; it doubles the solver cost, so measure the step time
first. At 128x64 the frames are 8 KB each and particles ~25 KB.

## Timing budget (classic ESP32, estimates)

| Stage | Cost | Note |
| --- | --- | --- |
| SPI transfer, 8 panels | 5.8 ms | blocking, core 0 |
| ball / tunnel / sprite | < 1 ms | |
| slosh (64x8 cells, 1-4 substeps) | ~1-3 ms | render dominates (dithered body) |
| particles (300, 2 substeps) | ~1-3 ms | |
| smoke 16x8x8, 6 iterations | ~6-15 ms per step | stepped at `rate` (default 30 Hz, minimum 20), rendered every frame |

Host numbers from `gmeter_sim --bench` are roughly 20-40x faster than the
ESP32; measure on target with `status` (step and SPI microseconds).

## Compile-time geometry

| Macro | Constraint | Host CMake |
| --- | --- | --- |
| `GM_PANEL_W` | must be 128 in the firmware (the driver addresses columns 0..127) | `-DGM_PANEL_W=` |
| `GM_PANEL_H` | multiple of 8; 56 for the transparent panels, 64 for generic modules | `-DGM_PANEL_H=` |
| `GM_LAYERS` | 1..8 in the firmware (one chip-select pin per layer) | `-DGM_LAYERS=` |
| `GM_LAYER_PITCH_PX` | panel spacing in mm / 0.274 | `-DGM_LAYER_PITCH_PX=` |
| `GM_SMOKE_NX`, `GM_SMOKE_NY` | NX divides the width, NY the height; default 4x4-px cells, ESP32 builds use 16 x 8 | `-DGM_SMOKE_NX=` / `-DGM_SMOKE_NY=` |
| `GM_PANEL_MODEL`, `GM_SPI_HZ`, `GM_TARGET_FPS`, `GM_PANEL_FLIP_MASK`, `GM_PANELS_REVERSED` | firmware only | n/a |

Set the firmware values in `platformio.ini`. `board_config.hpp` static-asserts
the width and layer limits. Tests pass at 128x56 and 128x64.

## Next steps

1. Panel bring-up on real hardware (`test 0`..`test 7` to map chip selects;
   `ramp` to balance brightness through the stack). The firmware already
   builds with the real ESP32 toolchain and boots in Espressif's QEMU.
2. Measure real transmittance and panel spacing; set `GM_LAYER_PITCH_PX` and
   the default contrast ramp accordingly.
3. Move the SPI transfer to the ESP-IDF DMA queue to free core 0.
4. A Wi-Fi tuning page (the `ParamTable` already exposes everything by name).
5. More effects: a wireframe box that shears with g, a "needle" through the
   depth. Authored animations already go through Gmeter Studio.
6. Optional gyro use (yaw rate) to separate hills from braking and to spin
   the smoke.
