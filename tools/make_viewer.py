#!/usr/bin/env python3
"""Bundle a gmeter_sim recording into the self-contained web viewer.

  host/build/gmeter_sim --effect all --scenario drive --out frames.bin
  tools/make_viewer.py frames.bin -o gmeter_viewer.html

The recording is gzip-compressed and base64-embedded into host/viewer/index.html,
which decodes it in the browser (DecompressionStream). Open the result directly
from disk; no server needed.
"""
import argparse
import base64
import gzip
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLACEHOLDER = "__GMV_B64__"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", help="frames.bin from gmeter_sim --out")
    ap.add_argument("-o", "--output", default="gmeter_viewer.html")
    ap.add_argument("--template", default=str(ROOT / "host" / "viewer" / "index.html"))
    ap.add_argument("--max-mb", type=float, default=15.0, help="refuse to write a page larger than this")
    args = ap.parse_args()

    raw = Path(args.frames).read_bytes()
    if raw[:4] != b"GMV1":
        sys.exit("not a gmeter_sim recording (missing GMV1 header)")
    gz = gzip.compress(raw, compresslevel=9)
    b64 = base64.b64encode(gz).decode("ascii")
    template = Path(args.template).read_text(encoding="utf-8")
    if PLACEHOLDER not in template:
        sys.exit(f"template has no {PLACEHOLDER} placeholder")
    html = template.replace(PLACEHOLDER, b64)
    size_mb = len(html.encode("utf-8")) / 1e6
    if size_mb > args.max_mb:
        sys.exit(f"page would be {size_mb:.1f} MB (> {args.max_mb} MB); record fewer frames or a lower --fps")
    Path(args.output).write_text(html, encoding="utf-8")
    print(f"wrote {args.output}: frames {len(raw)/1e6:.1f} MB -> gzip {len(gz)/1e6:.2f} MB -> page {size_mb:.2f} MB")


if __name__ == "__main__":
    main()
