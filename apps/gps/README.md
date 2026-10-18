# Qualcomm QMI LOC + Geoclue bridge

The stock QMI LOC service needs a **master client** registration before it
will accept `LOC_START` or event registration.  `locmaster.c` is an
`LD_PRELOAD` shim for qmicli that performs that registration and also sets the
LOC client type/string ID.

## Build / install

```sh
make
sudo ./install.sh
```

`install.sh` installs:

```text
/usr/local/lib/giulia-gps/gps_nmea_feed.py
/usr/local/lib/giulia-gps/locmaster.so
/etc/systemd/system/giulia-gps.service
/etc/geoclue/geoclue.conf  (nmea-socket=/run/giulia-gps.sock)
```

The service creates `/run/giulia-gps.sock`, runs `qmicli` with the shim, and
feeds GGA/RMC NMEA to Geoclue.

## Manual run

```sh
make
LD_PRELOAD=$PWD/locmaster.so qmicli -d qrtr://0 --loc-start
LD_PRELOAD=$PWD/locmaster.so qmicli -d qrtr://0 --loc-follow-position-report
```

## Protocol details

```text
QMI service LOC: 0x10
master request:  0x00BA
TLV:             0x01, uint32 little-endian
key:             0xBAABCDEF

REGISTER_EVENTS:
  client type      = AFW
  client string id = qmicli
```

## Status

A real satellite fix was obtained at a window:

```text
status: success
technology: satellite
uncertainty: ~9.27 m
DOP: position 0.8 / horizontal 0.5 / vertical 0.6
satellites: 36 used
time source: navigation-solution
```

Geoclue receives `GPS GGA+RMC` through `/run/giulia-gps.sock`; KDE/Qt
location can use it.  Geoclue currently reports accuracy `0` for this NMEA
source even though GGA carries HDOP; position/altitude/time are valid.

Start/stop on demand to save power:

```sh
sudo systemctl start giulia-gps
sudo systemctl stop giulia-gps
# sudo systemctl enable giulia-gps   # optional boot autostart
```

Source for the QMI LOC master request/TLV/key: Qualcomm
`location-apis-qcom`, `loc_api/loc_api_v02/location_service_v02.c` and
`LocApiV02.cpp`.
