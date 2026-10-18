#!/bin/sh
# Run after booting the kernel/DTB with the front-camera node.
# Android uses CSIPHY4 -> CSID2 -> IFE2 for this sensor; match that.
set -eu
# Never run two camss pipelines at once: overlapping streams have wedged
# the camera hardware hard enough to trigger a watchdog reboot.
CAM_LOCK=/tmp/cam-capture.lock
if ! mkdir "$CAM_LOCK" 2>/dev/null; then
    echo "camera capture already running; aborting" >&2
    exit 1
fi
trap 'rmdir "$CAM_LOCK" 2>/dev/null || true' EXIT INT TERM
if pgrep -x v4l2-ctl >/dev/null 2>&1; then
    echo "v4l2-ctl is already running; aborting" >&2
    exit 1
fi
MEDIA=/dev/media0
SENSOR_NAME=$(media-ctl -d "$MEDIA" -p | sed -n 's/^- entity [0-9]*: \(imx480 [^ ]*\).*/\1/p' | head -1)
[ -n "$SENSOR_NAME" ] || { echo "imx480 not found" >&2; exit 1; }
SENSOR="\"$SENSOR_NAME\":0"
SUBDEV=$(media-ctl -d "$MEDIA" -p | awk '/entity .*imx480/{f=1} f && /device node name/{print $NF; exit}')
VIDEO=/dev/video6
OUT=/tmp/front.raw

# Start from a clean single path: CSIPHY4 -> CSID2 -> VFE2/RDI0.
for q in 0 1 2 3 4 5; do
    for c in 0 1 2 3 4; do
        media-ctl -d "$MEDIA" -l "\"msm_csiphy$q\":1->\"msm_csid$c\":0[0]" >/dev/null 2>&1 || true
    done
done
for c in 0 1 2 3 4; do
    for v in 0 1 2 3 4; do
        media-ctl -d "$MEDIA" -l "\"msm_csid$c\":1->\"msm_vfe${v}_rdi0\":0[0]" >/dev/null 2>&1 || true
    done
done

media-ctl -d "$MEDIA" -l '"msm_csiphy4":1->"msm_csid2":0[1]'
media-ctl -d "$MEDIA" -l '"msm_csid2":1->"msm_vfe2_rdi0":0[1]'

for pad in "$SENSOR" '"msm_csiphy4":0' '"msm_csiphy4":1' \
           '"msm_csid2":0' '"msm_csid2":1' '"msm_vfe2_rdi0":0'; do
    media-ctl -d "$MEDIA" -V "${pad}[fmt:SRGGB10_1X10/2304x1728]" >/dev/null
done

# Live image.  Use test_pattern=2 for the sensor colour-bar pattern.
v4l2-ctl -d "$SUBDEV" --set-ctrl test_pattern=0 >/dev/null

v4l2-ctl -d "$VIDEO" --set-fmt-video=width=2304,height=1728,pixelformat=pRAA >/dev/null
rm -f "$OUT"
if timeout 15 v4l2-ctl -d "$VIDEO" --stream-mmap=4 --stream-count=2 \
        --stream-to="$OUT"; then
    ls -lh "$OUT"
else
    echo "capture timed out (no CSI frames)" >&2
fi

dmesg | tail -20
