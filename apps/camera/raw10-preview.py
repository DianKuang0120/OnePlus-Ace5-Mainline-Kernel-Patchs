#!/usr/bin/env python3
"""Unpack a packed 10-bit RAW frame into a PNG preview.

Usage:
    raw10-preview.py [raw] [png] [width] [height]

The defaults match main-capture.sh (4096x3072); for the 2 MP macro use
    raw10-preview.py /tmp/macro.raw /tmp/macro_preview.png 1600 1200
"""
import sys
import numpy as np

WIDTH = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
HEIGHT = int(sys.argv[4]) if len(sys.argv) > 4 else 3072
FRAME_BYTES = WIDTH * HEIGHT * 10 // 8


def unpack(frame, width, height):
    b = frame.reshape(-1, 5)
    p = np.empty((b.shape[0], 4), dtype=np.uint16)
    p[:, 0] = (b[:, 0].astype(np.uint16) << 2) | (b[:, 4] & 3)
    p[:, 1] = (b[:, 1].astype(np.uint16) << 2) | ((b[:, 4] >> 2) & 3)
    p[:, 2] = (b[:, 2].astype(np.uint16) << 2) | ((b[:, 4] >> 4) & 3)
    p[:, 3] = (b[:, 3].astype(np.uint16) << 2) | ((b[:, 4] >> 6) & 3)
    return p.reshape(height, width)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/main.raw"
    dst = sys.argv[2] if len(sys.argv) > 2 else "/tmp/main_preview.png"

    data = np.fromfile(src, dtype=np.uint8)
    if data.size < FRAME_BYTES:
        sys.exit("short file: %d bytes" % data.size)

    img = unpack(data[:FRAME_BYTES], WIDTH, HEIGHT)
    # Green channel is enough for a preview.
    g = img[1::2, 1::2].astype(np.float32)
    lo, hi = np.percentile(g, 0.1), np.percentile(g, 99.9)
    median, mean = np.median(g), g.mean()
    if hi <= lo:
        hi = lo + 1
    g = np.clip((g - lo) * 255.0 / (hi - lo), 0, 255).astype(np.uint8)

    try:
        from PIL import Image
        Image.fromarray(g, "L").resize((512, 384)).save(dst)
    except ImportError:
        dst = dst.rsplit(".", 1)[0] + ".pgm"
        with open(dst, "wb") as fh:
            small = g[::8, ::8]
            fh.write(b"P5\n%d %d\n255\n" % (small.shape[1], small.shape[0]))
            fh.write(small.tobytes())

    print("median %.1f, 0.1%% %.1f, 99.9%% %.1f, mean %.1f"
          % (median, lo, hi, mean))
    print("wrote", dst)


if __name__ == "__main__":
    main()
