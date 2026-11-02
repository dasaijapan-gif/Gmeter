# Host tests of the firmware core

C tests for `components/gmeter_core`, built and run on the PC. They are not
part of the firmware build and are not shipped to the supplier
(`tools/make_release.py` leaves this folder out of the source package).

Two kinds of tests:

- **Behaviour tests** (`test_behaviour.c`, `test_properties.c`): properties
  with tolerances, such as filter response, calibration physics, slosh volume
  conservation, particles settling, the HUD's burn-in orbit, the parameter
  tables, smoke's grid lifecycle and the engine's sanitising of bad g.
- **Reference outputs** (`test_golden.c`): about a hundred cases that drive
  the core the way the firmware does (every effect on every demo scenario
  through the engine, fuzzing through the effect vtables, non-finite g,
  mode switching, both editions, the estimator and its calibrations, text,
  the parameter tables) and fold every output into a 64-bit FNV-1a digest:
  every frame byte, burn-in offsets, parameter values, estimator outputs.
  `golden_56.txt` and `golden_64.txt` hold the recorded digests for the two
  panel heights. A change to the core that alters anything the unit shows or
  prints changes a digest.

## Build and run

From the repository root, with build directories outside the repository:

```
cmake -S idf/test -B /tmp/gm56 -G Ninja -DGM_PANEL_H=56
cmake --build /tmp/gm56 && /tmp/gm56/gm_tests
cmake -S idf/test -B /tmp/gm64 -G Ninja -DGM_PANEL_H=64
cmake --build /tmp/gm64 && /tmp/gm64/gm_tests
```

Run both heights before committing a change to the core. Each run takes a
few seconds. `-DGM_SANITIZE=ON` builds with AddressSanitizer and
UndefinedBehaviorSanitizer, including its check of every float-to-int
conversion (the digests must not change).
`gm_tests <text>` runs only the tests and cases whose names contain the text;
`gm_tests --list` lists them.

The digests depend on the compiler flags in `CMakeLists.txt` (`-O2
-ffp-contract=off -fno-math-errno`) and on the host's `sinf`, `cosf`, `expf`
and friends; they were recorded with GCC on x86-64 Linux (glibc).

## When a reference case fails

A mismatch names the case. To find the first step where the output changed,
dump the per-step digests from a tree that still passes (for example a
`git stash` or a worktree of the previous commit), then compare:

```
gm_tests --dump fx_slosh_drive > /tmp/before.txt     # reference tree
gm_tests --dump fx_slosh_drive --against /tmp/before.txt   # changed tree
```

The second command prints the first step whose digest differs; `--dump`
also prints what the case does before each step (parameter changes, mode
switches, bad g samples).

## Re-recording

Only when a change is meant to alter what the unit shows or prints:

```
gm_tests --record              # every case, for the height this build is for
gm_tests --record nonfinite_   # only the cases whose names contain the text
```

Re-record at both heights (56 and 64), re-record only the cases the change is
meant to affect, and justify every re-record in the commit message: which
cases, and why their output changed.

Cases named `nonfinite_*` feed NaN, infinite and enormous g, input that a
working sensor never produces, through the engine, which sanitises it: a
sample with a non-finite component counts as the previous sample and each
axis is clamped to 16 g. Every other case keeps g finite and within 16 g (the
engine cases within 9 g, what the serial `g` command can inject), where the
sanitising changes nothing.
