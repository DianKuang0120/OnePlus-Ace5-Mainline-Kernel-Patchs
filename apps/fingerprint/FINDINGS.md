# Fingerprint bring-up notes (2026-10-17)

## Hardware / firmware

- OnePlus 13R/Ace 5 (giulia, SM8650) uses a **Jiiov JV0301** under-display
  optical sensor, not a Goodix part.
- The DTS node is a placeholder with `compatible = "goodix,gf3626"`, reset
  GPIO 89, `vreg_l14b_3p2`, and `firmware-name = "uff_jv"`.
- The real TA is `uff_jv` (`/lib/firmware/uff_jv.mdt` + `.b00..b08`), built
  for **QSEE5/QTEE on SM8650** (strings show `tee_qsee5_*`, `qsee_fts_*`,
  `qtee_tas/apps/securemsm/trustzone/qsapps/uff`).

## Mainline driver state

- `drivers/input/misc/goodix_fp_spi.c` probes and exposes `/dev/goodix_fp`,
  the vendor ioctl ABI and netlink protocol 25.  It does not do SPI itself;
  the secure world owns the SPI/TA side.
- The driver is only a control/ABI shim.  A userspace client (the vendor HAL
  or an equivalent) is still needed to drive the TA.

## QSEECOM TEE

- `qseecomtee.ko` was blacklisted by
  `/etc/modprobe.d/zz-blacklist-qseecomtee.conf`; after removing the blacklist
  and `modprobe qseecomtee`, the devices appear:
  - `/dev/tee1` (client, impl_id=5)
  - `/dev/teepriv0` (privileged, impl_id=5, CAP_SYS_ADMIN)
- `/dev/tee0` is the QTEE/smcinvoke device (impl_id=4).

## Test result (`fingerprint/tee_probe`)

- Client session by name `uff_jv`:
  - `TEE_IOC_OPEN_SESSION` -> `ENOENT`
  - QMI-scM lookup: `owner=0x32, svc=1, cmd=3, result=0xffffffff`
  - TA is not boot-loaded.
- Privileged load:
  - `TEE_IOC_OPEN_SESSION` -> `EIO`
  - `qcom_scm: qseecom: scm call failed with error -5`
  - dynamic debug: `owner=32, svc=1, cmd=1, result=0, type=0, data=0`
  - `a1..a3` are zero; `a0` is nonzero and `qcom_scm_remap_error(a0)`
    returns `-EIO`.

## Image assembly

- `uff_jv.mdt` is ELF64 with 9 program headers and a hash segment.
- The `.bXX` sizes match the program-header `p_filesz` values exactly.
- `qcom_mdt_read_image()` and the vendor `__qseecom_get_fw_data()` should
  produce the same `.mdt + b00..b08` layout and sizes.
- Therefore the failure is at APP_START/secure-world acceptance, not at the
  kernel image assembly.

## Likely blockers

1. SM8650 QSEECom 0x1402000 may have a different APP_START/SCM acceptance
   path than the SM8250-targeted mainline `qseecomtee` driver.
2. The TA is a QSEE5/QTEE SM8650 TA; it may need an SM8650 service image
   (`cmnlib64` etc.) or the vendor QSEECom loader.  No `cmnlib*` files were
   found in `/lib/firmware` or the extracted Android firmware.
3. The vendor QSEECom driver (`OnePlus13R/vendor/qcom/opensource/
   securemsm-kernel/qseecom/`) is the known-working loader for this TA.

## Next options

- **A. Port the vendor QSEECom driver** (SCM call + service image load) to
  mainline.  Large, but the most likely to succeed.
- **B. Instrument the SMC return `a0`** and compare against the vendor
  `qcom_scm_qseecom_call()` semantics; then adapt APP_START/args.
- **C. Try the QTEE/smcinvoke path** if `uff_jv` is actually a QSEE5 object
  TA rather than a QSEECOM app.
- **D. Park fingerprint** and return to camera/SMS; the QSEECOM path is not a
  quick win.

## Tools

- `fingerprint/tee_probe.c` / `tee_probe`: enumerate TEE devices, try a
  client session by name, try privileged APP_START.
- `fingerprint/FINDINGS.md` (this file).

## 2026-10-17 late — correct path found: smcinvoke QSEEComCompatAppLoader

- The legacy QSEECOM APP_START SMC is **not implemented** on SM8650:
  - std call `fn=0x72000101` -> a0 = 0xffffffffffffffff (SMCCC NOT_SUPPORTED)
  - fast call `fn=0xf2000101` -> a0 = 0xffffffffffffffff
  - APP_LOOKUP / GET_VERSION work, so this is specific to APP_START.
- The vendor loader uses **smcinvoke**, not APP_START:
  - `smcinvoke_kernel.c` opens `CQSEEComCompatAppLoader_UID = 122` through
    `IClientEnv_open`, then calls `IQSEEComCompatAppLoader_loadFromBuffer`
    (or `loadFromRegion`) to load `uff_jv`.
- The mainline qcomtee driver (`/dev/tee0`, impl_id=4) is the smcinvoke
  transport.  With a small root-check relaxation (allow `TEE_OBJREF_NULL`
  credential for `CAP_SYS_ADMIN`), a userspace client can:
  1. root op 5 -> client env object
  2. client env op 0 with UID 122 -> app loader object
  3. app loader op 0/1 -> app controller object
- `fingerprint/qcomtee_load` implements this.  Results:
  - client env: rc=0
  - app loader: rc=0
  - `loadFromBuffer` (9.1 MB UBUF): EINVAL — qcomtee copies the image into
    the object message and `MAX_INBOUND_BUFFER_SIZE` is 4 MB.
  - `loadFromRegion` (TEE SHM memory object): `TEE_IOC_SHM_ALLOC` for 9.1 MB
    fails with ENOMEM because the qcomtee pool uses `alloc_pages_exact()` and
    cannot get a contiguous 9 MB block after boot.
- Remaining work: provide a 9 MB memory object to `loadFromRegion`, either:
  - make qcomtee allocate user SHM from `qcom_tzmem` / CMA (SHM Bridge), or
  - implement the load as an in-kernel qcomtee client using `qcom_tzmem` +
    a custom memory object, then expose the app controller to userspace.
- The qseecomtee legacy driver is not the right loader for this TA.

## 2026-10-17 late — TA loaded successfully

The smcinvoke app loader wants the **ELF file layout**, not the QSEECOM
concatenated image:

- `.b00` contains the ELF header + program headers.
- Every `.bNN` (1..last) is placed at the `p_offset` recorded in `.b00`.
- `fw_size = p_offset[last] + size(.bNN last)`.
- The `.mdt` file is **not** used by `firmware_request_from_smcinvoke()`.

`qcomtee_fp_load()` now assembles that layout into qcom_tzmem and invokes
`QSEEComCompatAppLoader` UID 122 op 0 (`loadFromRegion`).  Result:

```text
qcomtee: QTEE version 5.2.0
qcomtee: loaded 'uff_jv' (img 9154648 bytes), controller ffff000805482000
```

The app controller object is kept in `struct qcomtee.fp_ctl`.

Next:
- implement `IQSEEComCompat_sendRequest` (op 0) on the controller:
  4 input buffers (req, rsp, embedded offsets, is64), 2 output buffers
  (req, rsp), 4 NULL input objects; counts (4,2,4,0).
- expose a small misc device or TEE object so the fingerprint HAL can send
  commands, then port the Jiiov/oplus command ABI from
  `OnePlus13R/vendor/oplus/secure/biometrics/fingerprints/` and/or the
  vendor HAL binary.

## 2026-10-17 late — IQSEEComCompat_sendRequest and business commands

The app controller exposes op 0 (`sendRequest`) with the vendor layout:
4 IB (req, rsp, embedded offsets, is64), 2 OB (req, rsp), 4 IO (smo1..4).
The kernel now implements this in `qcomtee_fp_send()` and exposes
`/dev/qcomtee_fp`:

- `QCOMTEE_FP_IOCTL_SEND` (`'F', 0x10`): raw 64-byte QSEECom request.
- `QCOMTEE_FP_IOCTL_SEND_DATA` (`'F', 0x11`): command buffer + one
  qcomtee memory object (tee_shm/SHM Bridge), with embedded offset 4 so the
  app controller writes the object handle into the request header.
  `is64 = 1`, counts `(4,2,4,0)`.

Observed app-controller limits: a 64/128/256/512/1024-byte response works;
4096 is rejected with QTEE `-95` (`buffers too large`).  The vendor HAL uses
64-byte responses for the normal path, so this is not a problem.

### Command buffer layout

The vendor HAL message is **not** the QSEEComCompat 64-byte header.  The
shared buffer has:

```text
offset 0x00: u32 type = 2
offset 0x04: u32 command id (FP_CMD_*)
offset 0x08: u32 status/result (out)
offset 0x30: payload
```

The 64-byte QSEEComCompat request has its total length in word 0 (the kernel
fills it).  The first word of the shared buffer is `2`, and the command id is
in word 1.  This was confirmed from the HAL's builders (e.g. the 0x834/0x835
messages at service offsets `0x4b0b0` / `0x4ab20`) and by the TA returning
`0x66` (invalid parameter) instead of `0x67` when the command id was written
at offset 4.

### Normal fingerprint core commands

`ta_fpcore_handle_message()` dispatches `0x7d0..0x7e7`, `0x800`, `0x801`.
Required buffer lengths checked by the TA:

| cmd | name | length |
| --- | --- | --- |
| 0x7d0 | FP_CMD_FPCORE_INIT | 0x1c0 |
| 0x7d1 | DEINIT | 0x78 |
| 0x7d2 | SET_ACTIVE_GROUP | 0x100 |
| 0x7d3 | ALGO_GET_FEATURE | 0xa0 |
| 0x7d4 | CAPTURE_IMG | 0xb8 |
| 0x7d5 | PRE_ENROLL | 0x80 |
| 0x7d6 | ENROLL | 0xc8 |
| 0x7d7 | ENROLL_DOWN | 0x90 |
| 0x7d8 | ALGO_ENROLL_IMAGE | 0xb0 |
| 0x7d9 | SAVE_TEMPLATE | 0x80 |
| 0x7da | POST_ENROLL | 0x78 |
| 0x7db | GET_AUTHENTICATOR_ID | 0x80 |
| 0x7dc | AUTHENTICATE | 0x88 |
| 0x7dd | AUTHENTICATE_DOWN | 0xa0 |
| 0x7de | AUTHENTICATE_COMPARE | 0xf0 |
| 0x7df | AUTHENTICATE_STUDY | 0x80 |
| 0x7e0 | AUTHENTICATE_FINISH | 0x78 |
| 0x7e1 | REMOVE | 0x80 |
| 0x7e2 | ENUMERATE | 0x98 |
| 0x7e3 | CANCEL | 0x30 |
| 0x7e4 | SET_HMACKEY | 0x130 |
| 0x7e5 | CUSTOMENC | 0x238 |
| 0x7e6 | GET_DCSINFO | 0x240 |
| 0x7e7 | SET_SENSOR_MODE | 0x80 |
| 0x800 | AUTH_STUDY_SPLIT | 0x80 |
| 0x801 | UPDATE_TEMPLATE | 0x80 |

`FP_CMD_FPCORE_INIT_TA_CONFIG` is command `0x834`, length `0xb0`; the HAL
sends it with a 32-byte config copied from its object.  Command `0x835` is
the DCS-info path at length `0x240`.

### Current result

With the correct header (`type=2`, cmd at +4) the TA executes:

```text
init-ta-config: 0 (success)
init: 5050 (0x13ba)
```

`0x13ba` is a vendor/TA return value from the Jiiov init path
(`[ANC_TA][ReturnValueMap]` maps unknown/vendor failures to 5050); it is not
a transport failure.  The remaining blockers are secure-world SPI/sensor
init (the TA owns the SPI bus) and the TA config/HAT material normally
supplied by the Android HAL/keymaster:

- `SET_ACTIVE_GROUP`/`ENROLL`/`AUTHENTICATE` need the Android HAT token and
  secure storage setup.
- The HAL's `sendHmacKeyToFpta()` starts the `keymaster64` TA and supplies
  the HMAC key (`SET_HMACKEY`, 0x7e4); this is not implemented yet.
- AP-side power/reset is available via `/dev/goodix_fp` (`GF_IOC_RESET`,
  `GF_IOC_ENABLE_POWER`); vdd is `vreg_l14b_3p2` at 3.2 V, reset GPIO 89.
  The mainline DTS currently declares reset as `GPIO_ACTIVE_LOW`, while the
  vendor `oplus,fp_spi` driver drives its `reset_gpio` as an active-high
  signal (it ends `fp_hw_reset()` with value 1).  With the current mainline
  polarity, `GF_IOC_RESET` leaves GPIO89 physically **high**; check this
  against the sensor before treating the reset state as correct.  For the
  current `fpctl -p` tests we only enable vdd and leave the line low.

`fpctl` (this directory) is the userspace command tool.  Example:

```sh
fpctl -p init
fpctl status
fpctl raw 0x7d0 0x1c0
```

## 2026-10-17 late — Android HAL capture (Frida 17.24.0)

Captured the stock Android 16 HAL (`fps_hal`,
`vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff`) over
adb/frida.  The raw log is in
`captures/android-fps-hal-startup.log`.

Startup sequence captured by stopping `fps_hal` and spawning it under Frida:

1. `len=408, type=0, cmd=0xffffffff` — initial QSEECom/TA probe
   (`QSEECom_send_modified_cmd` word 0 = 408).
2. `len=176, type=2, cmd=0x834` — `INIT_TA_CONFIG`.
   The real payload is not zeros:
   - `"23851"` at payload+0x00
   - `"BOE"` at payload+0x20
   - `"G_OPTICAL_JV0301"` at payload+0x40
   - `"NULL"` at payload+0x60
3. `len=576, type=2, cmd=0x835` — `GET_DCSINFO`, sent repeatedly with a
   small stage/index in the first payload word.
4. `len=448, type=2, cmd=0x7d0` — `INIT`; payload is zero except a
   timestamp at +0x10.  QSEECom request word 0 = **0xf1c0**.

The QSEEComCompat request word 0 rule seen in the capture:

- `0x834` / `0x835`: word 0 = payload length (`0xb0`, `0x240`).
- Normal fpcore commands (`0x7d0`, `0x7d5`, `0x7d6`, `0x7e3`, `0x7da`):
  word 0 = **0xf000 + payload length** (`0xf1c0`, `0xf080`, `0xf0c8`,
  `0xf030`, `0xf078`).

The kernel `QCOMTEE_FP_IOCTL_SEND_DATA` now accepts `qsee_len`; 0 means
"use data_len".  `fpctl` uses:

- `0x834`: captured config payload, `qsee_len=0`;
- `0x7d0`: zero payload, timestamp optional, `qsee_len=0xf1c0`;
- other normal commands: `qsee_len=0xf000 + total_len`.

Also captured the real business path while opening Settings:

```text
0x7d5 PRE_ENROLL       len 128, payload all zero
0x7d6 ENROLL           len 200, payload +0x30 contains HAT/token material
0x7e3 CANCEL           len 48
0x7da POST_ENROLL      len 120
```

This confirms the previously inferred header and gives the exact init
configuration.  Next step is testing `fpctl -p init` on the mainline kernel
with the captured config and the `0xf000 + len` QSEECom word.

## 2026-10-18 — mainline init succeeds

With the captured Android config and the correct QSEECom word, mainline now
returns success for INIT:

```text
$ sudo ./fpctl -p init
init-ta-config: 0 (0x0)
init: 0 (0x0)
```

`fpctl -x` also dumps the shared buffer and the 64-byte QSEECom response.
The TA result in `data[8]` is 0; the QSEECom response word at `rsp+0x10` is
`0x65` for these successful calls.

Current command results on mainline:

```text
init         : 0
status       : 0 (authenticator id is still zero)
pre-enroll   : 0 (no challenge in the shared buffer)
get-feature  : 0 (no feature data yet)
capture      : 0 (sensor capture path not yet exercised)
enumerate    : 0
cancel       : 0
```

These are TA-level successes, but the normal fingerprint business flow still
needs the Android HAT/token and keymaster path:

- `SET_ACTIVE_GROUP` / `ENROLL` / `AUTHENTICATE` use the HAT token captured in
  the Android `0x7d6` message.
- `SET_HMACKEY` (0x7e4) needs the keymaster64 HMAC key.
- The current `fpctl` can replay the raw commands, but it does not yet
  generate a valid HAT.

Next practical step: capture a complete Android enroll/authenticate sequence
(including keymaster), or implement the keymaster/gatekeeper HAT generation on
mainline.  The TA transport, init config and command ABI are no longer
blocking.

## 2026-10-18 — full Android enrollment captured

A left-hand fingerprint was enrolled successfully on Android while Frida was
attached to `fps_hal`.  The raw capture is
`captures/android-fps-enroll.log` (698 HAL messages).

Command sequence observed during enrollment:

```text
0x7d5  PRE_ENROLL            3
0x7d6  ENROLL (HAT)          3   payload +0x30 = HAT/token material
0x7d7  ENROLL_DOWN         105
0x7d4  CAPTURE_IMG         103
0x7d3  ALGO_GET_FEATURE     99
0x7d8  ALGO_ENROLL_IMAGE    98
0x7d9  SAVE_TEMPLATE         1
0x7da  POST_ENROLL           3
0x7db  GET_AUTHENTICATOR_ID  1
0x7e1  REMOVE                1
0x7e2  ENUMERATE             2
0x7e3  CANCEL                2
0x7e6  GET_DCSINFO         172
0x836  sensor/logger        105
```

All normal messages use QSEECom word 0 = `0xf000 + len`, e.g.:

```text
0x7d6 len 0xc8 -> 0xf0c8
0x7d7 len 0x90 -> 0xf090
0x7d4 len 0xb8 -> 0xf0b8
0x7d9 len 0x80 -> 0xf080
```

The `0x7d6` payload is the HAT/token; it is different for each of the three
attempts and is signed by the Android keymaster.  The successful enrollment
ended with `0x7d9 SAVE_TEMPLATE`.

The screen appeared black in `screencap` because the enrollment activity is a
secure window, but the HAL logs show real sensor captures and the enrollment
completed.  The page was the genuine
`Settings$FingerprintEnrollEnrolling` activity.

Next: mainline must validate the HAT.  Options:

1. Replay a captured HAT on mainline after loading the keymaster TA.
2. Use the Android keymaster/hardware-backed HAT path.
3. Implement the keymaster64 lookup/load through the same qcomtee app loader
   and generate/validate the token.

## 2026-10-18 — captured Android HAT accepted on mainline

Replaying the last Android `0x7d6` payload on mainline returns:

```text
enroll: 0 (0x0)
```

The HAT/token is visible at the start of the payload (after six zero bytes)
and the QSEECom response word is `0x65`.  This means the Android-signed HAT
was accepted by the TA on the mainline boot; the keymaster/HAT validation is
not the immediate blocker for starting enrollment.

Next: drive the enrollment sample sequence (`0x7d7`, `0x7d4`, `0x7d3`,
`0x7d8`) while the sensor is powered, then `0x7d9`/`0x7da`.

Helper commands added:

```sh
python3 extract_hat.py captures/android-fps-enroll.log 2  # last HAT
python3 extract_cmd.py captures/android-fps-enroll.log 0x7d7
```

`fpctl power-hold <seconds>` keeps `/dev/goodix_fp` open and the vdd enabled
across several `fpctl` invocations.

## 2026-10-17 — Android auth capture, exact sensor and HBM

A successful left-finger authentication was captured with Frida 17.24.0 on
the Android 16 CPH2645 build (`captures/android-fps-auth.log`, raw logcat in
`captures/android-fps-auth-logcat.log`).  The Frida hook is the existing
`frida_dump.js`; the captured command blocks are under `captures/auth/`
(`sequence.json` plus one `.bin` per message).

Useful Android properties:

```text
persist.vendor.fingerprint.optical.sensorlocation = 632::2520
persist.vendor.fingerprint.optical.iconlocation  = 260
persist.vendor.fingerprint.optical.iconsize      = 195
persist.vendor.fingerprint.optical.sensorrotation= 83.5
persist.vendor.fingerprint.brightness.strategy   = hbm
persist.vendor.fingerprint.brightness.threshold  = 5451
```

So the portrait sensor center is **(632, 2520)**, not 2400; the earlier
coordinates were about 120 px too high.  In the current landscape rotation
the sensor is on the right long edge, about 2.5 cm in from that edge.

Authentication sequence captured:

```text
0x7e4  SET_HMACKEY          304 bytes
0x7dc  auth init            136
0x7dd  auth down            160
0x7d4  CAPTURE_IMG         184   (payload mode = 3)
0x7d3  ALGO_GET_FEATURE    160   (payload mode = 3)
0x7de  AUTHENTICATE        240
0x7df  AUTH_RESULT          128
0x7e6  GET_DCSINFO         568 (twice)
0x7e0  auth done            120
```

The HMAC key is in `captures/auth/0x7e4_79b64.bin` (transport block; payload
starts at +0x30).  These payloads are the reference for the mainline auth
implementation.

### HBM

Android sets DRM connector property `HBM_EN` (property id 83) to 4 during
fingerprint auth and back to 0 afterwards.  The panel DT contains the exact
commands:

```text
hbm-on:
  39 00 00 40 00 00 04 ff 5a a5 1e
  39 00 00 40 00 00 02 80 89
  39 00 00 40 00 00 04 ff 5a a5 00
  39 00 00 00 00 00 03 51 0f 4a

hbm-off:      39 00 00 00 00 00 03 51 0d bb
hbm-max:      39 00 00 00 00 00 03 51 10 ed
hbm-exit-max: 39 00 00 00 00 00 03 51 0f fe
```

`panel-boe-aa577.c` now has a `hbm` sysfs attribute:

```sh
echo 2 > /sys/.../ae94000.dsi.0/hbm   # HBM max (fingerprint)
echo 0 > /sys/.../ae94000.dsi.0/hbm   # normal
```

The previous mainline tests used only max SDR backlight (4094) and no HBM;
the vendor threshold for the fingerprint is 5451 and the panel HBM values
are 0x0f4a / 0x10ed, so the sensor was under-illuminated.  This, together
with the wrong y coordinate, explains the all-zero capture and empty
template list.

After booting the HBM-enabled kernel the next check is:

```sh
find /sys/devices -path '*ae94000.dsi.0/hbm'
echo 2 > <that path>
# full white screen, finger at (632, 2520), then run the 25-frame flow
```

## 2026-10-18 — app-loader region/op2 experiments (stopped at secure container)

The vendor HAL loads apps in this order:

```text
uff_spi start_app('/odm/vendor/firmware', ta='uff_spi', 128)
408-byte spi init via QSEECom_send_modified_cmd
QSEECom_shutdown_app(uff_spi)
uff_jv start_app('/odm/vendor/firmware', ta='uff_jv', 128)
```

qcomtee's `QSEEComCompatAppLoader` was extended to:

- load a splitbin as an app-loader region with op 0 (IB = region path,
  IO = image memory object, OO = controller);
- start the app from the region with op 2 (IB = **region path**, 1 OB app id,
  1 OO controller).  `op2` with just the TA name returns 23; with the full
  region path it returns 0/app_id=2/controller;
- send the captured 408-byte `fp_spi_init.bin` to the SPI app.

Result:

```text
op0 region /odm/vendor/firmware/uff_spi -> ok
op2 region /odm/vendor/firmware/uff_spi -> ok
spi init modified -> ok
op0 region /odm/vendor/firmware/uff_jv  -> result=16 while SPI is resident
```

Releasing the SPI region or app lets `uff_jv` load, but then the TA reports
`fail to get sensor instance` again.  The app-loader has one region/app slot;
Android's qseecomd/QSEECom has a real multi-region container.  qseecomtee's
APP_START path returns SCM -EIO on this platform.

Current kernel module falls back to the stable single-app `uff_jv` order.
Fingerprint remains blocked on a multi-region/app QSEECom container or a
combined sensor+fpcore TA image.
