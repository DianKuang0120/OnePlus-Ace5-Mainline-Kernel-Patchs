#!/usr/bin/env python3
"""
giulia-camera - a small phone-style camera app for the giulia ultrawide
(Sony IMX355) running mainline Linux.

Live preview, auto exposure, auto white balance, and full-resolution stills
saved as PNG/JPEG.

Run:  python3 giulia_camera.py
      (or ./giulia-camera from the PATH once installed)

Keys:  Space  take a photo
       Esc    quit
"""

import os
import struct
import subprocess
import sys
import time
import zlib
from datetime import datetime

import numpy as np
from PySide6.QtCore import Qt, QThread, Signal, QSize
from PySide6.QtGui import QImage, QPixmap, QKeySequence, QAction
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                               QHBoxLayout, QPushButton, QLabel, QSlider,
                               QComboBox, QCheckBox, QSizePolicy, QMessageBox)

MEDIA = "/dev/media0"
SENSOR_PREFIX = "imx355"
VIDEO_ENTITY = "msm_vfe0_video0"
# Photo sizes.  The live preview always streams 1920x1080 (binned, ~30 fps,
# much lower readout noise); picking a size here only decides what the shutter
# switches to for the actual still.
MODES = [("8 MP  (3280x2464)", 3280, 2464),
         ("2 MP  (1920x1080)", 1920, 1080)]
PREVIEW_W, PREVIEW_H = 1920, 1080
STILL_BLANK = 16000          # ~0.2 s frames while taking a photo
PREVIEW_EXP_MAX = 2600       # keeps the preview at ~30 fps
EXP_DEFAULT = 320
GAIN_DEFAULT = 120
AE_TARGET = 100.0        # mean RAW level of a well exposed frame (incl. black)
BLACK_LEVEL = 64.0
EXP_MIN, EXP_MAX = 4, 16000
GAIN_MIN, GAIN_MAX = 0, 960
BLACK_LIFT = 0.02         # tiny black point lift, keeps blacks black


# --------------------------------------------------------------------------
# helpers (shared with giulia-photo)
# --------------------------------------------------------------------------

def shell(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def find_nodes():
    p = shell(["media-ctl", "-d", MEDIA, "-p"])
    if p.returncode:
        raise RuntimeError("cannot read %s - is camss loaded?" % MEDIA)
    sensor = subdev = video = None
    cur = None
    for line in p.stdout.splitlines():
        if "entity" in line and ": " in line:
            cur = line.split(": ", 1)[1].split(" (")[0].strip()
        if "device node name" in line and cur:
            node = line.rsplit(None, 1)[-1]
            if cur.startswith(SENSOR_PREFIX):
                sensor, subdev = cur, node
            elif cur.startswith(VIDEO_ENTITY):
                video = node
    if not sensor or not video:
        raise RuntimeError("IMX355 not found in the media graph")
    return sensor, subdev, video


def setup_pipeline(sensor, w, h):
    # CSID0 is shared with the main sensor (csiphy1): drop those links first,
    # otherwise CSIPHY0 -> CSID0 cannot be enabled (EBUSY).
    for q in ("msm_csiphy1", "msm_csiphy2", "msm_csiphy3",
              "msm_csiphy4", "msm_csiphy5"):
        for c in ("msm_csid0", "msm_csid1", "msm_csid2"):
            shell(["media-ctl", "-d", MEDIA, "-l",
                   '"%s":1->"%s":0[0]' % (q, c)])
    shell(["media-ctl", "-d", MEDIA, "-l", '"msm_csiphy0":1->"msm_csid0":0[1]'])
    shell(["media-ctl", "-d", MEDIA, "-l", '"msm_csid0":1->"msm_vfe0_rdi0":0[1]'])
    for pad in ['"%s":0' % sensor, '"msm_csiphy0":0', '"msm_csiphy0":1',
                '"msm_csid0":0', '"msm_csid0":1', '"msm_vfe0_rdi0":0']:
        shell(["media-ctl", "-d", MEDIA, "-V",
               "%s[fmt:SRGGB10_1X10/%dx%d]" % (pad, w, h)])


def ctrl_range(subdev, name, fallback=(1, 0xffff)):
    out = shell(["v4l2-ctl", "-d", subdev, "--list-ctrls"]).stdout
    for line in out.splitlines():
        if line.strip().startswith(name + " "):
            d = dict(tok.split("=", 1) for tok in line.split() if "=" in tok)
            return int(d.get("min", "0")), int(d.get("max", "0"))
    return fallback


def set_blanking(subdev, lines):
    lo, hi = ctrl_range(subdev, "vertical_blanking")
    if not hi:
        return None
    v = max(lo, min(hi, lines))
    set_ctrl(subdev, vertical_blanking=v)
    return v


def set_ctrl(subdev, **vals):
    args = ["v4l2-ctl", "-d", subdev]
    for k, v in vals.items():
        args += ["--set-ctrl", "%s=%d" % (k, v)]
    shell(args)


def unpack_raw10(buf, w, h):
    n = (len(buf) // 5) * 5
    if n < w * h * 10 // 8:
        return None
    d = np.frombuffer(buf[:n], dtype=np.uint8).reshape(-1, 5).astype(np.uint16)
    px = np.empty(d.shape[0] * 4, dtype=np.uint16)
    px[0::4] = (d[:, 0] << 2) | (d[:, 4] & 3)
    px[1::4] = (d[:, 1] << 2) | ((d[:, 4] >> 2) & 3)
    px[2::4] = (d[:, 2] << 2) | ((d[:, 4] >> 4) & 3)
    px[3::4] = (d[:, 3] << 2) | ((d[:, 4] >> 6) & 3)
    return px[:w * h].reshape(h, w)


def get_v4l2_format(video):
    """Return (bytes_per_line, size_image) for the current format."""
    import re as _re
    out = shell(["v4l2-ctl", "-d", video, "--get-fmt-video"]).stdout
    bpl = _re.search(r"Bytes per Line\s*:\s*(\d+)", out)
    size = _re.search(r"Size Image\s*:\s*(\d+)", out)
    return (int(bpl.group(1)) if bpl else 0,
            int(size.group(1)) if size else 0)


def unpack_frame(buf, w, h, bpl):
    """Unpack a (possibly padded) 10-bit Bayer packed frame."""
    stride_bytes = w * 10 // 8
    if bpl < stride_bytes:
        bpl = stride_bytes
    if len(buf) < bpl * h:
        return None
    a = np.frombuffer(buf[:bpl * h], dtype=np.uint8).reshape(h, bpl)[:, :stride_bytes]
    n5 = stride_bytes // 5
    if n5 * 5 != stride_bytes:
        return None
    d = a[:, :n5 * 5].reshape(h, n5, 5).astype(np.uint16)
    px = np.empty((h, n5 * 4), dtype=np.uint16)
    px[:, 0::4] = (d[:, :, 0] << 2) | (d[:, :, 4] & 3)
    px[:, 1::4] = (d[:, :, 1] << 2) | ((d[:, :, 4] >> 2) & 3)
    px[:, 2::4] = (d[:, :, 2] << 2) | ((d[:, :, 4] >> 4) & 3)
    px[:, 3::4] = (d[:, :, 3] << 2) | ((d[:, :, 4] >> 6) & 3)
    return px[:, :w]


def quick_preview(px, w, h, scale=4):
    """Half-resolution RGB straight from the four Bayer phases (fast)."""
    # black must go before any per-channel gain, otherwise every channel
    # keeps a different offset (64*(gain-1)) and the shadows turn magenta
    r = px[0::2, 0::2].astype(np.float32) - BLACK_LEVEL
    g1 = px[0::2, 1::2].astype(np.float32) - BLACK_LEVEL
    g2 = px[1::2, 0::2].astype(np.float32) - BLACK_LEVEL
    b = px[1::2, 1::2].astype(np.float32) - BLACK_LEVEL
    g = 0.5 * (g1 + g2)
    # box-reduce by (scale/2)
    k = max(1, scale // 2)
    if k > 1:
        def red(a):
            hh = (a.shape[0] // k) * k
            ww = (a.shape[1] // k) * k
            return a[:hh, :ww].reshape(hh // k, k, ww // k, k).mean(axis=(1, 3))
        r, g, b = red(r), red(g), red(b)
    return np.clip(np.stack([r, g, b], -1), 0, None)


def box_fill(values, mask, h, w):
    p = np.pad(values, 1)
    pm = np.pad(mask.astype(np.float32), 1)
    s = np.zeros((h, w), dtype=np.float32)
    c = np.zeros((h, w), dtype=np.float32)
    for dy in range(3):
        for dx in range(3):
            s += p[dy:dy + h, dx:dx + w]
            c += pm[dy:dy + h, dx:dx + w]
    return np.where(mask, values, s / np.maximum(c, 1.0))


def demosaic(px, w, h):
    f = np.clip(px.astype(np.float32) - BLACK_LEVEL, 0, None)
    rmask = np.zeros((h, w), bool); rmask[0::2, 0::2] = True
    bmask = np.zeros((h, w), bool); bmask[1::2, 1::2] = True
    gmask = np.zeros((h, w), bool)
    gmask[0::2, 1::2] = True; gmask[1::2, 0::2] = True
    return np.stack([box_fill(np.where(rmask, f, 0), rmask, h, w),
                     box_fill(np.where(gmask, f, 0), gmask, h, w),
                     box_fill(np.where(bmask, f, 0), bmask, h, w)], -1)


def denoise(rgb, chroma=0.9, luma=0.35):
    """3x3 box smoothing: heavy on chroma, light on luma."""
    def box(a):
        p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge")
        out = np.zeros_like(a)
        for dy in range(3):
            for dx in range(3):
                out += p[dy:dy + a.shape[0], dx:dx + a.shape[1]]
        return out / 9.0
    y = rgb.mean(axis=2, keepdims=True)
    c = rgb - y
    c = (1.0 - chroma) * c + chroma * box(c)
    y = (1.0 - luma) * y + luma * box(y)
    return np.clip(y + c, 0, None)


def shadow_desat(x, knee=0.035, full=0.16):
    """Fade colour out towards black: residual chroma noise in the shadows is
    what reads as a magenta cast."""
    y = x.mean(axis=2, keepdims=True)
    t = np.clip((y - knee) / (full - knee), 0, 1)
    return y + (x - y) * t


def awb(rgb):
    m = rgb.reshape(-1, 3).mean(axis=0)
    return rgb * np.clip(m.mean() / np.maximum(m, 1e-6), 0.5, 2.0)


def srgb(rgb):
    hi = np.percentile(rgb, 99.7)
    x = np.clip(rgb / hi if hi > 0 else rgb, 0, 1)
    lo = np.percentile(x, 0.5)
    x = np.clip((x - lo) / max(1e-6, 1 - lo), 0, 1)
    x = shadow_desat(x)
    x = np.clip((x - BLACK_LIFT) / (1.0 - BLACK_LIFT), 0, 1)
    return np.clip(x ** (1 / 2.2), 0, 1)


def save_png(path, rgb8):
    h, w, _ = rgb8.shape
    raw = b"".join(b"\x00" + rgb8[y].tobytes() for y in range(h))
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))


def rgb_to_qimage(rgb):
    a = np.clip(rgb, 0, 255).astype(np.uint8)
    h, w, _ = a.shape
    return QImage(a.data, w, h, 3 * w, QImage.Format_RGB888).copy()


# --------------------------------------------------------------------------
# capture thread
# --------------------------------------------------------------------------

class Capture(QThread):
    preview = Signal(object)      # QImage
    raw_frame = Signal(object)    # np.uint16 (h, w), latest full frame
    still_ready = Signal(object)  # list of raw frames, still capture finished
    failed = Signal(str)
    stats = Signal(str)

    def __init__(self, video, subdev, w, h, exp, gain,
                 blank=None, exp_max=PREVIEW_EXP_MAX, frames_limit=0):
        super().__init__()
        self.video, self.subdev, self.w, self.h = video, subdev, w, h
        self.exp, self.gain = exp, gain
        self.blank = blank
        self.exp_max = exp_max
        self.frames_limit = frames_limit
        self.shot = frames_limit > 0
        self.auto_exp = True
        self.auto_wb = True
        self.frames = 0
        self.running = False
        self.proc = None
        self.wb_gains = np.ones(3, np.float32)
        self.recent = []
        self.prev = None

    @staticmethod
    def denoise_preview(rgb, chroma=0.55):
        y = rgb.mean(axis=2, keepdims=True)
        c = rgb - y
        p = np.pad(c, ((1, 1), (1, 1), (0, 0)), mode="edge")
        acc = np.zeros_like(c)
        for dy in range(3):
            for dx in range(3):
                acc += p[dy:dy + c.shape[0], dx:dx + c.shape[1]]
        b = acc / 9.0
        return (1.0 - chroma) * c + chroma * b + y

    def stop(self):
        self.running = False
        if self.proc:
            self.proc.terminate()

    def run(self):
        self.running = True
        if self.blank:
            set_blanking(self.subdev, self.blank)
        _lo, _hi = ctrl_range(self.subdev, "exposure")
        if _hi:
            self.exp_max = min(self.exp_max, _hi)
            self.exp = max(1, min(self.exp, self.exp_max))
        set_ctrl(self.subdev, exposure=self.exp, analogue_gain=self.gain)
        shell(["v4l2-ctl", "-d", self.video,
               "--set-fmt-video=width=%d,height=%d,pixelformat=pRAA"
               % (self.w, self.h)])
        bpl, size = get_v4l2_format(self.video)
        if not size:
            size = self.w * self.h * 10 // 8
        self.bpl = bpl
        cmd = ["v4l2-ctl", "-d", self.video, "--stream-mmap",
               "--stream-count=1000000", "--stream-to=/dev/stdout"]
        try:
            self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL,
                                         bufsize=size)
        except Exception as e:            # noqa: BLE001
            self.failed.emit(str(e))
            return

        fps_t = time.time()
        fps_n = 0
        ae_tick = 0
        while self.running:
            data = self.proc.stdout.read(size)
            if not data or len(data) < size:
                if self.running:
                    self.failed.emit("stream ended (check dmesg)")
                break
            px = unpack_frame(data, self.w, self.h, self.bpl)
            if px is None:
                continue
            self.frames += 1
            fps_n += 1
            self.recent.append(px)
            if len(self.recent) > 4:
                self.recent.pop(0)
            if self.frames_limit and self.frames >= self.frames_limit:
                self.still_ready.emit(list(self.recent))
                break

            # ---- preview: half-res RGB from the four Bayer phases ----
            rgb = quick_preview(px, self.w, self.h, 4)
            if self.auto_wb:
                m = rgb.reshape(-1, 3).mean(axis=0)
                target = np.clip(m.mean() / np.maximum(m, 1e-6), 0.5, 2.0)
                self.wb_gains = 0.85 * self.wb_gains + 0.15 * target
            rgb = self.denoise_preview(rgb)
            rgb = rgb * self.wb_gains
            hi = np.percentile(rgb[::2, ::2], 99.7) or 1.0
            x = np.clip(rgb / hi, 0, 1)
            lo = np.percentile(x[::2, ::2], 0.4)
            x = np.clip((x - lo) / max(1e-6, 1.0 - lo), 0, 1)
            x = shadow_desat(x)
            # motion-adaptive temporal blend: static areas average away most
            # of the sensor noise, moving detail switches to the new frame so
            # there is no ghost trail
            if self.prev is not None and self.prev.shape == x.shape:
                d = np.abs(x - self.prev).mean(axis=2, keepdims=True)
                a = np.clip(0.30 + 9.0 * d, 0.30, 1.0)
                x = a * x + (1.0 - a) * self.prev
            self.prev = x
            img = rgb_to_qimage((x ** (1 / 2.2)) * 255.0)
            self.preview.emit(img)
            self.raw_frame.emit(px)

            # ---- auto exposure on a 95th-percentile meter ----
            ae_tick += 1
            if self.auto_exp and ae_tick % (1 if self.shot else 2) == 0:
                # average metering: a small bright lamp in frame must not
                # pull the exposure down for everything else
                level = float(px[::4, ::4].mean())
                if level > 0:
                    err = AE_TARGET / level
                    if err > 1.08 or err < 0.92:
                        # bounded step: never stall on a large error
                        step = min(max(err, 0.35), 2.0)
                        # exposure first (noise free), gain only at its limit
                        if step > 1.0:
                            if self.exp < self.exp_max:
                                self.exp = min(self.exp_max, int(self.exp * step) + 1)
                            else:
                                self.gain = min(GAIN_MAX,
                                                int(self.gain * step) + 1)
                        else:
                            if self.gain > GAIN_DEFAULT:
                                self.gain = max(GAIN_DEFAULT, int(self.gain * step))
                            else:
                                self.exp = max(EXP_MIN, int(self.exp * step))
                        set_ctrl(self.subdev, exposure=self.exp,
                                 analogue_gain=self.gain)
            if time.time() - fps_t >= 1.0:
                self.stats.emit("%d fps   exposure %d   gain %d   %dx%d"
                                % (fps_n, self.exp, self.gain, self.w, self.h))
                fps_t, fps_n = time.time(), 0

        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except Exception:             # noqa: BLE001
                self.proc.kill()


# --------------------------------------------------------------------------
# still capture (runs in a separate thread so the UI stays responsive)
# --------------------------------------------------------------------------

class SaveStill(QThread):
    done = Signal(str)
    failed = Signal(str)

    def __init__(self, px, w, h, outdir):
        super().__init__()
        self.px, self.w, self.h, self.outdir = px, w, h, outdir

    def run(self):
        try:
            px = self.px
            if isinstance(px, list):
                ref = float(px[-1].mean())
                keep = [f for f in px
                        if f.shape == px[-1].shape
                        and abs(float(f.mean()) - ref) <= 0.10 * ref]
                if len(keep) >= 2:
                    px = np.mean(np.stack(keep), axis=0)
                else:
                    px = px[-1]
            rgb = demosaic(px, self.w, self.h)
            rgb = denoise(rgb)
            rgb = awb(rgb)
            rgb8 = (srgb(rgb) * 255.0 + 0.5).astype(np.uint8)
            os.makedirs(self.outdir, exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            png = os.path.join(self.outdir, "giulia-%s.png" % stamp)
            save_png(png, rgb8)
            jpg = png[:-4] + ".jpg"
            if subprocess.run(["which", "ffmpeg"], capture_output=True).returncode == 0:
                shell(["ffmpeg", "-loglevel", "error", "-y", "-i", png, jpg])
            self.done.emit(png if not os.path.exists(jpg) else jpg)
        except Exception as e:            # noqa: BLE001
            self.failed.emit(str(e))


# --------------------------------------------------------------------------
# UI
# --------------------------------------------------------------------------

class Preview(QLabel):
    def __init__(self):
        super().__init__()
        self.setAlignment(Qt.AlignCenter)
        self.setMinimumSize(480, 360)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setStyleSheet("background:#101010;color:#777;")
        self.setText("starting camera...")
        self._pix = None

    def set_image(self, img):
        self._pix = QPixmap.fromImage(img)
        self._rescale()

    def _rescale(self):
        if self._pix:
            self.setPixmap(self._pix.scaled(self.size(), Qt.KeepAspectRatio,
                                            Qt.SmoothTransformation))

    def resizeEvent(self, e):
        self._rescale()
        super().resizeEvent(e)


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("giulia camera")
        self.cap = None
        self.last_px = None
        self.saving = None
        self.busy = False
        self.still_wh = (PREVIEW_W, PREVIEW_H)
        self.outdir = os.path.expanduser("~/Pictures/giulia")

        try:
            self.sensor, self.subdev, self.video = find_nodes()
        except Exception as e:            # noqa: BLE001
            QMessageBox.critical(self, "giulia camera", str(e))
            raise SystemExit(1)

        setup_pipeline(self.sensor, PREVIEW_W, PREVIEW_H)

        self.exp_lo, self.exp_hi = ctrl_range(self.subdev, "exposure")
        self.gain_lo, self.gain_hi = ctrl_range(self.subdev, "analogue_gain",
                                                (0, 960))

        root = QWidget()
        v = QVBoxLayout(root)
        v.setContentsMargins(6, 6, 6, 6)

        self.preview = Preview()
        v.addWidget(self.preview, 1)

        bar = QHBoxLayout()
        self.shutter = QPushButton("◉  拍照")
        self.shutter.setMinimumHeight(44)
        self.shutter.setMinimumWidth(120)
        self.shutter.clicked.connect(self.take_photo)
        bar.addWidget(self.shutter)

        self.mode = QComboBox()
        self.mode.addItems([m[0] for m in MODES])
        self.mode.setToolTip("照片分辨率（预览固定 1080p 以保证流畅）")
        bar.addWidget(self.mode)

        bar.addWidget(QLabel("曝光"))
        self.exp = QSlider(Qt.Horizontal)
        self.exp.setRange(self.exp_lo, min(self.exp_hi, 4000))
        self.exp.setValue(EXP_DEFAULT)
        bar.addWidget(self.exp, 1)

        bar.addWidget(QLabel("增益"))
        self.gain = QSlider(Qt.Horizontal)
        self.gain.setRange(self.gain_lo, self.gain_hi)
        self.gain.setValue(GAIN_DEFAULT)
        bar.addWidget(self.gain, 1)

        self.exp.valueChanged.connect(self.on_exp_slider)
        self.gain.valueChanged.connect(self.on_gain_slider)

        self.ae = QCheckBox("自动曝光")
        self.ae.setChecked(True)
        bar.addWidget(self.ae)
        self.awb = QCheckBox("自动白平衡")
        self.awb.setChecked(True)
        bar.addWidget(self.awb)

        v.addLayout(bar)
        self.setCentralWidget(root)

        self.status = self.statusBar()
        self.status.showMessage("camera starting...")
        self.resize(1100, 800)

        a = QAction(self)
        a.setShortcut(QKeySequence(Qt.Key_Space))
        a.triggered.connect(self.take_photo)
        self.addAction(a)

        self.start_capture()

    def stop_capture(self):
        if self.cap:
            self.cap.stop()
            self.cap.wait(2000)
            self.cap = None

    def start_capture(self):
        setup_pipeline(self.sensor, PREVIEW_W, PREVIEW_H)
        self.cap = Capture(self.video, self.subdev, PREVIEW_W, PREVIEW_H,
                           self.exp.value(), self.gain.value(),
                           blank=None, exp_max=PREVIEW_EXP_MAX)
        self.cap.preview.connect(self.preview.set_image)
        self.cap.raw_frame.connect(self.on_frame)
        self.cap.stats.connect(self.status.showMessage)
        self.cap.failed.connect(self.on_failed)
        self.cap.start()

    def restart(self):
        self.stop_capture()
        self.preview.setText("switching mode...")
        self.start_capture()

    def on_exp_slider(self, v):
        if self.cap and not self.ae.isChecked():
            self.cap.exp = v
            set_ctrl(self.subdev, exposure=v)

    def on_gain_slider(self, v):
        if self.cap and not self.ae.isChecked():
            self.cap.gain = v
            set_ctrl(self.subdev, analogue_gain=v)

    def on_frame(self, px):
        self.last_px = px
        if self.cap:
            self.cap.auto_exp = self.ae.isChecked()
            self.cap.auto_wb = self.awb.isChecked()
            # only mirror the AE's own values back, never fight the user
            if self.ae.isChecked():
                for wdg, val in ((self.exp, self.cap.exp), (self.gain, self.cap.gain)):
                    wdg.blockSignals(True)
                    wdg.setValue(min(wdg.maximum(), val))
                    wdg.blockSignals(False)

    def on_failed(self, msg):
        self.preview.setText("camera error: " + msg)
        self.status.showMessage("error: " + msg)

    def take_photo(self):
        if self.busy:
            return
        self.busy = True
        self.shutter.setEnabled(False)
        mi = self.mode.currentIndex()
        w, h = MODES[mi][1], MODES[mi][2]
        self.still_wh = (w, h)

        # Same amount of light, but spend it on a long exposure instead of
        # analogue gain: that is where the noise comes from.
        total = 1
        if self.cap:
            total = max(1, self.cap.exp) * max(1, self.cap.gain)
        exp = 16000
        gain = int(np.clip(total / float(exp), self.gain_lo,
                           max(self.gain_lo, self.gain_hi)))

        self.status.showMessage("拍照中 %dx%d ..." % (w, h))
        self.preview.setText("拍照中 %dx%d ..." % (w, h))
        self.stop_capture()
        setup_pipeline(self.sensor, w, h)
        self.cap = Capture(self.video, self.subdev, w, h, exp, gain,
                           blank=STILL_BLANK, exp_max=16000, frames_limit=12)
        self.cap.still_ready.connect(self.on_still)
        self.cap.failed.connect(self.on_failed)
        self.cap.start()

    def on_still(self, frames):
        self.stop_capture()
        if not frames:
            self.on_save_failed("no frames captured")
            return
        w, h = self.still_wh
        self.status.showMessage("saving photo...")
        self.saving = SaveStill(list(frames), w, h, self.outdir)
        self.saving.done.connect(self.on_saved)
        self.saving.failed.connect(self.on_save_failed)
        self.saving.start()

    def on_saved(self, path):
        self.saving = None
        self.busy = False
        self.shutter.setEnabled(True)
        self.status.showMessage("saved " + path, 8000)
        self.restart()

    def on_save_failed(self, msg):
        self.saving = None
        self.busy = False
        self.shutter.setEnabled(True)
        self.status.showMessage("save failed: " + msg, 8000)
        self.restart()

    def cleanup(self):
        if self.cap:
            self.cap.stop()
            self.cap.wait(2000)
            self.cap = None

    def closeEvent(self, e):
        if self.cap:
            self.cap.stop()
            self.cap.wait(2000)
        super().closeEvent(e)


def main():
    import signal
    app = QApplication(sys.argv)
    app.setApplicationName("giulia camera")
    win = MainWindow()
    # make a SIGTERM/SIGINT stop the capture thread and its v4l2-ctl child
    app.aboutToQuit.connect(win.cleanup)
    signal.signal(signal.SIGTERM, lambda *a: app.quit())
    signal.signal(signal.SIGINT, lambda *a: app.quit())
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
