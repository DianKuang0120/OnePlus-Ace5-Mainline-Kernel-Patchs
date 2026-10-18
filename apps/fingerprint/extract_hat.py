#!/usr/bin/env python3
"""
Extract a 0x7d6 ENROLL payload from a captured Android fps_hal log.

The output is the hex string accepted by:
    fpctl -p -x enroll "$(python3 extract_hat.py captures/android-fps-enroll.log)"

The HAT token is device-specific and may be time-limited; treat it as
sensitive biometric/token material.
"""
import re
import sys


def main(path, index):
    lines = open(path, "r", errors="replace").read().splitlines()
    found = 0
    last = None
    i = 0
    while i < len(lines):
        m = re.search(
            r"\[HAL send \+0x77fbc\] len=(\d+) type=\d+ cmd=0x([0-9a-f]+)",
            lines[i],
        )
        if not m:
            i += 1
            continue
        length = int(m.group(1))
        cmd = int(m.group(2), 16)
        i += 1
        data = []
        while i < len(lines) and len(data) < length:
            bs = re.findall(r"\b[0-9a-f]{2}\b", lines[i])
            if len(bs) == 16:
                data += [int(x, 16) for x in bs]
            i += 1
        if cmd == 0x7D6 and len(data) >= length:
            if found == index:
                print(bytes(data[0x30:length]).hex())
                return
            found += 1

    sys.exit(f"no 0x7d6 payload at index {index}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: extract_hat.py <android-fps-enroll.log> [index]")
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 0)
