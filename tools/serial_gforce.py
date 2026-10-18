#!/usr/bin/env python3
"""Drive the G-meter over USB serial without driving the car.

Streams synthetic g-force (lateral, vertical, longitudinal, in g) to the
firmware's `g` command so you can watch the panels react on the bench.

  tools/serial_gforce.py /dev/ttyUSB0 --scenario slalom
  tools/serial_gforce.py COM5 --sine 0.5 --axis lat
  tools/serial_gforce.py /dev/ttyUSB0 --keys          # arrow keys steer the g vector

Requires pyserial (pip install pyserial).
"""
import argparse
import math
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: pip install pyserial")

# Mirrors the presets in lib/gmeter_core/src/gmeter/scenario.cpp (dynamic g:
# lat, vert, lon). Kept simple here; the firmware's own `demo` command plays
# the full presets on-device.
SCENARIOS = {
    "launch": [(0, 0, 0, 0), (0.9, 0, 0, 0.55), (2.5, 0, 0, 0.3), (5, 0, 0, 0)],
    "brake": [(0, 0, 0, 0), (0.9, 0, 0, -0.75), (2.6, 0, 0, -0.8), (3.1, 0, 0, 0), (5, 0, 0, 0)],
    "slalom": [(0, 0, 0, 0)] + [(0.8 + i, (0.65 if i % 2 == 0 else -0.65), 0, 0) for i in range(8)] + [(9.5, 0, 0, 0)],
    "corner": [(0, 0, 0, 0), (1.6, 0.6, 0, 0), (4, 0.6, 0, 0), (5, 0, 0, 0), (6, -0.55, 0, 0), (8, -0.55, 0, 0), (9, 0, 0, 0)],
    "bump": [(0, 0, 0, 0), (1.0, 0, 0, 0), (1.08, 0, 0.6, 0), (1.16, 0, -0.5, 0), (1.3, 0, 0, 0), (3, 0, 0, 0)],
}


def smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def sample(keys, t):
    if t <= keys[0][0]:
        return keys[0][1:]
    if t >= keys[-1][0]:
        return keys[-1][1:]
    for (t0, *a), (t1, *b) in zip(keys, keys[1:]):
        if t0 <= t <= t1:
            u = smoothstep((t - t0) / (t1 - t0)) if t1 > t0 else 1
            return tuple(x + (y - x) * u for x, y in zip(a, b))
    return keys[-1][1:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--rate", type=float, default=50.0, help="updates per second")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--scenario", choices=sorted(SCENARIOS))
    g.add_argument("--sine", type=float, metavar="AMP_G", help="sinusoidal g on one axis")
    g.add_argument("--keys", action="store_true", help="interactive arrow-key control")
    ap.add_argument("--axis", choices=["lat", "vert", "lon"], default="lat")
    ap.add_argument("--period", type=float, default=2.0, help="sine period seconds")
    ap.add_argument("--loop", action="store_true")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    time.sleep(0.5)
    ser.write(b"src serial\n")

    def send(lat, vert, lon):
        ser.write(f"g {lat:.3f} {vert:.3f} {lon:.3f}\n".encode())
        while ser.in_waiting:
            sys.stdout.write(ser.readline().decode(errors="replace"))

    dt = 1.0 / args.rate
    try:
        if args.keys:
            import curses

            def ui(stdscr):
                stdscr.nodelay(True)
                stdscr.addstr(0, 0, "arrows: lateral/longitudinal   w/s: vertical   space: zero   q: quit")
                lat = vert = lon = 0.0
                while True:
                    c = stdscr.getch()
                    if c == ord("q"):
                        break
                    elif c == curses.KEY_LEFT: lat -= 0.05
                    elif c == curses.KEY_RIGHT: lat += 0.05
                    elif c == curses.KEY_UP: lon += 0.05
                    elif c == curses.KEY_DOWN: lon -= 0.05
                    elif c == ord("w"): vert += 0.05
                    elif c == ord("s"): vert -= 0.05
                    elif c == ord(" "): lat = vert = lon = 0.0
                    lat, vert, lon = (max(-2, min(2, v)) for v in (lat, vert, lon))
                    stdscr.addstr(2, 0, f"lat {lat:+.2f}  vert {vert:+.2f}  lon {lon:+.2f}    ")
                    send(lat, vert, lon)
                    time.sleep(dt)
            curses.wrapper(ui)
        else:
            t0 = time.time()
            keys = SCENARIOS.get(args.scenario or "", None)
            while True:
                t = time.time() - t0
                if keys:
                    if t > keys[-1][0]:
                        if not args.loop:
                            break
                        t0 = time.time(); t = 0
                    lat, vert, lon = sample(keys, t)
                else:
                    v = (args.sine or 0.5) * math.sin(2 * math.pi * t / args.period)
                    lat, vert, lon = {"lat": (v, 0, 0), "vert": (0, v, 0), "lon": (0, 0, v)}[args.axis]
                send(lat, vert, lon)
                time.sleep(dt)
    except KeyboardInterrupt:
        pass
    finally:
        ser.write(b"g 0 0 0\nsrc imu\n")
        ser.close()


if __name__ == "__main__":
    main()
