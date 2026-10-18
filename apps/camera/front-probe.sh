#!/bin/sh
set -eu
printf '== modprobe ==\n'
modprobe imx471 || true
printf '\n== dmesg ==\n'
dmesg | grep -i -E 'imx471|imx480|csiphy4|cci' | tail -40 || true
printf '\n== media graph ==\n'
media-ctl -d /dev/media0 -p 2>/dev/null | sed -n '1,180p' || true
printf '\n== video nodes ==\n'
v4l2-ctl --list-devices 2>/dev/null || true
printf '\n== subdev ctrls ==\n'
for d in /dev/v4l-subdev*; do
    [ -e "$d" ] || continue
    echo "--- $d"
    v4l2-ctl -d "$d" --list-ctrls 2>/dev/null | head -20 || true
done
