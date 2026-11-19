#!/usr/bin/env python3
"""Batch-rasterize SVG/XPM desktop icons to cached PNGs using GdkPixbuf."""

import os
import sys

try:
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf
except Exception:
    sys.exit(0)


def main() -> int:
    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            continue
        src, dst, sz_str = parts
        try:
            sz = max(16, min(128, int(sz_str)))
            if os.path.exists(dst):
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            pb = GdkPixbuf.Pixbuf.new_from_file_at_size(src, sz, sz)
            pb.savev(dst, "png", [], [])
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
