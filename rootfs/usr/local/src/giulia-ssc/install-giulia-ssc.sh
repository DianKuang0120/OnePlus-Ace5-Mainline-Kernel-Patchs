#!/bin/bash
# Install the giulia SSC (FastRPC) sensor support from this repo's rootfs overlay.
# Run as root on the MAINLINE kernel (the ADSP / /dev/fastrpc-adsp node must exist).
set -euo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd)
overlay=$(cd -- "$here/../../../.." && pwd)   # .../rootfs

for f in usr/local/libexec/giulia-ssc/hexagonrpcd \
         usr/local/lib/giulia-ssc/libhexagonrpc.so.0.5 \
         usr/lib/systemd/system/giulia-ssc.service \
         usr/lib/udev/rules.d/90-giulia-ssc.rules \
         usr/lib/systemd/system/iio-sensor-proxy.service.d/giulia-ssc.conf; do
	[ -f "$overlay/$f" ] || { echo "missing overlay file: $f" >&2; exit 1; }
done

install -D -m 0755 "$overlay/usr/local/libexec/giulia-ssc/hexagonrpcd" \
	/usr/local/libexec/giulia-ssc/hexagonrpcd
install -D -m 0755 "$overlay/usr/local/lib/giulia-ssc/libhexagonrpc.so.0.5" \
	/usr/local/lib/giulia-ssc/libhexagonrpc.so.0.5
install -D -m 0644 "$overlay/usr/lib/systemd/system/giulia-ssc.service" \
	/usr/lib/systemd/system/giulia-ssc.service
install -D -m 0644 "$overlay/usr/lib/udev/rules.d/90-giulia-ssc.rules" \
	/usr/lib/udev/rules.d/90-giulia-ssc.rules
install -D -m 0644 "$overlay/usr/lib/systemd/system/iio-sensor-proxy.service.d/giulia-ssc.conf" \
	/usr/lib/systemd/system/iio-sensor-proxy.service.d/giulia-ssc.conf

state=/var/lib/giulia-ssc/root
if [ -e "$state" ] && [ "${FORCE:-0}" != 1 ]; then
	echo "refusing existing SSC state: $state (set FORCE=1 to overlay)" >&2
	exit 1
fi
mkdir -p "$state"
cp -a --no-preserve=ownership "$overlay/var/lib/giulia-ssc/root/." "$state/"

udevadm control --reload-rules || true
systemctl daemon-reload
systemctl enable --now giulia-ssc.service

echo "installed giulia SSC support"
echo "check:  journalctl -u giulia-ssc -n 80 --no-pager"
echo "        monitor-sensor --all"
