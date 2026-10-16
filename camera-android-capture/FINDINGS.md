# IMX906 register capture — SOLVED (2026-10-17)

We now have the **complete, lossless** IMX906 (LYT-700) register sequence, plus
all the bus/PHY facts needed by the mainline driver.  This file records how,
where the data lives, and what is still missing.

## 1. Getting at the phone's Android data from the running Linux

We run *inside* the phone (Linux userspace on a container/VM), so Android's
partitions are plain block devices:

```
sda14  13.6G  super      <- Android dynamic partitions
sda16  953M   esp        <- mainline kernel + dtb
sda17  238G   Linux      <- this rootfs
```

Extract a logical partition (needs root; on mainline `systemd-run` pops a
polkit prompt, inside the Android container use `sudo`):

```sh
lpunpack --partition=vendor_dlkm_a /dev/sda14 /tmp/out
```

Images are **EROFS (LZ4)** → decompress with `fsck.erofs --extract=DIR img`
(or one file: `fsck.erofs --path=/etc/x --extract=DIR img`).

And Android itself is reachable with **root adb** (`adb connect <ip:port>`,
KernelSU), so the phone never has to be rebooted for captures.

## 2. Where the register table is NOT

* `vendor_dlkm_a` → `/lib/modules/camera.ko` (17.6 MB, stripped): has the
  OPLUS FTM tables and full mode tables for *other* sensors, **no IMX906**.
* `odm/lib64/camera/com.qti.sensor.imx906.so`: 19 KB, DOL helper only.
* vendor / odm / vendor_dlkm / my_product / my_stock / product / system_ext /
  my_{region,preload,manifest,company,heytap,carrier} — nothing.
* `com.qti.sensormodule.sunny_imx906.bin`: Chromatix calibration.

`camera.ko`'s table layout (for reference): `struct cam_sensor_i2c_reg_array`
= `{u32 addr; u32 data; u32 delay; u32 mask; +12 B}` → **stride 28**.
`imx906_setting` (the 59-entry FTM table) is at offset **0x9f51f8** and matches
the OPLUS source byte for byte; IMX355's mode table is at 0x4e3888.

The IMX906 **mode/init table is sent from userspace** (`cam_sensor` receives it
as a packet), which is why it is not in any kernel module.

## 3. The old CCI capture is lossy — the queue-word log is not

`debug_mdl` bit 14 (CAM_CCI) logs `cam_cci_data_queue()`, but that line only
shows the **first register of each packed CCI word**: measured on the
mode-config region, 103 lines hid 464 further writes (~18 % coverage).

The rule that *does* work is the raw hardware-queue word log:

```
...: 1248: CCI0_I2C_M1_Q0 addr type 2 data type 1 cmd_size <N>     # call start
...: 1438: free_size <f>, en_seq_write <E> i: <I> len: <L>         # buffer
...: 1447: CCI0_I2C_M1_Q0 LOAD_DATA_ADDR <word>, len:<L>, cnt:<n>   # L words
```

Each buffer is `I` little-endian bytes split over `L` words:

```
first buffer of a call:  [cmd][addr_hi][addr_lo][d0][d1]...   dK -> addr+K
continuation (en_seq=1): [cmd][d0][d1]...                     dK -> last+K
```

(`en_seq_write` is logged *after* the flag flips, so the first buffer of a
sequential call still carries the address — hence the `cmd_size` line marks
call starts.)  `cmd & 0xf == 9` is `CCI_I2C_WRITE_CMD`.
`decode_cci_words.py` implements this and was validated byte-for-byte against
the known 59-entry FTM table.

### Capture recipe (no reboot needed)

```sh
adb shell 'am force-stop com.oplus.camera; dmesg -c >/dev/null'
adb shell 'echo 16384 > /sys/module/camera/parameters/debug_mdl'   # bit14 CAM_CCI
adb shell 'am start -a android.media.action.STILL_IMAGE_CAMERA'
sleep 8
adb shell dmesg | grep LOAD_DATA_ADDR > words.txt
adb shell 'echo 0 > /sys/module/camera/parameters/debug_mdl'
./decode_cci_words.py words.txt decoded.txt
./gen_tables.py decoded.txt imx906_tables.h
```

Raw result archived: `cci_words_dmesg.txt`, `imx906_full_decoded.txt`
(**3685 writes**).

## 4. What the sequence contains

| range (decoded index) | meaning |
|---|---|
| 0 – 58   | OPLUS FTM table (59 regs, matches `cam_sensor_initsettings.h`) |
| 59 – 60  | `0x31a7=0x06`, `0x0106=0x01` |
| 61 – 3130| **QSC calibration blob** — 3072 bytes written to `0x1000`, read from the module EEPROM (`eeprom_slave_addr=0xa0`, `qsc_data_size=0xc00`). Per-unit data → **omitted from the mainline tables** |
| 3131 +   | mode configuration |
| … – 3650 | stream-on block, ends with `0x0100=0x01` |
| 3651 +   | per-frame AE + stream-off |

Mode configuration highlights (16-bit registers):

```
HMAX 0x0342/43 = 0x25d0 (9680)      VMAX 0x0340/41 = 0x1d68 (7528)
crop 0x0344-0x034b = 0,0,0x1fff,0x17ff
0x0900=01 0x0901=22 0x0902=00       X_OUT 0x034c/d = 0x1000 (4096)
0x3005=02 0x3144=00 0x3148=04       Y_OUT 0x034e/f = 0x0c00 (3072)
0x3149=01 0x31c0=41
PLL: 0x0301=08 0x0303=02 0x0305=03 0x0306=01 0x0307=57
     0x030b=01 0x030d=08 0x030e=05 0x030f=52
0x3205=00 0x3213=01 0x324c=01 0x3700=01 0x3701=02 0x3702=01 ...
```

Stream-on (`imx906_start_regs`, 22 writes):

```
0x0303=60 0x0301=25 0x0104=01 0x3160=00 0x0340=35 0x0341=38
0x0202=35 0x0203=08 0x0204=3f 0x0205=00 0x020e=01 0x020f=00
0x0104=00 0x0b8e=01 0x0b8f=00 0x0b90=02 0x0b91=1e 0x0b92=01
0x0b93=fa 0x0b94=01 0x0b95=00 0x0100=01        <- streaming on
```

Exposure is `0x0202/0x0203` (16-bit), gain `0x0204/0x0205`.

## 5. Sensor facts from Android (CAM_SENSOR / CAM_CSIPHY, bits 5/15)

```
Sensor[imx906-0] res index: 3, width: 0x1000, height: 0xc00,
                 capability: SHDR(1EXP) 2PD, fps: 30
sensor_id: 0x906, sensor_slave_addr: 0x34 (7-bit 0x1a)
CSIPHY1: 3phase:1 (C-PHY!), lane_cnt: 0x3, lane_assign: 0x210,
         lane_enable: 0x2a, settle time: 5600000000, datarate: 7471533333
```

**The IMX906 is driven in C-PHY mode (3 trios), not D-PHY.**

## 6. Consequence: mainline camss only accepts D-PHY

`drivers/media/platform/qcom/camss/camss.c` rejects anything but
`V4L2_MBUS_CSI2_DPHY` ("Most SoCs support both D-PHY and C-PHY standards, but
currently only D-PHY is supported in the driver"), **yet** the sm8650 csiphy
resources already use `csiphy_ops_3ph_1_0` (the C-PHY ops).

Two ways forward:

1. **Let mainline speak C-PHY** — allow `V4L2_MBUS_CSI2_CPHY` in
   `camss_parse_endpoint()`, describe the sensor endpoint as C-PHY with
   `data-lanes = <0 1 2>` (3 trios) and set the link frequency from
   `datarate` (`7471533333`, qcom reports the aggregate → per-trio ≈ 2.49 G),
   then check `csiphy_ops_3ph_1_0` really is selected for the port.
2. **Switch the sensor to D-PHY** — `0x0111` is the Sony
   `CSI_SIGNALING_MODE` register and the vendor writes `0x03`; try `0x01`/`0x02`
   with a matching PLL/lane setup and a D-PHY link frequency.

Option 1 is the honest one (the hardware does it, the driver just refuses).

## 7. Status

* `imx906_tables.h` — generated (559 init + 22 stream-on writes), driver builds.
* DTS — node, rails, MCLK1 + GDSC, reset, `&cci0_i2c1`, camss `port@1` ready;
  endpoint still says `data-lanes = <0 1 2 3>` + a guessed 600 MHz — must
  become C-PHY / 3 trios.
* Missing: C-PHY plumbing (see §6) and finally an image.
