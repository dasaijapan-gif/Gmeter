#!/usr/bin/env python3
"""Build the firmware for every board and assemble the supplier package.

The package is built from a clean `git archive` of HEAD, so the binaries always
match the source that ships with them (uncommitted changes are not included).

  python3 tools/make_release.py                      # dist/gmeter_supplier + .zip
  python3 tools/make_release.py --studio gmeter_studio.html --pio ~/.platformio/penv/bin/pio

Layout of the result:

  README_FIRST.md          supplier guide (docs/SUPPLIER.md)
  BUILD_INFO.txt           commit, toolchain, memory use
  firmware/<chip>_<W>x<H>/ <name>_full.bin (flash at 0x0), the separate parts,
                           flash_args.txt
  firmware/SHA256SUMS.txt
  source/gmeter/           the PlatformIO project (whole repository)
  docs/                    documentation and licences
  studio/                  Gmeter Studio, if --studio is given

Needs PlatformIO (`pio`) and esptool (`pip install esptool`, or the copy
PlatformIO installs).
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ENVS = ["esp32dev", "esp32-s3", "esp32dev-128x64"]


def run(cmd, cwd=None, capture=False):
    print("+ " + " ".join(cmd), flush=True)
    if capture:
        r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            sys.stdout.write(r.stdout)
            raise SystemExit("command failed: %s" % " ".join(cmd))
        return r.stdout
    subprocess.run(cmd, cwd=cwd, check=True)
    return ""


def chip_of(meta):
    cc = os.path.basename(meta.get("cc_path", ""))
    m = re.match(r"(?:xtensa|riscv32)-(esp32\w*)-elf-gcc", cc)
    if not m:
        raise SystemExit("cannot tell the chip from compiler %r" % cc)
    return m.group(1)


def define(meta, name, default=None):
    for d in meta.get("defines", []):
        if d.startswith(name + "="):
            return d.split("=", 1)[1]
    return default


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "dist", "gmeter_supplier"))
    ap.add_argument("--envs", nargs="+", default=DEFAULT_ENVS)
    ap.add_argument("--pio", default="pio", help="PlatformIO command")
    ap.add_argument("--esptool", default=None, help="esptool command (default: python -m esptool)")
    ap.add_argument("--studio", help="built Gmeter Studio page to include")
    ap.add_argument("--no-zip", action="store_true")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    esptool = a.esptool.split() if a.esptool else [sys.executable, "-m", "esptool"]

    # 1. Source: a clean export of HEAD.
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, stdout=subprocess.PIPE, text=True).stdout.strip()
    if dirty:
        print("warning: uncommitted changes are not part of the package (built from HEAD)")
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, stdout=subprocess.PIPE, text=True,
                            check=True).stdout.strip()
    tar_bytes = subprocess.run(["git", "archive", "--format=tar", "--prefix=gmeter/", "HEAD"], cwd=ROOT,
                               stdout=subprocess.PIPE, check=True).stdout
    src_parent = os.path.join(out, "source")
    os.makedirs(src_parent)
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as t:
        if hasattr(tarfile, "data_filter"):
            t.extractall(src_parent, filter="data")
        else:
            t.extractall(src_parent)
    project = os.path.join(src_parent, "gmeter")

    # 2. Build every environment from that source.
    build_log = run([a.pio, "run"] + sum((["-e", e] for e in a.envs), []), cwd=project, capture=True)
    usage = {}
    current = None
    for line in build_log.splitlines():
        m = re.match(r"Processing (\S+)", line)
        if m:
            current = m.group(1)
        m = re.match(r"(RAM|Flash):.*used (\d+) bytes from (\d+) bytes", line)
        if m and current:
            usage.setdefault(current, {})[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    meta_raw = run([a.pio, "project", "metadata", "--json-output"] + sum((["-e", e] for e in a.envs), []),
                   cwd=project, capture=True)
    meta_all = json.loads(meta_raw[meta_raw.index("{"):])

    # 3. Firmware folders.
    fw_root = os.path.join(out, "firmware")
    os.makedirs(fw_root)
    info = []
    for env in a.envs:
        meta = meta_all[env]
        chip = chip_of(meta)
        w, h = define(meta, "GM_PANEL_W", "128"), define(meta, "GM_PANEL_H", "64")
        label = "%s_%sx%s" % (chip, w, h)
        dest = os.path.join(fw_root, label)
        os.makedirs(dest)
        parts = []
        for img in meta["extra"]["flash_images"]:
            name = os.path.basename(img["path"])
            shutil.copy(img["path"], os.path.join(dest, name))
            parts.append((img["offset"], name))
        app = os.path.join(os.path.dirname(meta["prog_path"]), "firmware.bin")
        shutil.copy(app, os.path.join(dest, "firmware.bin"))
        parts.append((meta["extra"]["application_offset"], "firmware.bin"))
        parts.sort(key=lambda p: int(p[0], 16))
        full = "gmeter_%s_full.bin" % label
        args = []
        for off, name in parts:
            args += [off, name]
        run(esptool + ["--chip", chip, "merge_bin", "-o", full, "--flash_mode", "keep", "--flash_freq", "keep",
                       "--flash_size", "keep"] + args, cwd=dest, capture=True)
        with open(os.path.join(dest, "flash_args.txt"), "w") as f:
            f.write("# Either write the single image at 0x0:\n")
            f.write("esptool.py --chip %s --baud 460800 write_flash 0x0 %s\n\n" % (chip, full))
            f.write("# or the separate parts at their offsets:\n")
            f.write("esptool.py --chip %s --baud 460800 write_flash %s\n" % (chip, " ".join(args)))
        u = usage.get(env, {})
        info.append("%-16s %-16s chip %-8s RAM %s  Flash %s" % (
            env, label, chip,
            "%d/%d" % u["RAM"] if "RAM" in u else "?", "%d/%d" % u["Flash"] if "Flash" in u else "?"))
    with open(os.path.join(fw_root, "SHA256SUMS.txt"), "w") as f:
        for dirpath, _, files in sorted(os.walk(fw_root)):
            for name in sorted(files):
                if name == "SHA256SUMS.txt" or not name.endswith(".bin"):
                    continue
                path = os.path.join(dirpath, name)
                f.write("%s  %s\n" % (sha256(path), os.path.relpath(path, fw_root)))

    # Leave the shipped source clean.
    shutil.rmtree(os.path.join(project, ".pio"), ignore_errors=True)

    # 4. Docs, guide, Studio, build info.
    docs = os.path.join(out, "docs")
    os.makedirs(docs)
    for name in sorted(os.listdir(os.path.join(project, "docs"))):
        shutil.copy(os.path.join(project, "docs", name), docs)
    shutil.copy(os.path.join(project, "LICENSE"), docs)
    shutil.copy(os.path.join(project, "docs", "SUPPLIER.md"), os.path.join(out, "README_FIRST.md"))
    if a.studio:
        os.makedirs(os.path.join(out, "studio"))
        shutil.copy(a.studio, os.path.join(out, "studio", "gmeter_studio.html"))
    toolchain = []
    for env in a.envs:
        cc = meta_all[env].get("cc_path", "")
        try:
            first = subprocess.run([cc, "--version"], stdout=subprocess.PIPE, text=True).stdout.splitlines()[0]
        except (OSError, IndexError):
            first = os.path.basename(cc)
        if first not in toolchain:
            toolchain.append(first)
    framework = ""
    fw_pkg = os.path.join(os.path.dirname(os.path.dirname(meta_all[a.envs[0]]["extra"]["flash_images"][-1]["path"])),
                          "..", "package.json")
    try:
        with open(os.path.normpath(fw_pkg)) as f:
            framework = json.load(f).get("version", "")
    except (OSError, ValueError):
        pass
    with open(os.path.join(out, "BUILD_INFO.txt"), "w") as f:
        f.write("Gmeter firmware package\n")
        f.write("commit     %s\n" % commit)
        f.write("built      %s UTC\n" % datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M"))
        f.write("platform   see platformio.ini (pinned)\n")
        if framework:
            f.write("framework  framework-arduinoespressif32 %s\n" % framework)
        for line in toolchain:
            f.write("compiler   %s\n" % line)
        f.write("\n")
        f.write("\n".join(info) + "\n")

    if not a.no_zip:
        archive = shutil.make_archive(out, "zip", os.path.dirname(out), os.path.basename(out))
        print("wrote", archive)
    print("wrote", out)


if __name__ == "__main__":
    main()
