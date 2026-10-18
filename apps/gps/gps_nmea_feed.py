#!/usr/bin/env python3
"""Feed QMI LOC position reports to Geoclue as NMEA over a UNIX socket."""
import datetime
import os
import re
import socket
import subprocess
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SOCKET = "/run/giulia-gps.sock"
QMI = ["qmicli", "-d", "qrtr://0", "--loc-follow-position-report"]
ENV = dict(os.environ, LD_PRELOAD=os.path.join(HERE, "locmaster.so"))

clients = []
lock = threading.Lock()


def checksum(body):
    cs = 0
    for c in body:
        cs ^= ord(c)
    return f"{cs:02X}"


def nmea(body):
    return f"${body}*{checksum(body)}\r\n"


def ddmm(v, hemi):
    a = abs(v)
    d = int(a)
    m = (a - d) * 60.0
    return f"{d:02d}{m:07.4f}", hemi


def make_sentences(lat, lon, alt, hdop, nsat, utc_ms):
    dt = datetime.datetime.fromtimestamp(utc_ms / 1000.0,
                                         datetime.timezone.utc)
    tm = dt.strftime("%H%M%S.") + f"{dt.microsecond // 10000:02d}"
    date = dt.strftime("%d%m%y")
    la, ns = ddmm(lat, "N" if lat >= 0 else "S")
    lo, ew = ddmm(lon, "E" if lon >= 0 else "W")
    gga = (f"GPGGA,{tm},{la},{ns},{lo},{ew},1,{nsat:02d},"
           f"{hdop:.1f},{alt:.1f},M,0.0,M,,")
    rmc = (f"GPRMC,{tm},A,{la},{ns},{lo},{ew},0.0,0.0,{date},,,A")
    return nmea(gga) + nmea(rmc)


def broadcast(data):
    if isinstance(data, str):
        print("NMEA", data.replace("\r\n", " | "), flush=True)
        data = data.encode()
    with lock:
        for c in list(clients):
            try:
                c.sendall(data)
            except OSError:
                try:
                    c.close()
                except OSError:
                    pass
                clients.remove(c)


def accept_loop():
    try:
        os.unlink(SOCKET)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCKET)
    os.chmod(SOCKET, 0o666)
    srv.listen(4)
    print("listening", SOCKET, flush=True)
    while True:
        c, _ = srv.accept()
        with lock:
            clients.append(c)
        print("client connected", flush=True)


def parse_block(block):
    if "status: success" not in block:
        return None
    def num(name):
        m = re.search(rf"^\s*{name}:\s*([-0-9.]+)", block, re.M)
        return float(m.group(1)) if m else None
    lat = num("latitude")
    lon = num("longitude")
    alt = num("altitude w.r.t. mean sea level")
    hdop = num("horizontal DOP")
    utc = re.search(r"^\s*UTC timestamp:\s*(\d+)", block, re.M)
    sats = re.search(r"^\s*Satellites used:\s*(.+)$", block, re.M)
    if lat is None or lon is None or alt is None or utc is None:
        return None
    nsat = len(sats.group(1).split(",")) if sats else 0
    return make_sentences(lat, lon, alt, hdop or 0.0, nsat,
                          int(utc.group(1)))


def follow():
    while True:
        print("starting qmicli", flush=True)
        p = subprocess.Popen(QMI, env=ENV, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True,
                             bufsize=1)
        block = []
        assert p.stdout
        for line in p.stdout:
            print(line, end="", flush=True)
            if line.startswith("[position report]"):
                if block:
                    data = parse_block("\n".join(block))
                    if data:
                        broadcast(data)
                block = [line]
            else:
                block.append(line)
        data = parse_block("\n".join(block)) if block else None
        if data:
            broadcast(data)
        p.wait()
        print("qmicli exited", p.returncode, flush=True)
        time.sleep(2)


if __name__ == "__main__":
    threading.Thread(target=accept_loop, daemon=True).start()
    follow()
