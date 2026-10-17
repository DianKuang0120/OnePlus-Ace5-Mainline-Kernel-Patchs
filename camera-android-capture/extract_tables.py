#!/usr/bin/env python3
# Extract the IMX906 register tables from the stock Android camera module.
#
# The vendor camera kernel (camera.ko) embeds the per-sensor register tables as
# arrays of  struct cam_sensor_i2c_reg_array { u32/u16 addr, data, delay, mask
# [, u16 operation] }.  We locate them by matching the known first entries of
# the factory-test table (.imx906_setting, 59 registers, taken verbatim from
# the OPLUS source tree), then dump everything that follows.
#
# Usage:
#   ./extract_tables.py /path/to/camera.ko [--dump-start 0x... ]

import struct, sys, argparse

# First entries of .imx906_setting (vendor source, authoritative)
FTM_HEAD = [
    (0x0136, 0x13), (0x0137, 0x33), (0x3304, 0x00), (0x33F0, 0x04),
    (0x33F1, 0x06), (0x0111, 0x03), (0x4815, 0x0F), (0x614D, 0x00),
    (0x614F, 0x00), (0x6188, 0x09), (0x6189, 0x09), (0x6190, 0x09),
]

LAYOUTS = {
    "u16x4":      lambda a, d: struct.pack("<HHHH", a, d, 0, 0),
    "u32x4":      lambda a, d: struct.pack("<IIII", a, d, 0, 0),
    "u16x2":      lambda a, d: struct.pack("<HH", a, d),
    "u16x4+op":   lambda a, d: struct.pack("<HHHHH", a, d, 0, 0, 0),
    "u16x4+op_p": lambda a, d: struct.pack("<HHHHH", a, d, 0, 0, 0) + b"\x00\x00\x00",
    "u32x4+op":   lambda a, d: struct.pack("<IIIII", a, d, 0, 0, 0),
}


def find_patterns(data):
    hits = []
    for name, fn in LAYOUTS.items():
        for n in (4, 6, 8, 12):
            pat = b"".join(fn(a, d) for a, d in FTM_HEAD[:n])
            off = 0
            while True:
                off = data.find(pat, off)
                if off < 0:
                    break
                hits.append((off, name, n))
                off += 1
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko")
    ap.add_argument("--entries", type=int, default=600)
    args = ap.parse_args()

    data = open(args.ko, "rb").read()
    print(f"{args.ko}: {len(data)} bytes")
    hits = find_patterns(data)
    if not hits:
        print("no table found (module might be compressed or layout unknown)")
        return 1

    for off, name, n in hits:
        print(f"  hit @ 0x{off:x}  layout={name} matched={n} entries")

    # prefer the longest match
    off, name, n = max(hits, key=lambda h: h[2])
    fn = LAYOUTS[name]
    stride = len(fn(0, 0))
    print(f"\n== dumping with layout {name} (stride {stride}) from 0x{off:x} ==")
    i = 0
    while i < args.entries and off + stride <= len(data):
        rec = data[off:off + stride]
        if name.startswith("u32"):
            a, d = struct.unpack("<II", rec[:8])
        else:
            a, d = struct.unpack("<HH", rec[:4])
        if a > 0xFFFF or d > 0xFFFFFFFF:
            break
        print(f"{i:4d}  0x{a:04x} 0x{d:04x}")
        off += stride
        i += 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
