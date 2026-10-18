#!/usr/bin/env python3
"""
Single-process Jiiov enrollment driver for /dev/qcomtee_fp.

Uses the captured Android payloads (HAT, enroll-down, capture, feature,
enroll-image) and keeps the qcomtee file descriptor and TA session open for
the whole flow.

Run as root after loading the qcomtee module:

    python3 flow.py

The sensor is under the display, bottom-center (~x=632, y=2400 on 1264x2780).
"""
import ctypes
import fcntl
import glob
import os
import struct
import sys
import time

FP_DEV = "/dev/qcomtee_fp"
GF_DEV = "/dev/goodix_fp"
GF_IOC_RESET = 0x6702  # _IO('g', 2)
GF_IOC_ENABLE_IRQ = 0x6703  # _IO('g', 3)
GF_IOC_ENABLE_POWER = 0x6707  # _IO('g', 7)
FP_IOCTL_SEND_DATA = 0xC0304611  # _IOWR('F', 0x11, 48-byte struct)
FP_IOCTL_SEND = 0xC0184610      # _IOWR('F', 0x10, 24-byte struct)

CAP_DIR = os.path.join(os.path.dirname(__file__), "captures")
STARTUP = os.path.join(CAP_DIR, "android-fps-hal-startup.log")
ENROLL = os.path.join(CAP_DIR, "android-fps-enroll.log")

FP_TYPE = 2
FP_HDR = 0x30


def parse_blocks(path):
    lines = open(path, errors="replace").read().splitlines()
    blocks = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if "[HAL send +0x77fbc]" not in line:
            i += 1
            continue
        try:
            ln = int(line.split("len=")[1].split()[0])
            cmd = int(line.split("cmd=0x")[1].split()[0], 16)
        except (IndexError, ValueError):
            i += 1
            continue
        i += 1
        data = []
        while i < len(lines) and len(data) < ln:
            bs = [x for x in lines[i].replace(":", " ").split()
                  if len(x) == 2 and all(c in "0123456789abcdef" for c in x)]
            if len(bs) == 16:
                data += [int(x, 16) for x in bs]
            i += 1
        if len(data) >= ln:
            blocks.append((cmd, bytes(data[:ln])))
    return blocks


def payloads(path, cmd, limit=1):
    out = []
    for c, data in parse_blocks(path):
        if c == cmd:
            out.append(data[FP_HDR:])
            if len(out) >= limit:
                break
    return out


startup = parse_blocks(STARTUP)
enroll = parse_blocks(ENROLL)
config = next(d for c, d in startup if c == 0x834)[FP_HDR:]
p835 = next(d[FP_HDR:] for c, d in startup if c == 0x835)
hats = [d[FP_HDR:] for c, d in enroll if c == 0x7d6]
p7 = next(d[FP_HDR:] for c, d in enroll if c == 0x7d7)
p4 = next(d[FP_HDR:] for c, d in enroll if c == 0x7d4)
p3 = next(d[FP_HDR:] for c, d in enroll if c == 0x7d3)
p8 = next(d[FP_HDR:] for c, d in enroll if c == 0x7d8)
p9 = next(d[FP_HDR:] for c, d in enroll if c == 0x7d9)
pda = next(d[FP_HDR:] for c, d in enroll if c == 0x7da)
pe2 = next(d[FP_HDR:] for c, d in enroll if c == 0x7e2)
pe6 = next(d[FP_HDR:] for c, d in enroll if c == 0x7e6)
p836 = next(d[FP_HDR:] for c, d in enroll if c == 0x836)

hat_index = int(os.environ.get("HAT_INDEX", "0"))
hat = hats[hat_index]

sample_big = os.environ.get("SAMPLE_BIG", "0") == "1"
capture_big = os.environ.get("CAPTURE_BIG", "1") == "1"
capture_qsee_small = os.environ.get("CAPTURE_QSEE_SMALL", "0") == "1"
last_resp = b""


def fp_send(fd, cmd, total_len, payload=b"", qsee_len=0, cap=0):
    global last_resp
    if not cap:
        cap = total_len
    if cap < total_len:
        cap = total_len
    data = bytearray(cap)
    data[0:4] = struct.pack("<I", FP_TYPE)
    data[4:8] = struct.pack("<I", cmd)
    # The vendor HAL's sendMessage() fills these fields before every command.
    data[0x10:0x18] = struct.pack("<Q", time.monotonic_ns())
    data[0x18:0x1C] = struct.pack("<I", total_len)
    data[0x1C:0x20] = struct.pack("<I", cap - total_len)
    data[FP_HDR:FP_HDR + len(payload)] = payload

    req = bytearray(64)
    req[0:4] = struct.pack("<I", qsee_len if qsee_len else total_len)
    resp = bytearray(1024)

    data_buf = (ctypes.c_char * cap).from_buffer(data)
    req_buf = (ctypes.c_char * 64).from_buffer(req)
    resp_buf = (ctypes.c_char * len(resp)).from_buffer(resp)
    arg = bytearray(struct.pack("<QIIQQIIII",
                                ctypes.addressof(req_buf), 64, len(resp),
                                ctypes.addressof(resp_buf),
                                ctypes.addressof(data_buf), total_len, 0,
                                qsee_len, cap))
    fcntl.ioctl(fd, FP_IOCTL_SEND_DATA, arg)
    a = struct.unpack("<QIIQQIIII", arg)
    out_len = a[6] if a[6] else total_len
    resp_len = min(a[2], len(resp))
    last_resp = bytes(resp[:resp_len])
    status = struct.unpack_from("<I", data, 8)[0]
    return status, bytes(data[:out_len])


def fp_send_simple(fd, msg, qsee_len=0, extra_len=0, resp_len=2048):
    """QSEECom_send_cmd shape: the message itself is the request buffer."""
    req = bytearray(msg)
    if len(req) >= 0x20:
        req[0x18:0x1C] = struct.pack("<I", qsee_len or len(req))
        req[0x1C:0x20] = struct.pack("<I", extra_len)
    resp = bytearray(resp_len)
    req_buf = (ctypes.c_char * len(req)).from_buffer(req)
    resp_buf = (ctypes.c_char * resp_len).from_buffer(resp)
    arg = bytearray(struct.pack("<QIIQ",
                                ctypes.addressof(req_buf), len(req),
                                resp_len, ctypes.addressof(resp_buf)))
    fcntl.ioctl(fd, FP_IOCTL_SEND, arg)
    a = struct.unpack("<QIIQ", arg)
    status = struct.unpack_from("<I", req, 8)[0]
    return status, bytes(resp[:min(a[2], resp_len)]), bytes(req)


def fp_send_dcs(fd):
    msg = next(d for c, d in startup if c == 0x835)
    return fp_send_simple(fd, msg)


def find_hbm_path():
    for bl in glob.glob("/sys/class/backlight/*"):
        try:
            real = os.path.realpath(bl)
        except OSError:
            continue
        for dev in (real, os.path.dirname(real)):
            p = os.path.join(dev, "hbm")
            if os.path.exists(p):
                return p
    return None


def set_hbm(path, mode):
    if not path:
        return
    try:
        with open(path, "w") as f:
            f.write(str(mode))
        print("hbm:", mode, path)
    except OSError as e:
        print("hbm write:", e)


def main():
    # Keep the supply on for the whole flow.
    gfd = os.open(GF_DEV, os.O_RDWR)
    try:
        fcntl.ioctl(gfd, GF_IOC_ENABLE_POWER, 0)
    except OSError as e:
        print("power ioctl:", e)
    # open() holds the sensor in reset; the HAL releases it with GF_IOC_RESET.
    try:
        fcntl.ioctl(gfd, GF_IOC_RESET, 0)
        time.sleep(0.02)
        print("goodix reset")
    except OSError as e:
        print("reset ioctl:", e)
    try:
        fcntl.ioctl(gfd, GF_IOC_ENABLE_IRQ, 0)
    except OSError as e:
        print("irq ioctl:", e)
    time.sleep(0.05)

    hbm_path = None
    if os.environ.get("HBM", "1") != "0":
        hbm_path = find_hbm_path()
        set_hbm(hbm_path, int(os.environ.get("HBM_MODE", "2")))

    fd = os.open(FP_DEV, os.O_RDWR)
    try:
        st, _ = fp_send(fd, 0x834, 0xB0, config, 0, 0xB0)
        print("init-ta-config:", st)
        # The HAL sends 0x835 through the simple QSEECom_send_cmd path, not
        # send_modified_cmd, and repeats it before INIT.
        for _ in range(4):
            st, _, _ = fp_send_dcs(fd)
        print("dcs-config:", st)
        st, _ = fp_send(fd, 0x7D0, 0x1C0, b"", 0xF000 + 0x1C0, 0x1C0)
        print("init:", st)
        st, _ = fp_send(fd, 0x7D5, 0x80, b"", 0xF000 + 0x80, 0x80)
        print("pre-enroll:", st)
        st, _ = fp_send(fd, 0x7D6, 0xC8, hat, 0xF000 + 0xC8, 0xC8)
        print("enroll:", st)
        if st:
            return

        print("HOLD FINGER ON SENSOR (landscape: right long edge, y~361)")
        sys.stdout.flush()
        time.sleep(int(os.environ.get("HOLD_SECONDS", "8")))
        samples = int(os.environ.get("SAMPLES", "1"))
        # The captured Android capture payload carries the vendor uptime in
        # milliseconds at payload+0x1c.  Refresh it instead of replaying a
        # timestamp from the old Android boot.
        p4m = bytearray(p4)
        if len(p4m) >= 32:
            struct.pack_into("<I", p4m, 28,
                             int(time.monotonic() * 1000) & 0xFFFFFFFF)
        cap_qsee = 0xB8 if capture_qsee_small else 0xF000 + 0xB8
        last_feature = b""
        last_enroll = b""
        for n in range(samples):
            st, _ = fp_send(fd, 0x7D7, 0x90, p7,
                            0xF000 + 0x90, 0xF090)
            time.sleep(0.1)
            st_cap, cap_data = fp_send(fd, 0x7D4, 0xB8, p4m,
                                       0xF000 + 0xB8, 0xF0B8)
            time.sleep(0.1)
            st_feat, last_feature = fp_send(fd, 0x7D3, 0xA0, p3,
                                            0xF000 + 0xA0, 0xF0A0)
            st_img, last_enroll = fp_send(fd, 0x7D8, 0xB0, p8,
                                          0xF000 + 0xB0, 0xF0B0)
            st_dcs, _ = fp_send(fd, 0x7E6, 0x238, pe6,
                                0xF000 + 0x238, 0x238)
            st_log, _ = fp_send(fd, 0x836, 0x30, p836,
                                0xF000 + 0x30, 0x30)
            print("sample %2d: down=%d cap=%d(%d) feat=%d(%d) img=%d(%d) dcs=%d log=%d" %
                  (n, st, st_cap, sum(1 for x in cap_data if x),
                   st_feat, sum(1 for x in last_feature if x),
                   st_img, sum(1 for x in last_enroll if x),
                   st_dcs, st_log))
            if n == 0:
                open("/tmp/capture.bin", "wb").write(cap_data)
            time.sleep(0.25)
        open("/tmp/feature.bin", "wb").write(last_feature)
        open("/tmp/enroll_image.bin", "wb").write(last_enroll)
        st, _ = fp_send(fd, 0x7D9, 0x80, p9, 0xF000 + 0x80, 0x80)
        print("save-template:", st)
        st, _ = fp_send(fd, 0x7DA, 0x78, pda, 0xF000 + 0x78, 0x78)
        print("post-enroll:", st)
        st, enum_data = fp_send(fd, 0x7E2, 0x98, pe2,
                                0xF000 + 0x98, 0x98)
        print("enumerate:", st, "len", len(enum_data),
              "nonzero", sum(1 for x in enum_data if x))
        print("enumerate tail:", enum_data[0x30:0xd0].hex())
    finally:
        os.close(fd)
        os.close(gfd)
        set_hbm(hbm_path, 0)


if __name__ == "__main__":
    main()
