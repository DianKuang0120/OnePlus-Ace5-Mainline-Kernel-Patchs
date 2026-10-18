# Front camera (Sony IMX480 module / IMX471 chip)

## Hardware

Android reports the front camera as `imx480`, but the sensor chip ID is
`0x0471` and the module is `sunny_imx480` / `gt24p64e_imx480`.  The sensor
behind the module is compatible with the Sony IMX471 driver.

```text
bus:        qupv3_2 i2c9 @ 0x884000
i2c addr:   0x10 (vendor 0x20 is the 8-bit form)
CSI PHY:    4 (AON)
MCLK:       CAM_CC_MCLK4_CLK, 19.2 MHz, gpio104
reset:      gpio7, active low
vana:       pm8010m L4, 2.8 V
vdig:       pm8010m L2, 1.05 V
vio:        pm8010m L5, 1.8 V
size:       4608x3456 (16 MP)
```

Android boot log:

```text
CAM-SENSOR: Sensor Power up success for imx480 sensor_id:0x471,
            slave_addr:0x20
```

## Mainline support

The kernel now has:

- `drivers/media/i2c/imx480.c` (IMX258/IMX471-style V4L2 driver, with the
  IMX480 global/mode tables and three supplies);
- `arch/arm64/boot/dts/qcom/sm8650-oneplus-giulia.dts`: `&i2c9` front
  camera node on CSIPHY4;
- `CONFIG_VIDEO_IMX471=m`.

Android routes the front camera through **CSIPHY4 -> CSID2 -> IFE2**, so
mainline must use the same path: **CSIPHY4 -> CSID2 -> VFE2/RDI0**.

The first mainline mode is 2304x1728 RAW10 @ 30 fps, matching the mode the
vendor uses (`Sensor[imx480-3] width: 0x900, height: 0x6c0, NORMAL, 30 fps`).
It uses 4-lane D-PHY at **729.6 MHz link** (Android reports MIPI datarate
1459.2 Mbps/lane).  The earlier 122.24 value was the wrong PB clock field.
The vendor CSIPHY4 value is
`lane_enable=0xd5`; mainline keeps the logical `data-lanes = <0 1 2 3>` so CSID
uses `lane_assign=0x3210`, and the CSIPHY D-PHY helper computes the physical
`0xd5` value from the lane positions.

Patches:

```text
giulia-patches/linux/0028-media-i2c-add-Sony-IMX480-driver.patch
giulia-patches/linux/0027-arm64-dts-qcom-sm8650-oneplus-giulia-add-front-camera.patch
```

Build/install:

```sh
cd /home/DianKuang/Mainline-Kernel/linux
./scripts/config --file out/.config -m VIDEO_IMX471
make ARCH=arm64 LLVM=1 CC="ccache clang" CXX="ccache clang++" \
    O=out -j8
sudo make ARCH=arm64 LLVM=1 CC="ccache clang" CXX="ccache clang++" \
    O=out INSTALL_MOD_STRIP=1 modules_install
```

## Probe

After booting the new kernel/DTB:

```sh
sudo modprobe imx471
dmesg | grep -i -E 'imx471|imx480|csiphy4'
media-ctl -p
v4l2-ctl --list-devices
```

Expected on success:

```text
imx471 9-0020: ... probe
```

If probe fails:

- `chip id mismatch` means the module is not the expected IMX471-compatible
  part, or the supplies/reset are wrong;
- `-ENOENT`/`-EPROBE_DEFER` means MCLK4 or a regulator is missing;
- check that the front camera is not held in reset by the bootloader.

## Capture topology

The front sensor is on CSIPHY4.  The exact CSID/VFE node should be read from
`media-ctl -p`; do not assume CSID0 (that is the ultrawide IMX355 path).

# Main camera (Sony IMX906 / LYT-700)

## Hardware / topology

```text
sensor:     Sony IMX906 (LYT-700), 50 MP
CCI bus:    CCI0 master 1 (i2c-12), addr 0x1a (16-bit regs, 8-bit data)
CSI PHY:    1 (C-PHY, 3 trios, lane_assign=0x210, lane_enable=0x2a)
CSID:       0
VFE:        0 / RDI0 -> /dev/video0
MCLK:       CAM_CC_MCLK1_CLK, 19.2 MHz, gpio101
reset:      gpio82, active low
rails:      avdd L6M 1.8 V, dvdd L1M 1.1 V, dovdd L5M 1.8 V,
            vaf/v_custom2 L7M, v_custom1 L3M 2.8 V
mode:       4096x3072 RAW10 @ 30 fps, link 2.49 GHz
```

Android maps the main sensor to `CSIPHY1 -> CSID0 -> IFE0`.  Mainline uses
`CSIPHY1 -> CSID0 -> VFE0/RDI0` (`/dev/video0`).

## Two bring-up fixes that were required

1. **CSI virtual channel.** The vendor mode table programs `0x0110 = 0x02`
   (VC 2).  mainline camss hardcodes VC 0 in the CSID RDI config, so every
   packet was dropped with `UNMAPPED_VC_DT`.  The driver now sets
   `0x0110 = 0x00`.

2. **Start table.** The first two writes decoded from the vendor stream-on
   block (`0x0303=0x60`, `0x0302=0xe6`) are per-frame AE values captured at a
   different moment.  Writing `0x0303=0x60` before stream-on wedges the
   sensor's PLL: I2C reads then return a constant until the sensor is power
   cycled.  Those two writes are removed from `imx906_start_regs`.

With those two changes the sensor reports `chipid=0x0906`, `0x0100=0x01`,
`0x0111=0x03`, the CSIPHY status matches the vendor streaming snapshot, and
two full frames are captured.

## Capture

```sh
sudo ./main-capture.sh          # leaves /tmp/main.raw (2 x 4096x3072 RAW10)
```

The file is MIPI-packed RAW10 (4 pixels in 5 bytes).  The preview helper
writes an auto-contrasted PNG:

```sh
./raw10-preview.py /tmp/main.raw /tmp/main_preview.png
```

Quick unpack:

```python
import numpy as np
d = np.fromfile('/tmp/main.raw', dtype=np.uint8)
fr = 4096 * 3072 * 10 // 8
f = d[:fr].reshape(-1, 5)
p = np.empty((f.shape[0], 4), dtype=np.uint16)
p[:, 0] = (f[:, 0].astype(np.uint16) << 2) | (f[:, 4] & 3)
p[:, 1] = (f[:, 1].astype(np.uint16) << 2) | ((f[:, 4] >> 2) & 3)
p[:, 2] = (f[:, 2].astype(np.uint16) << 2) | ((f[:, 4] >> 4) & 3)
p[:, 3] = (f[:, 3].astype(np.uint16) << 2) | ((f[:, 4] >> 6) & 3)
img = p.reshape(3072, 4096)
```

## QSC calibration

The driver reads the 3072-byte per-unit QSC (lens shading) blob from the
module EEPROM (i2c 0x50 / offset 0x2210) and writes it to sensor register
0x1000 before stream-on, in 8-byte chunks.  If the EEPROM cannot be read the
driver logs `QSC calibration unavailable` and streams without it; frame
capture is unaffected.

Patches:

```text
giulia-patches/linux/0020-media-i2c-add-Sony-IMX906-driver.patch
giulia-patches/linux/0021-camss-add-sm8650-C-PHY-support.patch
giulia-patches/linux/0029-camss-csiphy4-use-vendor-settle-count.patch
```

# 2MP macro (OmniVision OV02B10)

## Hardware / topology

```text
sensor:     OmniVision OV02B10, 1600x1200 (2 MP)
CCI bus:    CCI1 master 0, addr 0x3d (7-bit; vendor 8-bit 0x7a)
CSI PHY:    3 (D-PHY, 1 lane, lane_assign 0x0, lane_enable 0x81)
CSID:       2
VFE:        2 / RDI0 -> /dev/video6
MCLK:       CAM_CC_MCLK0_CLK, 24 MHz, gpio100
reset:      gpio110, active low
rails:      avdd pm8010m_l4 2.8 V, dovdd pm8010m_l5 1.8 V
mode:       1600x1200 RAW10 @ 30 fps
```

Android routes the macro through `CSIPHY3 -> CSID2 -> IFE2`, so mainline
uses `CSIPHY3 -> CSID2 -> VFE2/RDI0`.  The vendor log reports a 660 Mbps
D-PHY data rate (330 MHz link); the mainline `ov02b10` driver currently
advertises 360 MHz link for its Rockchip-derived 24 MHz mode table.

The sensor answers at 7-bit address 0x3d, not the more common 0x3c.
Downstream `qcom,cam-sensor2` has no `reg` property; the address was read
from the CCI trace (`Sensor Power up success for ov02b sensor_id:0x2b,
slave_addr:0x7a`, `set param sid 0x3d`).

## Mainline support

`arch/arm64/boot/dts/qcom/sm8650-oneplus-giulia.dts` adds:

```text
&camss/ports/port@3   csiphy3_ep
&cci1_i2c0            ov02b10: camera@3d
```

Patch:

```text
giulia-patches/linux/0030-arm64-dts-qcom-sm8650-oneplus-giulia-add-ov02b10-macro.patch
```

The driver is `CONFIG_VIDEO_OV02B10=m`.  Probe:

```sh
sudo modprobe ov02b10
dmesg | grep -i -E 'ov02b10|csiphy3'
media-ctl -p | grep -i ov02b10
```

Capture (leaves two frames in `/tmp/macro.raw`):

```sh
sudo ./macro-capture.sh
```

