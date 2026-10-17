# IMX906 (LYT-700) register capture from stock Android

The 50 MP main sensor (Sony IMX906, OPLUS cam-sensor0) is not in the
open-source camera-kernel (OPLUS keeps the per-sensor driver closed), so the
initialisation/mode tables had to be captured at runtime.

## How it was captured (stock Android, root adb)

The OPLUS/Qualcomm camera kernel logs every queued CCI i2c write when the
CAM_CCI debug module is enabled:

```sh
# enable CAM_CCI (bit 14) debug -> dmesg gets "cam_cci_data_queue: ... addr .. data .."
adb shell 'echo 16384 > /sys/module/camera/parameters/debug_mdl'
adb shell 'dmesg -c > /dev/null'
# open the main (back) camera, e.g.:
adb shell am start -a android.media.action.STILL_IMAGE_CAMERA
# (or open the camera app manually)
adb shell 'dmesg | grep "cmd_size .* addr 0x" > /data/local/tmp/cci_dump.txt'
adb pull /data/local/tmp/cci_dump.txt .
adb shell 'echo 0 > /sys/module/camera/parameters/debug_mdl'   # stop the spam
```

`CCI0_I2C_M1_Q0` is the IMX906 (CCI0 master 1, slave addr 0x34 = 7-bit 0x1a,
chip id 0x0906).  `/sys/kernel/debug/camera/cci/en_dump_cciN` only dumps the
CCI hardware registers on NACK/timeout, so it is not useful for a full trace.

## Files

| file | what |
|---|---|
| `cci_dump.txt` | raw `CAM_DBG ... cam_cci_data_queue ...` lines (790 writes) |
| `imx906_regs.txt` | `addr data` for every write on CCI0 master1 |
| `imx906_init.txt` | batches 0-5: the initialisation sequence (422 writes) |
| `imx906_streamon.txt` | batch 6: mode/PLL + group-hold + `0x0100=0x01` stream on |
| `imx906_perframe_examples.txt` | batches 7-11: per-frame AE groups |

## Structure of the trace (batches split where `cmd_size` grows)

- **batches 0-5 (init, 422 writes)** — batch 0 is the COM/init (matches the
  OPLUS `cam_sensor_initsettings.h` `.imx906_setting`), batches 1-5 contain a
  large block (incl. a ~287-write blob through register `0x1000`, i.e. a
  firmware/OTP download), PLL and mode setup.
- **batch 6 (stream on, 11 writes)**
  `0x0104=1` (group hold) → `0x3010/0x3160/0x0340(frame length)/0x0e2x/0x0e5x`
  → `0x0104=0` → `0x0b8e=1` → `0x0100=1` (stream on).
- **batches 7+ (per-frame AE, ~7-11 writes each)** — the values change with the
  scene, so these are the AE registers:
  `0x0e20` (exposure), `0x0e58` / `0x0e50` (gain), `0x0340` (frame length), and
  a `0x0003` trigger, all wrapped in `0x0104=1 ... 0x0104=0` group hold.

## Device info (from the Android dmesg)

- sensor_id 0x0906, slave_addr 0x34 (7-bit 0x1a)
- mode seen: `res index 3, width 0x1000 (4096), height 0xc00 (3072)`,
  `SHDR(2EXP) LBMF 2PD`, 30 fps
- wiring: CCI0 master1 (SDA/SCL gpio115/116), CSI PHY1, MCLK1 (gpio101,
  19.2 MHz), reset gpio82; vana pm8010m_l6 (2.8 V), vdig pm8010m_l1 (1.1 V),
  vio pm8010m_l5 (1.8 V)
