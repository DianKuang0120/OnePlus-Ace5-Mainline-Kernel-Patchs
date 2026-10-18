#!/usr/bin/env python3
"""Replay the captured Android authentication sequence on mainline qcomtee."""
import fcntl
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flow

AUTH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "captures", "auth")


def load(name):
    d = open(os.path.join(AUTH, name), "rb").read()
    return d, d[flow.FP_HDR:]


def main():
    gfd = os.open(flow.GF_DEV, os.O_RDWR)
    fcntl.ioctl(gfd, flow.GF_IOC_ENABLE_POWER, 0)
    fcntl.ioctl(gfd, flow.GF_IOC_RESET, 0)
    time.sleep(0.02)
    try:
        fcntl.ioctl(gfd, flow.GF_IOC_ENABLE_IRQ, 0)
    except OSError:
        pass

    hp = flow.find_hbm_path()
    flow.set_hbm(hp, 2)

    fd = os.open(flow.FP_DEV, os.O_RDWR)
    try:
        st, _ = flow.fp_send(fd, 0x834, 0xB0, flow.config, 0, 0xB0)
        print("init-ta-config:", st)
        st, _ = flow.fp_send(fd, 0x835, 0x240, flow.p835, 0x240, 0x240)
        print("dcs-config:", st)
        st, _ = flow.fp_send(fd, 0x7D0, 0x1C0, b"", 0xF000 + 0x1C0, 0x1C0)
        print("init:", st)

        for name, cmd, ln in [
            ("0x7e4_79b64.bin", 0x7E4, 0x130),
            ("0x7dc_77fbc.bin", 0x7DC, 0x88),
            ("0x7dd_77fbc.bin", 0x7DD, 0xA0),
        ]:
            d, p = load(name)
            st, _ = flow.fp_send(fd, cmd, ln, p, 0xF000 + ln, ln)
            print("cmd 0x%x:" % cmd, st)

        print("HOLD FINGER ON THE RED RING (right edge, y~361)")
        sys.stdout.flush()
        time.sleep(int(os.environ.get("HOLD_SECONDS", "15")))

        for name, cmd, ln in [
            ("0x7d4_77fbc.bin", 0x7D4, 0xB8),
            ("0x7d3_77fbc.bin", 0x7D3, 0xA0),
            ("0x7de_77fbc.bin", 0x7DE, 0xF0),
            ("0x7df_77fbc.bin", 0x7DF, 0x80),
            ("0x7e6_77fbc.bin", 0x7E6, 0x238),
            ("0x7e0_77fbc.bin", 0x7E0, 0x78),
        ]:
            d, p = load(name)
            # capture/feature output needs the large shared object
            cap = ln
            qsee = 0xF000 + ln
            if cmd in (0x7D4,):
                cap = 0xF000 + ln
                qsee = ln
            st, out = flow.fp_send(fd, cmd, ln, p, qsee, cap)
            nz = sum(1 for x in out if x)
            print("cmd 0x%x:" % cmd, st, "len", len(out), "nz", nz)
            rnz = sum(1 for x in flow.last_resp if x)
            print("  resp nz", rnz, flow.last_resp.hex())
            if cmd in (0x7DF, 0x7DE):
                print("  tail", out[-32:].hex())
                open("/tmp/auth_result_%x.bin" % cmd, "wb").write(out)
    finally:
        os.close(fd)
        os.close(gfd)
        flow.set_hbm(hp, 0)


if __name__ == "__main__":
    main()
