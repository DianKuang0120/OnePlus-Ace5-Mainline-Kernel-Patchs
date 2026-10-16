#!/usr/bin/env python3
"""
Decode the OPLUS/QCOM CCI queue words into a *lossless* register-write list.

With `camera.debug_mdl` bit 14 (CAM_CCI) set, `cam_cci_data_queue()` logs:

    ...: 1248: CCI0_I2C_M1_Q0 addr type 2 data type 1 cmd_size <N>   # new call
    ...: 1438: free_size <f>, en_seq_write <E> i: <I> len: <L>       # a buffer
    ...: 1447: CCI0_I2C_M1_Q0 LOAD_DATA_ADDR <word>, len:<L>, cnt:<n>  (L lines)

Each buffer is I bytes, little-endian packed into L words:

    first buffer of a call:  [cmd][addr_hi][addr_lo][d0][d1]...  dK -> addr+K
    continuation (en_seq=1): [cmd][d0][d1]...                    dK -> last+K

`en_seq_write` is logged *after* the flag is updated, so the first buffer of a
sequential call still carries the address - hence the `cmd_size` line is used to
find call starts.

`cmd & 0xf` == 9 is CCI_I2C_WRITE_CMD.

Usage:  ./decode_cci_words.py dmesg.txt > regs.txt
"""

import re
import sys

RE_CALL = re.compile(
    r"cam_cci_data_queue: \d+: (CCI\d+_I2C_M\d+_Q\d+) "
    r"addr type (\d+) data type (\d+) cmd_size (\d+)")
RE_FREE = re.compile(
    r"cam_cci_data_queue: \d+: free_size (\d+), "
    r"en_seq_write (\d+) i: (\d+) len: (\d+)")
RE_WORD = re.compile(
    r"cam_cci_data_queue: \d+: (CCI\d+_I2C_M\d+_Q\d+) "
    r"LOAD_DATA_ADDR 0x([0-9a-f]+), len:(\d+), cnt: (\d+)")


def main():
    out = []
    pend = None            # [en_seq, i, len, [words]]
    first_in_call = False
    last_addr = 0

    for line in open(sys.argv[1], errors="ignore"):
        if RE_CALL.search(line):
            first_in_call = True
            continue

        m = RE_FREE.search(line)
        if m:
            pend = [int(m.group(2)), int(m.group(3)), int(m.group(4)), []]
            continue

        m = RE_WORD.search(line)
        if m and pend:
            pend[3].append(int(m.group(2), 16))
            if len(pend[3]) < pend[2]:
                continue

            en_seq, i_bytes, nwords, words = pend
            raw = b"".join(w.to_bytes(4, "little") for w in words)[:i_bytes]
            has_addr = first_in_call or en_seq == 0
            first_in_call = False

            if has_addr:
                if len(raw) >= 3:
                    addr = (raw[1] << 8) | raw[2]
                    data = raw[3:]
                    for k, d in enumerate(data):
                        out.append((addr + k, d))
                    if data:
                        last_addr = addr + len(data) - 1
            else:
                data = raw[1:]
                for k, d in enumerate(data):
                    out.append((last_addr + 1 + k, d))
                if data:
                    last_addr += len(data)
            pend = None

    with open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout as fh:
        for a, d in out:
            fh.write(f"0x{a:04x} 0x{d:02x}\n")


if __name__ == "__main__":
    main()
