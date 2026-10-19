#!/usr/bin/env python3
"""Boot a merged ESP32 flash image in Espressif QEMU, type serial commands and
report crashes, watchdog resets and reboots.

QEMU: https://github.com/espressif/qemu/releases (qemu-xtensa-softmmu-*).
Build the image with esptool merge_bin (bootloader, partitions, boot_app0 and
firmware at their offsets, --fill-flash-size 4MB for the ESP32; the ESP32-S3
emulator needs a 16MB flash header and fill). No IMU or panels are emulated:
the firmware runs its demo drive and the SPI writes go nowhere.

  python3 tools/qemu_boot_check.py --qemu /path/qemu-system-xtensa \\
      --image esp32dev_flash.bin --machine esp32 --script commands.txt --log boot.log

Script lines: a serial command, `sleep <s>`, or `wait <s> <text>` (wait until
the text appears). The emulator's two watchdog resets before the bootloader
hands over are normal and are not counted.
"""
import argparse
import subprocess
import sys
import threading
import time

FATAL = ("Guru Meditation", "abort()", "Backtrace", "Task watchdog", "stack overflow",
         "CORRUPT HEAP", "assert failed", "Stack canary")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", required=True, help="path to qemu-system-xtensa")
    ap.add_argument("--image", required=True, help="merged flash image")
    ap.add_argument("--machine", default="esp32", choices=["esp32", "esp32s3"])
    ap.add_argument("--script", help="file of serial commands (default: help, status, list)")
    ap.add_argument("--log", default="qemu_boot.log")
    ap.add_argument("--boot-marker", default="gmeter: ", help="line printed once per firmware boot")
    a = ap.parse_args()

    cmd = [a.qemu, "-nographic", "-machine", a.machine, "-drive", "file=%s,if=mtd,format=raw" % a.image,
           "-serial", "stdio", "-monitor", "none", "-nic", "none"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = bytearray()
    lock = threading.Lock()

    def reader():
        while True:
            chunk = p.stdout.read1(4096)
            if not chunk:
                break
            with lock:
                buf.extend(chunk)

    threading.Thread(target=reader, daemon=True).start()

    def text():
        with lock:
            return buf.decode("utf-8", "replace")

    lines = open(a.script).read().splitlines() if a.script else ["wait 30 > ", "help", "status", "list", "sleep 2"]
    for line in lines:
        line = line.rstrip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("sleep "):
            time.sleep(float(line.split()[1]))
            continue
        if line.startswith("wait "):
            _, secs, needle = line.split(" ", 2)
            end = time.time() + float(secs)
            while time.time() < end and needle not in text():
                time.sleep(0.1)
            continue
        p.stdin.write((line + "\r\n").encode())
        p.stdin.flush()
        time.sleep(0.6)
    time.sleep(1.0)
    p.kill()
    out = text()
    with open(a.log, "w") as f:
        f.write(out)
    boots = out.count(a.boot_marker)
    fatal = [k for k in FATAL if k in out]
    print("boots: %d  fatal: %s  log: %s" % (boots, ", ".join(fatal) or "none", a.log))
    return 0 if boots == 1 and not fatal else 1


if __name__ == "__main__":
    sys.exit(main())
