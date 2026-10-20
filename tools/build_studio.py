#!/usr/bin/env python3
"""Build Gmeter Studio: compile the gmeter core to WebAssembly and bundle it
into one self-contained HTML page (3D viewport, animation editor, g-force
simulator).

  pip install ziglang            # one-time: provides a clang + libc++ for wasm32-wasi
  tools/build_studio.py -o gmeter_studio.html
  tools/build_studio.py --panel-h 64 -o studio64.html   # generic 128x64 panels

The page loads Three.js from cdnjs; everything else (including the WebAssembly
module) is embedded. Open it from disk in a current browser.
"""
import argparse
import base64
import gzip
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "lib" / "gmeter_core" / "src"
WEB = ROOT / "host" / "web"
PLACEHOLDER = "__GMWASM_B64__"
SKELETON = ('<!doctype html>\n<html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n')


def standalone(html):
    """The template omits the document skeleton (the artifact host adds one);
    add it for pages opened straight from disk."""
    return html if html.lstrip().lower().startswith("<!doctype") else SKELETON + html + "\n</html>\n"


def find_compiler():
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        pass
    zig = shutil.which("zig")
    if zig:
        return [zig, "c++"]
    sys.exit("No WebAssembly C++ compiler found. Install one with: pip install ziglang")


def build_wasm(out_path, defines):
    sources = sorted(CORE.glob("gmeter/*.cpp")) + sorted(CORE.glob("gmeter/effects/*.cpp")) + [WEB / "wasm_api.cpp"]
    cmd = find_compiler() + [
        "-target", "wasm32-wasi",
        "-std=gnu++17", "-O2", "-fno-exceptions", "-fno-rtti", "-fno-math-errno",
        "-mexec-model=reactor", "-s",
        "-I", str(CORE),
    ] + [f"-D{d}" for d in defines] + [str(s) for s in sources] + ["-o", str(out_path)]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="gmeter_studio.html")
    ap.add_argument("--panel-h", type=int, default=56, help="panel height in pixels (56 transparent WiseChip, 64 generic)")
    ap.add_argument("--layers", type=int, default=8)
    ap.add_argument("--pitch", type=float, default=16.0, help="layer pitch in pixel units (GM_LAYER_PITCH_PX)")
    ap.add_argument("--wasm-out", help="also keep the raw .wasm here")
    ap.add_argument("--bare", action="store_true", help="omit <!doctype>/<head> (for hosts that add their own skeleton)")
    args = ap.parse_args()

    defines = [
        "GM_PANEL_W=128", f"GM_PANEL_H={args.panel_h}", f"GM_LAYERS={args.layers}",
        # Same smoke grid as the ESP32 firmware (idf/components/gmeter_core/CMakeLists.txt), so tuning carries over.
        f"GM_LAYER_PITCH_PX={args.pitch}f", "GM_SMOKE_NX=16", "GM_SMOKE_NY=8",
    ]
    with tempfile.TemporaryDirectory() as tmp:
        wasm = Path(tmp) / "gmeter.wasm"
        build_wasm(wasm, defines)
        raw = wasm.read_bytes()
        if args.wasm_out:
            Path(args.wasm_out).write_bytes(raw)
    gz = gzip.compress(raw, compresslevel=9)
    b64 = base64.b64encode(gz).decode("ascii")
    template = (WEB / "studio.html").read_text(encoding="utf-8")
    if PLACEHOLDER not in template:
        sys.exit(f"template has no {PLACEHOLDER} placeholder")
    html = template.replace(PLACEHOLDER, b64)
    if not args.bare:
        html = standalone(html)
    Path(args.output).write_text(html, encoding="utf-8")
    print(f"wrote {args.output}: wasm {len(raw)/1024:.0f} KB -> gzip {len(gz)/1024:.0f} KB, page {len(html)/1024:.0f} KB")


if __name__ == "__main__":
    main()
