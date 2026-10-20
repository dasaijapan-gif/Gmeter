# Gmeter firmware (ESP-IDF)

The firmware for the Gmeter volumetric g-force meter: an ESP32 or ESP32-S3
drives eight stacked transparent SSD1309 OLED panels as a 3D display and reads
an MPU-6050 accelerometer. It is a plain ESP-IDF project written in C.

| Folder / file | What it is |
| --- | --- |
| `main/` | The application: `app_main.c` (start-up and the three tasks), `display_ssd1309.c` (panels), `imu_mpu6050.c` (sensor), `serial_cli.c` (serial commands), `storage.c` (saved settings), `console_io.c` (serial port), `board_config.h` (pins), `Kconfig.projbuild` (the Gmeter menu) |
| `components/gmeter_core/` | The hardware-independent core: g-force maths, the voxel frame and the ten effects |
| `sdkconfig.defaults`, `sdkconfig.defaults.esp32s3` | Project settings (flash size, CPU speed, console, log level, ...) |
| `configs/hud.defaults` | Overlay: HUD edition |
| `configs/panel_128x64.defaults` | Overlay: generic SSD1309 128x64 panels |
| `partitions.csv` | Flash layout: settings (NVS) at 0x9000, the app at 0x10000 |

## 1. Install ESP-IDF

Install ESP-IDF **v5.2 or newer** (the firmware is built and tested with
v5.5.6) by following Espressif's *Get Started* guide for your PC:
<https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/>.
Install the tools for `esp32` and `esp32s3`. Then open an ESP-IDF terminal
(Windows: the "ESP-IDF CMD" shortcut; Linux/macOS: run `. ~/esp/esp-idf/export.sh`,
the `export.sh` in your ESP-IDF folder)
and change to this folder (the one with this README).

## 2. Build and flash (standard edition, 128x56 panels)

```sh
idf.py set-target esp32          # classic ESP32 (WROOM-32, WROOM-32E, WROVER)
idf.py build
idf.py -p COM5 flash monitor     # your serial port, e.g. /dev/ttyUSB0 on Linux
```

For an ESP32-S3 board use `idf.py set-target esp32s3` instead. `set-target`
starts a fresh configuration; run it once per chip, before the first build.
`monitor` opens the serial console at 115200 baud (leave it with Ctrl+]).
Type `help` there for the commands. The supplier guide (`docs/SUPPLIER.md` in
the Gmeter repository, `README_FIRST.md` in a supplier package) describes the
checks and the calibration.

## 3. Variants

Six variants are used: two editions, each for three board/panel combinations.
Build each one in its own folder with its own configuration, so they never
mix:

| Variant | Command |
| --- | --- |
| ESP32, 128x56, standard | `idf.py -B build_esp32 -DSDKCONFIG=build_esp32/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" set-target esp32 build` |
| ESP32-S3, 128x56, standard | `idf.py -B build_esp32s3 -DSDKCONFIG=build_esp32s3/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" set-target esp32s3 build` |
| ESP32, 128x64, standard | `idf.py -B build_esp32_64 -DSDKCONFIG=build_esp32_64/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/panel_128x64.defaults" set-target esp32 build` |
| ESP32, 128x56, HUD edition | `idf.py -B build_esp32_hud -DSDKCONFIG=build_esp32_hud/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/hud.defaults" set-target esp32 build` |
| ESP32-S3, 128x56, HUD edition | `idf.py -B build_esp32s3_hud -DSDKCONFIG=build_esp32s3_hud/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/hud.defaults" set-target esp32s3 build` |
| ESP32, 128x64, HUD edition | `idf.py -B build_esp32_64_hud -DSDKCONFIG=build_esp32_64_hud/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/panel_128x64.defaults;configs/hud.defaults" set-target esp32 build` |

To flash a variant, give the same `-B` and `-DSDKCONFIG` again:
`idf.py -B build_esp32_hud -DSDKCONFIG=build_esp32_hud/sdkconfig -p COM5 flash monitor`.

The standard edition offers all ten effects. The HUD edition always shows the
NERV-style `hud` effect (see `docs/SUPPLIER_HUD.md` in the repository); it
refuses `mode`, `cycle` and `anim`, and its `save` leaves the effect stored by
the standard edition alone.

`tools/make_release.py` in the Gmeter repository builds all three variants of
an edition this way and packs the files for flashing.

## 4. Options (menuconfig)

`idf.py menuconfig` (with the same `-B` and `-DSDKCONFIG` for a variant
folder), then **Gmeter**:

| Option | Default | What it does |
| --- | --- | --- |
| Edition | Standard | Standard (all effects) or HUD edition (always `hud`) |
| Panels > Panel type | WiseChip 128x56 | WiseChip 128x56 transparent SSD1309 (DFRobot DFR0934 and others, panel model 0), generic SSD1309 128x64 (model 1) or SSD1306 128x64 (model 2) |
| Panels > SPI clock (Hz) | 10000000 | Panel bus speed; 10 MHz is the maximum. Try 8000000 with long wires |
| Panels > Panels mounted upside down | 0x00 | Bit mask: bit i turns round the picture of the panel on the i-th chip-select pin (the one `test i` lights; normally panel 0 is nearest the driver) |
| Panels > Chip selects wired windshield-first | off | The stack is wired the other way round |
| Target frame rate | 60 | Frames per second |

Save, then rebuild with the same options:
`idf.py -B build_x -DSDKCONFIG=build_x/sdkconfig build` (or its `flash`
command); plain `idf.py build` uses the default `build` folder. Do not re-run
the section 3 line for that folder: its `set-target` starts a fresh
configuration and erases the menuconfig changes. The values in
`sdkconfig.defaults` and the overlays only apply to a new configuration: after
editing one of those files, delete the build folder (or its `sdkconfig`) and
build again.

Pins are not menu options: they are in `main/board_config.h` (classic ESP32
and ESP32-S3 sections). The wiring tables in the supplier guide match it.

**Serial console.** The console is UART0 at 115200 baud. On the
ESP32-S3-DevKitC-1 that is the USB connector labelled UART. For an S3 board
with only a native USB connector, choose **Component config > ESP System
Settings > Channel for console output > USB Serial/JTAG Controller** in
menuconfig and rebuild; the command line then works on the native USB port.

## 5. Flash layout and addresses

| Part | File in the build folder | ESP32 address | ESP32-S3 address |
| --- | --- | --- | --- |
| Bootloader | `bootloader/bootloader.bin` | 0x1000 | 0x0 |
| Partition table | `partition_table/partition-table.bin` | 0x8000 | 0x8000 |
| Saved settings (NVS) | (written by the firmware) | 0x9000, 24 KB | 0x9000, 24 KB |
| Application | `gmeter.bin` | 0x10000 | 0x10000 |

The images are made for 4 MB of flash and run on any module with 4 MB or more.
`idf.py flash` writes the three files. With esptool directly, from the build
folder: `python -m esptool --chip esp32 -b 460800 write_flash @flash_args`
(use `--chip esp32s3` in an ESP32-S3 build folder). One file for address 0x0:

```sh
cd build_esp32
python -m esptool --chip esp32 merge_bin -o gmeter_full.bin @flash_args
```

These are esptool v4 spellings, which ESP-IDF 5.2-5.4 install; esptool v5
(ESP-IDF 5.5) also accepts them, or `write-flash` and `merge-bin`.

Writing a whole image (or `idf.py erase-flash`) clears the saved settings and
calibration. To update a unit and keep them, write only `gmeter.bin` at
0x10000 (`idf.py app-flash`), as long as `partitions.csv` is unchanged.

On boards with more than 4 MB of flash, ESP-IDF prints one line at start-up,
`W (...) spi_flash: Detected size(...k) larger than the size in the binary
image header(4096k). Using the size in the binary image header.` It is
harmless.

## 6. Custom artwork

Draw an animation in Gmeter Studio and export it. Save the exported header as
`main/user_animation.h` and rebuild: the `anim` effect then plays your artwork
instead of the built-in demo (`mode anim`, then `status` shows its name).
Delete the file to go back to the demo.

## 7. Start-up output

After the chip's own boot lines, a unit without a sensor prints:

```
gmeter: 128x56x8 voxels, panel model 0, spi 10000000 Hz, target 60 fps
no saved settings; using defaults
imu: not found (WHO_AM_I 0x00); showing demo scenario 'drive' and retrying. Type: help
>
```

The HUD edition adds `single-effect build: always shows hud` before the
`imu:` line.

## 8. First use with a sensor: calibration

The firmware has to learn how the MPU-6050 is mounted. Until then a sensor
that is not standing exactly upright with its Y arrow up shows about 1 g
while it is still (a value near 1.00, the ball at an edge, warnings): that is
expected.

1. Tell the firmware which sensor axis points at the windshield:
   `cal fwd <+x|-x|+y|-y|+z|-z>`. The default `+z` only fits a board standing
   upright with its Z axis forward. A GY-521 lying flat has +Z up, so use the
   in-plane axis that faces forward (for example `cal fwd +y`).
2. Park on level ground and type `cal rest` (hold still for 2 s). If the
   forward hint points up or down, the rest capture still sets "up" but
   reports that forward is unknown; step 3 is then required.
3. Recommended: drive straight, type `cal brake` and brake firmly for 1.5 s.
   This measures forward directly and removes any yaw in the mount. Run it
   again after any new `cal rest`.
4. `save`.

`save` stores the effect on screen (standard edition), the contrast ramp,
readout, invert, forward axis, auto-zero, filter, auto-cycle, panel refresh
interval and the calibration. `set` changes to effect parameters are RAM-only
and revert on reboot; to keep tuned values, change the effect's defaults in
its `gm_<name>_init` function (`components/gmeter_core/src/effects/`).

If the sensor drops out (loose connector, brownout), the display relaxes to
0 g and the firmware retries every second; `status` shows the state. A sensor
that appears after boot is picked up automatically. Without a sensor the
firmware plays a recorded demo drive so the panels can be checked on the
bench; `src serial` followed by `g 0.3 0 -0.5` sets the g-force by hand.

Holding the BOOT button for two seconds also starts a rest calibration (like
`cal rest`), so only do that with the car parked level. A short press steps
to the next effect (standard edition only).
