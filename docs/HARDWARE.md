# Hardware notes

## The panels

Every hobbyist transparent OLED on the market (SparkFun LCD-15173 "Transparent
Graphical OLED", Crystalfontz CFAL12856A0-0151-B, Waveshare 1.51" Transparent
OLED, DFRobot DFR0934) is the same glass: **WiseChip UG-2856KLBAG01**.

| Property | Value |
| --- | --- |
| Resolution | 128 x 56 visible pixels (the SSD1309 has 64 rows; rows 56-63 sit behind the opaque getter strip) |
| Pixel pitch | 0.274 mm; active area 35.05 x 15.32 mm; glass 42.04 x 27.22 x 1.25 mm |
| Controller | SSD1309, chip on glass; 4-wire SPI with BS1 = BS2 = 0 |
| Logic supply | VDD 1.65-3.3 V (ESP32 3.3 V logic connects directly) |
| Panel supply | **VCC 12.0-13.0 V external**. The SSD1309 has no charge pump; the breakout boards carry a boost converter. Never feed raw car battery voltage to VCC. |
| Current | 9.5 / 15.2 / 27.4 mA typical on VCC at 30 / 50 / 100 % pixels lit |
| Brightness | 100-120 cd/m2; light blue |
| Transmittance | 40 % min / 50 % typ at 550 nm per WiseChip (vendors quote up to 70 %) |
| SPI timing | cycle >= 100 ns (10 MHz max), SCLK high/low >= 50 ns, CS hold 50 ns, data latched on the rising edge |

The DFRobot **DFR0934** ("Fermion 1.51" OLED Transparent Display with
Converter") adds an 18 x 28 mm adapter board with the 12 V boost converter and
exposes SPI (pins include DC, RES, CS and MOSI) and DFRobot's GDI connector.
DFRobot lists the module as 128 x 64 and 3.3 V. The controller has 64 rows, but
the fully transparent area is 128 x 56; the remaining rows sit on the
non-transparent strip near the pins. Only SPI is useful for a stack, since
there is one GDI connector per controller board. Gmeter Studio models this
module.

Consequences for a stack of eight:

- The firmware default geometry is **128 x 56 x 8**, seven GDDRAM pages per layer.
  Switch to `esp32dev-128x64` for ordinary SSD1309 modules.
- The rearmost layer is seen through seven panes. At 50-70 % transmittance per
  pane that is roughly 1-8 % of its luminance. The driver applies a contrast
  ramp (`ramp <front> <back>`, default 0x50..0xFF) and effects are designed to
  be front-weighted; expect depth to read as "fog", not uniform brightness.
- Keep every panel in the same orientation (polarizer axes parallel). A panel
  mounted upside down can be corrected in software with `GM_PANEL_FLIP_MASK`,
  but test one first: crossed polarizers darken everything behind them.
- Each glass surface reflects about 4 %, so expect faint ghost images;
  minimise gaps and view near-normal.
- Polarizer limit is 80 C. Do not mount on a sun-baked dash top.
- Pixels age (12,000 h to half brightness at 80 cd/m2): the idle marker and any
  constant HUD should be dim, and a screensaver is worth adding.

## Wiring (classic ESP32-WROOM)

| Signal | GPIO | Notes |
| --- | --- | --- |
| SCK | 18 | VSPI IO_MUX pin; shared by all panels |
| MOSI | 23 | VSPI IO_MUX pin; shared |
| D/C | 19 | shared |
| RES | 13 | shared; 10 k pull-up to 3.3 V |
| CS layer 0..7 | 5, 17, 16, 4, 27, 26, 25, 33 | layer 0 is nearest the driver |
| SDA / SCL | 21 / 22 | MPU-6050 (GY-521) |
| Button | 0 | BOOT button, active low |

Avoid GPIO 6-11 (flash), 34-39 (input only), 12 (MTDI strapping), 1/3 (UART).
GPIO 5 is a strapping pin but is pulled up at boot, which is fine for an
active-low CS. On WROVER modules GPIO 16/17 belong to the PSRAM: use 32 and 14.
ESP32-S3 pins are in `src/board_config.hpp` (SPI2 IO_MUX: SCK 12, MOSI 11).
On the ESP32-S3-DevKitC-1 the serial CLI is on the connector labelled UART;
the native USB connector only flashes (add `-DARDUINO_USB_CDC_ON_BOOT=1` to the
`esp32-s3` build flags to move the CLI to native USB).

Eight loads on SCK/MOSI are about 100 pF. Keep the bus short with ground
returns, and if wiring exceeds ~10 cm add 22-33 ohm series resistors at the
ESP32 end or drop `GM_SPI_HZ` to 8 MHz.

Chip selects are plain GPIOs toggled around each transfer; the ESP32 hardware
only has three CS lines per SPI host, so the driver does not use them.

## Frame transfer budget

Per panel per frame: 6 command bytes (column 0-127, pages 0-6) then 896 data
bytes. Eight panels at 10 MHz: 5.8 ms (~170 fps ceiling); at 8 MHz 7.2 ms.
The Arduino SPI call blocks the CPU for that time, so the transfer runs on
core 0 while core 1 renders the next frame. A later upgrade is the ESP-IDF
`spi_master` queue with DMA buffers (one device with `spics_io_num = -1` and
CS/DC toggled in the pre/post callbacks), which frees that core time.

The bus is write-only. A panel scrambled by ignition noise cannot report it,
so the firmware re-sends the window command every frame and does a soft
refresh every `reinitMinutes` (default 10, set with `reinit <minutes>`, 0 = off):
every register is re-sent without a reset, RAM clear or delay, so nothing
blinks. Bare `reinit` does a hard reset (RES pulse, init, clear, 100 ms wait),
which blanks the stack for about 0.12 s.

## Power

- 3.3 V for the ESP32 and panel logic from a buck converter (not an LDO),
  rated >= 2 A: eight boost converters at full white draw about 1.2-1.6 A.
- Feed the car's 12 V to a 5 V buck, then 3.3 V. The GY-521 takes 5 V (its own
  LDO and pull-ups then sit at 3.3 V).
- Panel VCC (12 V) comes from each breakout's boost converter, which runs
  whenever 3.3 V is present; the firmware cannot switch it. At boot the
  firmware pulses reset, initialises and clears each panel, waits 100 ms and
  turns the display on. It does not follow the SSD1309 datasheet order (VCC on
  after init; display off, VCC off, then VDD off at power-down), which is how
  hobby modules are normally run. A custom adapter with a boost enable pin
  could add that; panel VCC must float, not be grounded, when off.

## IMU

Default: MPU-6050 on a GY-521 module at I2C address 0x68 (0x69 with AD0 high).
The part is end-of-life at TDK and modules are often clone dies; the driver
checks WHO_AM_I (0x68 genuine; 0x98 and 0x72 clones are accepted with a
warning). Clone dies are 6500-family parts whose accelerometer low-pass sits
in ACCEL_CONFIG2, so the driver sets it there too, and uses their own
temperature formula. `status` shows the raw vector and |a|, which should be
0.95-1.05 g at rest. A sensor that stops answering, or returns all zeros
(it reset into sleep), is marked lost after 10 bad samples; the display then
shows 0 g and the firmware re-initialises the bus and sensor every second.

Configuration used (`src/imu_mpu6050.cpp`): reset, PLL clock from the X gyro,
all axes on, DLPF 44 Hz, 200 Hz output, +/-4 g (vertical bumps exceed 2 g and
clipping inside the DLPF biases the output), gyro +/-250 dps. Burst read of
accelerometer and temperature from 0x3B.

Software filter: 2nd-order Butterworth at 4 Hz (`filter <hz>`). Car manoeuvres
live below 2-3 Hz; wheel hop is 10-15 Hz; a four-cylinder engine's second
order is 25 Hz at idle. 4 Hz gives about 56 ms of lag, -16 dB at 10 Hz and
-33 dB at 25 Hz.

Mount the sensor rigidly (soft mounts resonate), away from the sun, and allow
a minute of warm-up before `cal rest`. Tell the firmware which sensor axis
faces the windshield first (`cal fwd`): the default `+z` suits a board standing
upright; a GY-521 lying flat has +Z up and needs one of `+x -x +y -y`. If the
hint is within 30 degrees of vertical, `cal rest` sets only "up" and asks for
`cal brake`, which measures forward from a straight, firm stop. Zero-g drift with temperature is tens of
mg, which shows as a permanent small lean of the liquid; `zero on` enables a
slow re-zero of the horizontal axes while the car is provably still.

A premium alternative is the LSM6DSO/LSM6DSOX (much lower noise and offset,
in production); the `Mpu6050` class is small enough to clone for it.

## Open questions only the builder can answer

- Which exact boards (do all eight have the 12 V boost and SPI jumpers set)?
- Classic ESP32-WROOM, WROVER, or S3? This fixes the pin map.
- Physical panel spacing and whether all getter strips are on the same edge.
  Spacing sets `GM_LAYER_PITCH_PX` (spacing in mm / 0.274).
- Measured transmittance through 1, 2, 4 panels: decides the contrast ramp
  and whether 8 layers are usable or 6 look better.
- Power source in the car and wiring length to the farthest panel.
