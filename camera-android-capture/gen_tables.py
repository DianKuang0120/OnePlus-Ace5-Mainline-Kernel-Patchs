#!/usr/bin/env python3
"""Turn decoded3.txt (from decode_cci_words.py) into imx906_tables.h."""
import sys

w = []
for line in open(sys.argv[1]):
    a, d = line.split()
    w.append((int(a, 16), int(d, 16)))

# structure (see FINDINGS.md):
#   0..58     OPLUS FTM table (59 regs)
#   59        0x31a7 = 0x06
#   60        0x0106 = 0x01
#   61..3130  QSC calibration blob (3072 bytes @ 0x1000, from the module
#             EEPROM) - device specific, deliberately NOT written by mainline
#   3131..    mode configuration (HMAX/VMAX/crop/PLL/...)
#   ...       stream-on block ending in 0x0100 = 1
IDX_CONFIG = 3131
IDX_STREAM = 3629          # 0x0303=0x60 .. 0x0100=0x01
IDX_END = 3651             # inclusive end of the stream-on block

init = w[:61] + w[IDX_CONFIG:IDX_STREAM]
start = w[IDX_STREAM:IDX_END]

assert start[-1][0] == 0x0100 and start[-1][1] == 1, start[-1:]

# ---------------------------------------------------------------------------
# Mainline camss only supports D-PHY (camss_parse_endpoint() rejects C-PHY),
# while OPLUS drives the IMX906 as C-PHY.  Sony's CSI_SIGNALING_MODE register
# 0x0111 selects the PHY (the in-flight IMX681 driver documents 0x02 = D-PHY),
# and 0x0114 holds the lane count - 1 (IMX355, a 4-lane D-PHY part, uses 0x03;
# the OPLUS IMX906 config uses 0x02 = 3 C-PHY trios).
#
# So switch the part to 4-lane D-PHY for mainline.  Everything else (PLL, crop,
# timing) is left exactly as the vendor set it; the link frequency is tunable
# at load time (modprobe imx906 link_freq=<Hz>).
# ---------------------------------------------------------------------------
OVERRIDES = {
    0x0111: 0x02,   # CSI_SIGNALING_MODE: D-PHY
    0x0114: 0x03,   # CSI-2 lane count - 1: 4 lanes
}

init = [(a, OVERRIDES.get(a, d)) for a, d in init]
for a, d in OVERRIDES.items():
    assert any(r[0] == a for r in init), f"0x{a:04x} not in table"


def emit(name, regs):
    s = "static const struct imx906_reg %s[] = {\n" % name
    for i, (a, d) in enumerate(regs):
        s += "\t{ 0x%04x, 0x%02x }," % (a, d)
        if i % 4 == 3:
            s += "\n"
    if len(regs) % 4:
        s += "\n"
    return s + "};\n"

hdr = """// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX906 (LYT-700) register tables.
 *
 * Decoded losslessly from the CCI hardware queue on stock Android
 * (see camera-android-capture/: FINDINGS.md, decode_cci_words.py).
 * The QSC calibration blob (3072 bytes written to 0x1000 from the module
 * EEPROM) is intentionally omitted - it is per-unit calibration, not init.
 */
#ifndef __IMX906_TABLES_H__
#define __IMX906_TABLES_H__

struct imx906_reg { u16 addr; u8 val; };

"""
hdr += emit("imx906_init_regs", init)
hdr += "\n" + emit("imx906_start_regs", start)
hdr += "\n#endif\n"

open(sys.argv[2], "w").write(hdr)
print(f"init: {len(init)} writes, start: {len(start)} writes -> {sys.argv[2]}")
