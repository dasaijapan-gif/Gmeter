#!/usr/bin/env python3
"""Build the ESP-IDF firmware for every board and assemble the supplier package.

The package is built from a clean `git archive` of HEAD, so the binaries always
match the source that ships with them (uncommitted changes are not included).
Run it from an ESP-IDF v5 shell (after `. $IDF_PATH/export.sh`), so that
idf.py and ESP-IDF's Python (with esptool) are on PATH.

  python3 tools/make_release.py                          # standard edition: dist/gmeter_supplier + .zip
  python3 tools/make_release.py --edition hud --out dist/gmeter_hud
  python3 tools/make_release.py --studio gmeter_studio.html

Layout of the result:

  README_FIRST.md            supplier guide (docs/SUPPLIER.md, or docs/SUPPLIER_HUD.md
                             for the HUD edition; --guide overrides)
  BUILD_INFO.txt             commit, ESP-IDF version, compiler, memory use
  firmware/<chip>_<W>x<H>[_hud]/
                             gmeter_<label>_full.bin (flash at 0x0), the separate
                             parts, flash_args.txt
  firmware/SHA256SUMS.txt
  source/gmeter/             the ESP-IDF project (idf/ of the repository, without
                             the host tests): C sources, menuconfig options,
                             partition table
  docs/                      hardware and effect notes, licences
  studio/                    Gmeter Studio, if --studio is given
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# chip, extra sdkconfig defaults (besides sdkconfig.defaults)
VARIANTS = [
    ("esp32", []),
    ("esp32s3", []),
    ("esp32", ["configs/panel_128x64.defaults"]),
]
EDITION_DEFAULTS = {"standard": [], "hud": ["configs/hud.defaults"]}
EDITION_GUIDE = {"standard": "docs/SUPPLIER.md", "hud": "docs/SUPPLIER_HUD.md"}
DOCS = ["docs/HARDWARE.md", "docs/EFFECTS.md", "docs/THIRD_PARTY.md", "LICENSE"]


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


def committed(path):
    # Git's tree lookup is exact: no build products, drive paths or case variants.
    return subprocess.run(["git", "cat-file", "-e", "HEAD:" + path], cwd=ROOT,
                          stderr=subprocess.DEVNULL).returncode == 0


def sdkconfig_value(path, name):
    with open(path) as f:
        for line in f:
            if line.startswith(name + "="):
                return line.split("=", 1)[1].strip().strip('"')
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def memory_use(build):
    """Static RAM and flash use (from ESP-IDF's size report), as text for BUILD_INFO."""
    try:
        out = subprocess.run([sys.executable, "-m", "esp_idf_size", "--format", "json2",
                              os.path.join(build, "gmeter.map")], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             text=True).stdout
        parts = []
        for mem in json.loads(out)["layout"]:
            if mem["name"] in ("DRAM", "DIRAM", "IRAM", "Flash Code", "Flash Data") and mem["used"]:
                parts.append("%s %d%s" % (mem["name"], mem["used"], "/%d" % mem["total"] if mem["total"] else ""))
        app = os.path.getsize(os.path.join(build, "gmeter.bin"))
        return ", ".join(parts + ["gmeter.bin %d" % app])
    except (OSError, ValueError, KeyError, TypeError):
        return "?"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--edition", choices=sorted(EDITION_DEFAULTS), default="standard")
    ap.add_argument("--out", help="output folder (default dist/gmeter_supplier, or dist/gmeter_hud)")
    ap.add_argument("--idf", default="idf.py", help="idf.py command")
    ap.add_argument("--esptool", default=None, help="esptool command (default: python -m esptool)")
    ap.add_argument("--studio", help="built Gmeter Studio page to include")
    ap.add_argument("--guide", help="guide copied to README_FIRST.md (repository path; default: the edition's guide)")
    ap.add_argument("--no-zip", action="store_true")
    a = ap.parse_args()

    out = os.path.abspath(a.out or os.path.join(ROOT, "dist", "gmeter_supplier" if a.edition == "standard" else
                                                "gmeter_" + a.edition))
    guide = posixpath.normpath((a.guide or EDITION_GUIDE[a.edition]).replace("\\", "/"))
    if not committed(guide):
        raise SystemExit("guide %s is not in the committed source (HEAD)" % guide)
    for d in DOCS:
        if not committed(d):
            raise SystemExit("%s is not in the committed source (HEAD)" % d)
    if shutil.which(a.idf) is None:
        raise SystemExit("%s not found: run this from an ESP-IDF shell (. $IDF_PATH/export.sh)" % a.idf)
    esptool = a.esptool.split() if a.esptool else [sys.executable, "-m", "esptool"]

    # Every firmware folder the guide names must be built.
    fixed = "_" + a.edition if a.edition != "standard" else ""
    guide_text = subprocess.run(["git", "show", "HEAD:" + guide], cwd=ROOT, stdout=subprocess.PIPE, text=True,
                                check=True).stdout
    named = sorted(set(re.findall(r"firmware/([A-Za-z0-9_]+)/", guide_text)))
    expected = {"%s_128x%d%s" % (chip, 64 if extra else 56, fixed) for chip, extra in VARIANTS}
    absent = [n for n in named if n not in expected]
    if absent:
        raise SystemExit("%s names firmware folders this edition does not make: %s" % (guide, ", ".join(absent)))

    if os.path.exists(out):
        shutil.rmtree(out)
    if os.path.exists(out + ".zip"):
        os.remove(out + ".zip")
    os.makedirs(out)

    # 1. Source: a clean export of the ESP-IDF project at HEAD.
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, stdout=subprocess.PIPE, text=True).stdout.strip()
    if dirty:
        print("warning: uncommitted changes are not part of the package (built from HEAD)")
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, stdout=subprocess.PIPE, text=True,
                            check=True).stdout.strip()
    tar_bytes = subprocess.run(["git", "archive", "--format=tar", "--prefix=gmeter/", "HEAD:idf"], cwd=ROOT,
                               stdout=subprocess.PIPE, check=True).stdout
    src_parent = os.path.join(out, "source")
    os.makedirs(src_parent)
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as t:
        if hasattr(tarfile, "data_filter"):
            t.extractall(src_parent, filter="data")
        else:
            t.extractall(src_parent)
    project = os.path.join(src_parent, "gmeter")
    # The host tests compare the C core with the C++ core of the PC tools,
    # which is not part of this package.
    shutil.rmtree(os.path.join(project, "host_test"), ignore_errors=True)
    shipped = set()
    for dirpath, _, files in os.walk(project):
        for name in files:
            shipped.add(os.path.relpath(os.path.join(dirpath, name), project))

    # 2. Build every variant from that source, outside the shipped tree.
    work = tempfile.mkdtemp(prefix="gmeter_release_")
    fw_root = os.path.join(out, "firmware")
    os.makedirs(fw_root)
    info = []
    try:
        for chip, extra in VARIANTS:
            defaults = ["sdkconfig.defaults"] + extra + EDITION_DEFAULTS[a.edition]
            build = os.path.join(work, "build_%s_%s" % (chip, "_".join(os.path.basename(x).split(".")[0] for x in extra)
                                                         or "default"))
            run([a.idf, "-C", project, "-B", build, "-DSDKCONFIG=" + os.path.join(build, "sdkconfig"),
                 "-DSDKCONFIG_DEFAULTS=" + ";".join(os.path.join(project, d) for d in defaults),
                 "set-target", chip, "build"], capture=True)
            sdkconfig = os.path.join(build, "sdkconfig")
            width = sdkconfig_value(sdkconfig, "CONFIG_GMETER_PANEL_W")
            height = sdkconfig_value(sdkconfig, "CONFIG_GMETER_PANEL_H")
            hud = sdkconfig_value(sdkconfig, "CONFIG_GMETER_EDITION_HUD") == "y"
            if (a.edition == "hud") != hud:
                raise SystemExit("%s: the edition in sdkconfig does not match --edition %s" % (build, a.edition))
            label = "%s_%sx%s%s" % (chip, width, height, "_hud" if hud else "")
            if label not in expected:
                raise SystemExit("unexpected variant %s (expected one of %s)" % (label, ", ".join(sorted(expected))))
            dest = os.path.join(fw_root, label)
            os.makedirs(dest)
            with open(os.path.join(build, "flasher_args.json")) as f:
                flasher = json.load(f)
            parts = []
            for off, rel in sorted(flasher["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
                name = os.path.basename(rel)
                shutil.copy(os.path.join(build, rel), os.path.join(dest, name))
                parts.append((off, name))
            full = "gmeter_%s_full.bin" % label
            args = []
            for off, name in parts:
                args += [off, name]
            run(esptool + ["--chip", chip, "merge-bin", "-o", full, "--flash-mode", "keep", "--flash-freq", "keep",
                           "--flash-size", "keep"] + args, cwd=dest, capture=True)
            app_off, app_name = next((o, n) for o, n in parts if n == "gmeter.bin")
            with open(os.path.join(dest, "flash_args.txt"), "w") as f:
                f.write("# esptool v5 commands (esptool.py v4: write_flash and erase_flash with underscores).\n\n")
                f.write("# Either write the single image at 0x0:\n")
                f.write("esptool --chip %s --baud 460800 write-flash 0x0 %s\n\n" % (chip, full))
                f.write("# or the separate parts at their offsets:\n")
                f.write("esptool --chip %s --baud 460800 write-flash %s\n\n" % (chip, " ".join(args)))
                f.write("# The single _full.bin clears the saved settings and calibration (it covers\n")
                f.write("# the NVS area at 0x9000); the separate parts leave them in place. To start\n")
                f.write("# clean with the parts, run erase-flash first.\n\n")
                f.write("# To update a unit and keep its settings, write only the app (only if\n")
                f.write("# partition-table.bin is unchanged from the version on the board; compare\n")
                f.write("# SHA256SUMS.txt):\n")
                f.write("esptool --chip %s --baud 460800 write-flash %s %s\n" % (chip, app_off, app_name))
            info.append("%-20s %-8s %s" % (label, chip, memory_use(build)))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    with open(os.path.join(fw_root, "SHA256SUMS.txt"), "w") as f:
        for dirpath, _, files in sorted(os.walk(fw_root)):
            for name in sorted(files):
                if name == "SHA256SUMS.txt" or not name.endswith(".bin"):
                    continue
                path = os.path.join(dirpath, name)
                f.write("%s  %s\n" % (sha256(path), os.path.relpath(path, fw_root)))

    # Leave the shipped source exactly as exported.
    for dirpath, dirs, files in os.walk(project, topdown=False):
        for name in files:
            rel = os.path.relpath(os.path.join(dirpath, name), project)
            if rel not in shipped:
                os.remove(os.path.join(dirpath, name))
        if not os.listdir(dirpath) and dirpath != project:
            os.rmdir(dirpath)

    # 3. Docs, guide, Studio, build info.
    docs = os.path.join(out, "docs")
    os.makedirs(docs)
    for d in DOCS + [guide]:
        with open(os.path.join(docs, os.path.basename(d)), "wb") as f:
            f.write(subprocess.run(["git", "show", "HEAD:" + d], cwd=ROOT, stdout=subprocess.PIPE, check=True).stdout)
    shutil.copy(os.path.join(docs, os.path.basename(guide)), os.path.join(out, "README_FIRST.md"))
    if a.studio:
        os.makedirs(os.path.join(out, "studio"))
        shutil.copy(a.studio, os.path.join(out, "studio", "gmeter_studio.html"))
    idf_version = subprocess.run([a.idf, "--version"], stdout=subprocess.PIPE, text=True).stdout.strip()
    compilers = []
    for cc in ("xtensa-esp-elf-gcc",):
        try:
            compilers.append(subprocess.run([cc, "--version"], stdout=subprocess.PIPE, text=True).stdout.splitlines()[0])
        except (OSError, IndexError):
            pass
    with open(os.path.join(out, "BUILD_INFO.txt"), "w") as f:
        f.write("Gmeter firmware package (%s edition)\n" % a.edition)
        f.write("commit     %s\n" % commit)
        f.write("guide      %s\n" % guide)
        f.write("built      %s UTC\n" % datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M"))
        f.write("framework  %s\n" % idf_version)
        for line in compilers:
            f.write("compiler   %s\n" % line)
        f.write("\n")
        f.write("\n".join(info) + "\n")

    if not a.no_zip:
        archive = shutil.make_archive(out, "zip", os.path.dirname(out), os.path.basename(out))
        print("wrote", archive)
    print("wrote", out)


if __name__ == "__main__":
    main()
