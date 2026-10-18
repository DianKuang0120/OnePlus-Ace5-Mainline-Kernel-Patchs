#!/bin/sh
# 2MP macro (OmniVision OV02B10), CCI1 master 0, CSIPHY3 -> CSID2 -> VFE2.
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
SENSOR_NAME=$(media-ctl -d "$MEDIA" -p | sed -n 's/^- entity [0-9]*: \(ov02b10 [^ ]*\).*/\1/p' | head -1)
[ -n "$SENSOR_NAME" ] || { echo "ov02b10 not found" >&2; exit 1; }
SENSOR="\"$SENSOR_NAME\":0"
VIDEO=/dev/video6
OUT=/tmp/macro.raw

# Android uses CSIPHY3 -> CSID2 -> IFE2; match that in mainline.
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

media-ctl -d "$MEDIA" -l '"msm_csiphy3":1->"msm_csid2":0[1]'
media-ctl -d "$MEDIA" -l '"msm_csid2":1->"msm_vfe2_rdi0":0[1]'

for pad in "$SENSOR" '"msm_csiphy3":0' '"msm_csiphy3":1' \
           '"msm_csid2":0' '"msm_csid2":1' '"msm_vfe2_rdi0":0'; do
    media-ctl -d "$MEDIA" -V "${pad}[fmt:SBGGR10_1X10/1600x1200]" >/dev/null
done

v4l2-ctl -d "$VIDEO" --set-fmt-video=width=1600,height=1200,pixelformat=pBAA >/dev/null
rm -f "$OUT"
if timeout 15 v4l2-ctl -d "$VIDEO" --stream-mmap=4 --stream-count=2 \
        --stream-to="$OUT"; then
    ls -lh "$OUT"
else
    echo "macro capture timed out (no CSI frames)" >&2
fi

dmesg | tail -20
