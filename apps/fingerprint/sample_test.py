#!/usr/bin/env python3
"""One enrollment frame with the vendor message header fields filled."""
import fcntl, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flow

g = os.open(flow.GF_DEV, os.O_RDWR)
fcntl.ioctl(g, flow.GF_IOC_ENABLE_POWER, 0)
fcntl.ioctl(g, flow.GF_IOC_RESET, 0)
time.sleep(0.02)
try:
    fcntl.ioctl(g, flow.GF_IOC_ENABLE_IRQ, 0)
except OSError:
    pass
hp = flow.find_hbm_path(); flow.set_hbm(hp, 2)
fd = os.open(flow.FP_DEV, os.O_RDWR)


def send(name, cmd, ln, payload):
    try:
        st, d = flow.fp_send(fd, cmd, ln, payload,
                             0xF000 + ln, 0xF000 + ln)
        rnz = sum(1 for x in flow.last_resp if x)
        nz = [i for i, x in enumerate(d) if x]
        print(name, hex(cmd), "st", st, "len", len(d), "nz", len(nz),
              "resp10", flow.last_resp[0x10:0x18].hex())
        print("  data head", d[:0x40].hex())
        print("  data nz first", nz[:30], "last", nz[-5:])
        open("/tmp/%s.bin" % name, "wb").write(d)
        return st
    except OSError as e:
        print(name, hex(cmd), "ioctl", e)
        return None


try:
    print("cfg", flow.fp_send(fd, 0x834, 0xB0, flow.config, 0, 0xB0)[0])
    for _ in range(4):
        st, _, _ = flow.fp_send_dcs(fd)
    print("dcs", st)
    print("init", flow.fp_send(fd, 0x7D0, 0x1C0, b"", 0xF1C0, 0x1C0)[0])
    print("pre", flow.fp_send(fd, 0x7D5, 0x80, b"", 0xF080, 0x80)[0])
    print("enroll", flow.fp_send(fd, 0x7D6, 0xC8, flow.hat,
                                 0xF0C8, 0xC8)[0])
    print("HOLD FINGER ON THE RED RING (right edge, y~361)")
    sys.stdout.flush()
    time.sleep(int(os.environ.get("HOLD_SECONDS", "10")))
    send("down", 0x7D7, 0x90, flow.p7)
    time.sleep(0.1)
    send("capture", 0x7D4, 0xB8, flow.p4)
    time.sleep(0.1)
    send("feature", 0x7D3, 0xA0, flow.p3)
    send("enroll_image", 0x7D8, 0xB0, flow.p8)
finally:
    os.close(fd); os.close(g); flow.set_hbm(hp, 0)
