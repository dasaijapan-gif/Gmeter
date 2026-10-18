# Effects

All effects live in `lib/gmeter_core/src/gmeter/effects/`. Each one reads the
filtered `GForce` from the `EffectContext`, advances its own state and draws
into a cleared `VoxelFrame`. Units are pixels and seconds; 1 g of acceleration
is turned into pixels per second squared through a "virtual tank width"
(`tank_m`), so a 35 mm display can slosh like a one-metre tank instead of
buzzing at 5 Hz.

Tune at runtime with `set <param> <value>` over serial, `--param` in the
simulator, or the sliders in Gmeter Studio. `get` lists the current effect's
parameters with ranges. Values set over serial are RAM-only; `save` does not
store them, so make lasting changes in the effect's defaults.

## ball (`GBallEffect`)

The classic g-meter dot, in 3D. A critically damped spring (exact closed-form
update, stable at any frame rate) follows the inertial vector: lateral g moves
the ball left/right, longitudinal g moves it through the layers (braking
sends it toward the windshield), vertical g moves it up/down. The ball is an
ellipsoid drawn as a disc on each layer it intersects, with a fading 3D trail.
Layer 0 carries the HUD: reticle, dithered 0.5 g and 1 g rings, horizontal g
readout, peak hold and a longitudinal bar along the top edge.

| Param | Default | Meaning |
| --- | --- | --- |
| `scale_g` / `vscale_g` / `dscale_g` | 1.0 / 0.6 / 1.0 | g at full lateral / vertical / depth travel |
| `hz`, `zeta` | 1.8, 1.0 | spring natural frequency and damping ratio |
| `radius`, `depth_r` | 5 px, 1.3 layers (min 0.6) | ellipsoid size |
| `trail_s`, `trail_i` | 1.2 s, 0.55 | trail persistence (up to 4 s) and brightness |
| `inertia` | 1 | 1: moves like a loose object; 0: like an acceleration arrow |
| `hud`, `rings` | 1, 1 | front-layer overlays |

## slosh (`SloshEffect`)

A liquid height field on the horizontal plane (64 cells across, one per layer
in depth) integrated with the nonlinear shallow-water equations in flux form on
a staggered grid. The body force is the car's effective gravity, so the free
surface settles at slope a_horizontal / g_vertical: in a right turn it piles up
on the left; under braking it climbs toward the windshield. Waves travel at
sqrt(g * depth); the substep count follows the CFL condition each frame
(Courant 0.7, using the wave speed plus the flow speed on both axes). An outflow limiter keeps heights non-negative with exact volume
conservation; a conservative x-diffusion removes grid noise. The pressure term
sees a 1 Hz low-passed vertical g, while raw vertical jerk drives a whole-body
"bob" spring and a random splash so potholes look like potholes.

| Param | Default | Meaning |
| --- | --- | --- |
| `fill` | 0.42 | rest depth as a fraction of panel height |
| `tank_m` | 1.0 m | virtual tank width; bigger = slower slosh |
| `slope_gain` | 0.5 | tilt per g (1.0 is physically exact but 1 g dries one side) |
| `damping` | 0.6 /s | velocity damping |
| `viscosity` | 8 px^2/s | surface diffusion |
| `ripple` | 0 | idle ripple injection |
| `bob`, `bob_hz` | 10 px/g, 3 Hz | whole-body vertical bounce |
| `splash` | 60 | random velocity per unit vertical jerk |
| `style` | 1 | 0 surface line only, 1 surface + dithered body, 2 solid |
| `body_i`, `surface_px` | 0.12, 1 | body dither density, surface thickness |
| `substeps` | 12 | cost cap on substeps per frame; if the waves need more, that frame runs in slow motion rather than going unstable |

## particles (`ParticleEffect`)

Up to 400 particles under effective gravity, with wall bounces and drag. A
coarse density grid (8 x 8 px cells, one per layer) is splatted trilinearly;
cells denser than `rho0` build pressure whose gradient pushes particles apart,
so the pile keeps volume and slumps like sand or a thick liquid instead of
collapsing onto one row. Speed is clamped to 0.8 cell per substep so nothing
tunnels through a wall.

| Param | Default | Meaning |
| --- | --- | --- |
| `count` | 300 | active particles |
| `tank_m` | 1.0 m | gravity scale, as for slosh |
| `drag`, `bounce` | 0.6 /s, 0.25 | air drag, wall restitution |
| `pressure`, `rho0` | 12000, 0.5 particles/cell | repulsion stiffness (for a 1 m tank; scaled by 1/`tank_m`) and rest density |
| `jitter` | 15 | random agitation keeping the pile alive |
| `dot`, `substeps` | 1 px, 2 | particle size, physics substeps |

## smoke (`SmokeEffect`)

A low-resolution 3D "stable fluids" solver (semi-Lagrangian advection,
Gauss-Seidel pressure projection). An emitter at the centre of the volume
releases density that rises by buoyancy; the inertial force is applied
weighted by density, so the plume behaves like a heavy gas that is thrown
around while clear air stays put (a uniform body force in a closed box would
be cancelled by the pressure solve). The solver runs at `rate` Hz independent
of the frame rate; the dithered rasterisation hides the lower physics rate.
Grid size is compile-time (`GM_SMOKE_NX/NY`, one cell per layer in depth):
16 x 8 on the ESP32 builds and in Gmeter Studio; the plain host build defaults
to 4x4-pixel cells (32 x 14 at 128x56). The wind, buoyancy and emitter radius
are in cell units, so tune on the device's grid. Buffers are allocated only while
the effect is active.

| Param | Default | Meaning |
| --- | --- | --- |
| `wind` | 30 | cells/s^2 per g, applied to dense cells |
| `buoyancy` | 3 | rise per unit density |
| `emit`, `emit_r`, `emit_d` | 4 /s, 1.5 cells (min 0.75), 1 layer | emitter rate and size |
| `dissipate` | 1.2 /s | density decay |
| `iters` | 6 | pressure solver iterations |
| `rate` | 30 Hz (20-120) | physics steps per second; each step advances by the real time since the last one |
| `gain`, `swirl` | 1.5, 2 | brightness, emitter turbulence |

## tunnel (`TunnelEffect`)

One dashed ring per layer, shrinking toward the back. Ring centres are
displaced in proportion to depth by the inertial vector, so the whole tunnel
bends toward where a loose object would fly. Dashes rotate with lateral g;
brightness pulses travel through the depth with longitudinal g (braking sends
them toward the windshield). Cheap, and a good test of depth perception.

| Param | Default | Meaning |
| --- | --- | --- |
| `scale_g` | 1.0 | g for a full bend |
| `r_front`, `r_back` | 26, 5 px (front max H/2 - 1) | ring radii |
| `segments`, `spin`, `spin_g` | 10, 0.4 rad/s, 4 rad/s/g | dashes and rotation |
| `pulse`, `pulse_g` | 0.3 /s, 2.5 /s/g | depth pulses |
| `hz` | 2.5 | smoothing |

## sprite (`SpriteEffect`): make your own animation react to g

Takes any 1-bit sprite sheet and lets the g-force drive it:

- **position** in the volume, including which layer it sits on (`px_g_x`,
  `px_g_y`, `px_g_z` pixels per g, smoothed by a spring at `hz`);
- **which frame** is shown: g on one axis scrubs the frame index
  (`scrub_axis` 0 lateral / 1 vertical / 2 longitudinal, `frames_g` frames per g);
- **playback speed**: `idle_fps` plus `fps_g` per g of horizontal magnitude;
- **depth echo**: ghost copies on neighbouring layers (`echo`) so a flat
  sprite reads as a slab.

| Param | Default | Meaning |
| --- | --- | --- |
| `px_g_x`, `px_g_y`, `px_g_z` | 60, 24, 60 px/g | travel per g |
| `hz` | 2 | spring smoothing |
| `scrub_axis`, `frames_g` | 2, 8 | -1 none, 0 lateral, 1 vertical, 2 longitudinal; frames per g (negative reverses) |
| `idle_fps`, `fps_g` | 0, 12 | base playback rate; extra fps per g of horizontal magnitude |
| `echo` | 1 layer | ghost copies on neighbouring layers (0-4) |
| `inertia` | 1 | 1: thrown like a loose object; 0: follows the acceleration |

Export artwork with `tools/img2sprite.py anim.gif -o src/my_sprite.hpp
--name kMySprite --width 24` and construct `SpriteEffect(kMySprite, mapping)`.
A procedurally generated 16 x 16, 12-frame demo sprite is built in. For
state-driven, multi-slice artwork use the `anim` effect and Gmeter Studio.

## anim (`AnimationEffect`): authored artwork interpreted by g-force

The effect behind Gmeter Studio. An `AnimationAsset` holds frames of pixel
art (`width` x `height`, up to 128 x 56) with `depth` slices per frame that sit
on consecutive panels, so artwork can be volumetric. Clips map car states to
frame ranges:

| State | Chosen when |
| --- | --- |
| idle | horizontal g below `enter_g` |
| accelerate / brake | longitudinal g is the strongest component, forward / backward |
| left / right | lateral g is the strongest component (a right turn is lateral g to the right) |
| bump | vertical g crosses `bump_g`; plays once, then hands back |

Hysteresis keeps a state until its support falls below 60 % of `enter_g`, and
`hold_s` sets the minimum time in a state, so clips do not flicker at the
threshold. A clip with 0 frames falls back: left and right mirror each other
(when `mirror` is 1), the rest use idle, and an empty idle plays everything.

On top of the clip choice the artwork moves like a loose object (spring
smoothed, clamped inside the volume), playback can speed up with g (`fps_g`),
frames can be scrubbed by g on one axis (`scrub_axis`, `frames_g`), and a
single-slice sprite can be ghosted onto neighbouring layers (`echo`).

| Param | Default | Meaning |
| --- | --- | --- |
| `px_g_x`, `px_g_y`, `px_g_z` | 50, 16, 60 px/g | travel per g on each axis |
| `hz`, `inertia` | 2, 1 | motion smoothing; 1 = thrown like a loose object |
| `enter_g`, `hold_s`, `bump_g` | 0.25 g, 0.3 s, 0.3 g | state selection |
| `fps_g`, `scrub_axis`, `frames_g` | 0, -1, 0 | g-driven playback |
| `echo`, `mirror` | 0, 1 | depth ghosting, left/right mirroring |

Assets are plain aggregates. Gmeter Studio exports them as
`src/user_animation.hpp`, which the firmware and the simulator pick up
automatically (`__has_include`); without it the built-in demo plays (a
breathing ring, chevrons per direction and a starburst on bumps). On the car,
`anim brake` pins a clip and `anim auto` returns to g-force control.

## Writing a new effect

1. Subclass `gm::Effect`; implement `name()`, `update()`, `render()`.
2. Register tunables in the constructor with `params_.add(...)`.
3. Do physics in world units (`VoxelFrame::halfExtent()` gives the box) and
   draw with `frame.sphere/point/line3` (world) or the 2D primitives (voxel).
4. Add it to `host/sim_main.cpp` (`EffectSet`) and `src/main.cpp`
   (`engine.addEffect`), run `gmeter_sim --effect yours --ascii`, add a test.

Rules of thumb for eight additive layers: outlines beat solid fills (eight
filled layers wash out), keep the front layer for anything that must be read,
let depth carry motion (front-to-back movement is the one thing this display
does that a flat one cannot), and use dithering rather than more pixels for
"dimmer".
