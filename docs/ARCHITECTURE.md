# Architecture

## Data flow

```
 MPU-6050 ──200 Hz──> gm_estimator_t ────> gm_gforce_t ──60 fps──> gm_engine_t ──> gm_frame_t ──SPI──> 8 panels
 (sensor frame, g)    rotate to car frame  car frame               active effect   128x56x8 bits
                      2nd-order low-pass   proper accel            update+render
                      auto re-zero (opt.)
```

The core (`idf/components/gmeter_core`, every name starts with `gm_`) is
plain C with no ESP-IDF dependency; the application in `idf/main` feeds it
sensor samples and sends its frames to the panels, and `idf/test` runs the
same core on a PC. Without a sensor the firmware feeds the engine a recorded
demo drive instead (`gm_scenario_t`, keyframed dynamic g).

## Frames and conventions

**Car frame**: x right, y up, z forward. Forward is "deeper into the stack":
layer 0 is the panel nearest the driver, layer 7 nearest the windshield. Like
screen coordinates with depth, this frame is left-handed: right = forward x up
(up x forward points left). The accelerometer is right-handed, so the
sensor-to-car matrix has determinant -1.

**Voxel frame**: x = column 0..127 (left to right), y = row 0..H-1 (top to
bottom, screen convention), z = layer 0..7. `gm_world_to_voxel` maps a
car-frame position in pixel units (origin at the volume centre, layers
`GM_LAYER_PITCH_PX` apart) to voxel coordinates, so effects can do physics in
one consistent unit (pixels) and never think about panel indices.

**Proper acceleration**: an accelerometer measures the specific force
f = a_car - g_vec. At rest on level ground f = (0, +1, 0) g in the car frame.

| Quantity | Formula | At rest | Braking | Right turn |
| --- | --- | --- | --- | --- |
| `proper` | f | (0, 1, 0) | z < 0 | x > 0 |
| `gm_gforce_dynamic` | f - (0,1,0) | 0 | z < 0 | x > 0 |
| `gm_gforce_inertial` | -(f - (0,1,0)) | 0 | z > 0 (to windshield) | x < 0 (to the left) |
| `gm_gforce_effective_gravity` | -f | (0, -1, 0) | tilts toward +z | tilts toward -x |

Every effect uses `gm_gforce_inertial` (where a loose object goes) or
`gm_gforce_effective_gravity` (the body force on a fluid). A liquid surface
settles perpendicular to the effective gravity, so its slope is a_horizontal /
g_vertical. Note that on a hill the liquid tilts relative to the car; that is
physically right for liquid and arguably wrong for a g-ball, and separating
road grade from braking would need a gyro-based attitude estimate.

**Mounting calibration** (`gm_estimator_t`): average the rest vector to get
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
addressing mode: `GM_PAGES = GM_H / 8` rows of 128 bytes, each byte holding
eight vertically stacked pixels with bit 0 at the top. `gm_frame_layer(f, z)`
is therefore DMA-ready: the display driver sends a window command and streams
the bytes with no repacking.

Primitives: dithered `gm_frame_plot(f, x, y, z, intensity)` (ordered Bayer 8x8
with a per-layer offset so adjacent layers do not moire), lines, rectangles,
discs, 3D spheres rendered as per-layer discs, 3D lines, a 3x5 HUD font, and a
dithered `gm_frame_crossfade` used for mode transitions.

## Effects

An effect is a struct whose first member is `gm_effect_t`. Its vtable
(`gm_effect_vtbl_t`) holds `update` (physics; may substep) and `render`, and
optionally `reset`, `activate` / `deactivate` (heavy effects own their buffers
only while shown) and `burn_in_offset`. Every tunable is registered in the
effect's parameter table (`gm_params_add`), so the serial CLI (`get`, `set`)
and a future web page can tune it by name. `docs/EFFECTS.md` describes each
effect and its parameters.

The engine (`gm_engine_t`) owns the active effect, runs a 0.4 s dithered
crossfade on mode changes, optionally auto-cycles effects, and overlays the
mode name or a numeric readout on the front layer. In the single-effect (HUD)
edition, while the picture is not live sensor data, the firmware also has it
draw a small tag at the front layer's bottom right: `DEMO`, `SERIAL`, or
`NO SENSOR` when the IMU stops answering. Last of all it moves the whole
frame by the effect's `burn_in_offset` (the HUD's anti-burn-in orbit; zero
for the other effects).

The HUD edition (menuconfig: Gmeter > Edition, or `idf/configs/hud.defaults`)
boots into `hud` whatever was saved, turns auto-cycle off, and refuses
`mode`, `cycle` and `anim`; a short press of the BOOT button only prints a
note, and `save` leaves the effect and auto-cycle stored by the standard
edition alone.

## Firmware tasks

| Task | Core | Rate | Job |
| --- | --- | --- | --- |
| `imu` | 0 | 200 Hz | read sensor, estimator update, publish the g-force; calibration captures |
| `render` | 1 | 60 fps target | `gm_engine_step`, copy the frame into a free buffer |
| `display` | 0 | on frame ready | SPI transfer (~6 ms, polled), periodic re-init, test patterns |
| main task (`app_main`) | 1 | idle | serial CLI, mode button, housekeeping |

The tasks are in `idf/main/app_main.c`.

Frames are triple-buffered: the renderer writes to the buffer that is neither
displaying nor queued; if the display falls behind, the newest frame wins and
`status` counts the drop. The g vector, source and buffer indices are shared
under a spinlock. The engine and its effects are guarded by a FreeRTOS
mutex: `render_task` holds it while stepping, and the CLI and mode button hold
it while they change modes or parameters, so a mode switch can never race a
step (smoke allocates and frees its buffers on activate/deactivate). The
estimator has its own mutex between `imu_task` and the CLI. Floats are never
touched in an ISR.

Memory (128x56x8, classic ESP32): about 74 KB of static RAM, mostly the
engine's two frames at 7 KB each, slosh ~11.5 KB and particles ~23 KB; the
three transfer buffers and the display driver's scratch frames come from
DMA-capable heap at start-up. Smoke needs 57.6 KB of heap only while active
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

Measure on target with `status` (step and SPI microseconds).

## Compile-time geometry

| Macro | Constraint | Set by |
| --- | --- | --- |
| `GM_PANEL_W` | must be 128 in the firmware (the driver addresses columns 0..127) | `gm_config.h` (128) |
| `GM_PANEL_H` | multiple of 8; 56 for the transparent panels, 64 for generic modules | menuconfig (Gmeter > Panels); `-DGM_PANEL_H=` for `idf/test` |
| `GM_LAYERS` | 1..8 in the firmware (one chip-select pin per layer) | `gm_config.h` (8) |
| `GM_LAYER_PITCH_PX` | panel spacing in mm / 0.274 | `gm_config.h` (16) |
| `GM_SMOKE_NX`, `GM_SMOKE_NY` | NX divides the width, NY the height; the header default is 4x4-px cells | `idf/components/gmeter_core/CMakeLists.txt` (16 x 8) |
| Panel model, SPI clock, frame rate, flip mask, panel order | firmware only | menuconfig |

In the firmware `CONFIG_GMETER_PANEL_H` sets the panel height through
`gm_config.h`. `idf/test` compiles the core with the same geometry and smoke
grid. `idf/main/board_config.h` checks the width and layer limits. Tests pass
at 128x56 and 128x64.

## Next steps

1. Panel bring-up on real hardware (`test 0`..`test 7` to map chip selects;
   `ramp` to balance brightness through the stack). The firmware already
   builds with ESP-IDF and boots in Espressif's QEMU.
2. Measure real transmittance and panel spacing; set `GM_LAYER_PITCH_PX` and
   the default contrast ramp accordingly.
3. Queue the SPI transfers (interrupt-driven) instead of polling, to free
   core 0.
4. A Wi-Fi tuning page (the parameter tables already expose every tunable by
   name).
5. More effects: a wireframe box that shears with g, a "needle" through the
   depth. Authored animations already come from Gmeter Studio exports
   (`idf/main/user_animation.h`).
6. Optional gyro use (yaw rate) to separate hills from braking and to spin
   the smoke.
