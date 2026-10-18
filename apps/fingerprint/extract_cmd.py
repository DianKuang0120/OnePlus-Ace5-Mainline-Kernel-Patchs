#!/usr/bin/env python3
"""
Extract a 0x7d7/0x7d4/0x7d3/0x7d8 payload from an Android fps_hal log.

Usage:
    extract_cmd.py <log> <cmd-hex> [index]

The first matching message is used by default; index selects a later one.
Output is a hex string accepted by fpctl.
"""
import re
import sys


def main(path, cmd, index):
    lines = open(path, "r", errors="replace").read().splitlines()
    found = 0
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
        msg_cmd = int(m.group(2), 16)
        i += 1
        data = []
        while i < len(lines) and len(data) < length:
            bs = re.findall(r"\b[0-9a-f]{2}\b", lines[i])
            if len(bs) == 16:
                data += [int(x, 16) for x in bs]
            i += 1
        if msg_cmd == cmd and len(data) >= length:
            if found == index:
                print(bytes(data[0x30:length]).hex())
                return
            found += 1
    sys.exit(f"no payload for cmd 0x{cmd:x} index {index}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: extract_cmd.py <log> <cmd-hex> [index]")
    main(sys.argv[1], int(sys.argv[2], 0),
         int(sys.argv[3]) if len(sys.argv) > 3 else 0)
