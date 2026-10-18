#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PATCHROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)

make -C "$HERE"

install -d /usr/local/lib/giulia-gps
install -m755 "$HERE/gps_nmea_feed.py" /usr/local/lib/giulia-gps/
install -m644 "$HERE/locmaster.so" /usr/local/lib/giulia-gps/
install -m644 "$PATCHROOT/rootfs/etc/systemd/system/giulia-gps.service" \
	/etc/systemd/system/giulia-gps.service

if [ -f /etc/geoclue/geoclue.conf ] && [ ! -f /etc/geoclue/geoclue.conf.pre-giulia ]; then
	cp -a /etc/geoclue/geoclue.conf /etc/geoclue/geoclue.conf.pre-giulia
fi
install -m644 "$PATCHROOT/rootfs/etc/geoclue/geoclue.conf" \
	/etc/geoclue/geoclue.conf

systemctl daemon-reload
systemctl restart geoclue
systemctl restart giulia-gps 2>/dev/null || systemctl start giulia-gps

echo "giulia-gps installed; stop with: systemctl stop giulia-gps"
