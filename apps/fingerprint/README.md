# Fingerprint (Jiiov JV0301 / `uff_jv`) — smcinvoke loader and command channel

The SM8650 secure world does not implement the legacy QSEECOM `APP_START`
SMC (it returns SMCCC `NOT_SUPPORTED`).  The vendor loader reaches the TA
through smcinvoke's `QSEEComCompatAppLoader` (UID 122).

## Kernel side

- `0025-tee-qcomtee-load-fingerprint-ta.patch` modifies the upstream qcomtee
  driver:
  - stores the platform device for firmware requests;
  - adds a qcom_tzmem-backed memory object (the 9 MiB TA image cannot use the
    normal TEE SHM pool after boot);
  - loads `uff_jv` at qcomtee probe via
    `QSEEComCompatAppLoader` op 0 (`loadFromRegion`) and keeps the app
    controller object;
  - implements `IQSEEComCompat_sendRequest` (op 0) with the vendor argument
    layout (4 IB, 2 OB, 4 IO) and a tee_shm-backed memory object for the
    shared command buffer;
  - creates `/dev/qcomtee_fp` (mode 0600).
- `linux/drivers/tee/qcomtee/fp_loader.c` is the new loader/command source.
  Copy it next to the patched files when applying the series.

The smcinvoke image layout is **not** the QSEECOM concatenated `.mdt` layout:

1. `.b00` is the ELF header + program headers.
2. Put `.bNN` at the `p_offset` recorded in `.b00`.
3. `size = p_offset[last] + size(.bNN last)`.
4. `.mdt` is not used by the smcinvoke app loader.

With that layout the TA loads:

```text
qcomtee: QTEE version 5.2.0
qcomtee: loaded 'uff_jv' (img 9154648 bytes), controller ...
qcomtee: fingerprint command device /dev/qcomtee_fp ready
```

## Command ABI

`/dev/qcomtee_fp` has two ioctls:

- `QCOMTEE_FP_IOCTL_SEND` (`'F'`, 0x10): raw 64-byte QSEEComCompat request,
  no shared object (the simple `QSEECom_send_cmd` shape).
- `QCOMTEE_FP_IOCTL_SEND_DATA` (`'F'`, 0x11): request header plus one
  shared buffer.  The kernel creates a tee_shm memory object and passes it as
  `smo1`; the embedded-offset array is `{4,0,0,0}` so the app controller
  writes the object handle at request word 1.

The HAL/Jiiov command buffer in the shared object is:

```text
offset 0x00: u32 type = 2
offset 0x04: u32 command id (FP_CMD_*)
offset 0x08: u32 status/result (out)
offset 0x30: payload
```

The normal command ids, names and TA-required lengths are listed in
`FINDINGS.md`.  `fpctl.c` implements the header and the common commands.

## Userspace tools

- `fpctl.c` — command tool for `/dev/qcomtee_fp`; `-p` also powers the sensor
  through `/dev/goodix_fp` (without a reset pulse for now).  Examples:

  ```sh
  fpctl -p init          # INIT_TA_CONFIG (0x834) + INIT (0x7d0)
  fpctl status           # GET_AUTHENTICATOR_ID
  fpctl raw 0x7d0 0x1c0  # raw command with the correct header
  ```

- `qcomtee_load.c` — userspace smcinvoke probe.  It opens the qcomtee client
  env and the UID 122 app loader.  It only worked with a diagnostic
  `CAP_SYS_ADMIN` relaxation of qcomtee's root-object check that is **not**
  part of the final patch; the final loader runs from the driver's private
  context.
- `tee_probe.c` — legacy QSEECOM TEE probe (APP_START path), kept for
  comparison.  APP_START is not supported on this platform.
- `FINDINGS.md` — full investigation notes and the command table.
- `frida_dump.js` — Android Frida hook for the vendor HAL `sendMessage`
  (module+0x77fbc) and `QSEECom_send_cmd` / `QSEECom_send_modified_cmd`.
  Capture the real init/config payloads and replay them with `fpctl raw`.
- `captures/android-fps-hal-startup.log` — startup/config/DCS capture.
- `captures/android-fps-enroll.log` — complete left-finger enrollment
  (HAT, capture, feature, save-template).
- `extract_hat.py` — extract a captured `0x7d6` HAT payload.
- `extract_cmd.py` — extract a captured `0x7d7`/`0x7d4`/`0x7d3`/`0x7d8`
  payload.

## Current status

The stock Android HAL was captured with Frida 17.24.0; see
`captures/android-fps-hal-startup.log`.  It uses:

- `0x834` config payload with `"23851"`, `"BOE"`, `"G_OPTICAL_JV0301"`,
  `"NULL"` and QSEECom word 0 = length;
- `0x7d0` with QSEECom word 0 = **0xf000 + length** (`0xf1c0`).

`fpctl` embeds that captured config and sets the QSEECom word correctly.
On mainline, INIT now succeeds:

```text
init-ta-config: 0 (0x0)
init: 0 (0x0)
```

`fpctl -x` also dumps the shared buffer and the 64-byte QSEECom response.
The captured Android HAT was accepted on mainline (`enroll: 0`), so the next
step is the enrollment sample sequence and template save.

Enroll/authenticate also need the Android HAT token and the keymaster HMAC
key path (`SET_HMACKEY`, 0x7e4); the real `0x7d6` HAT payload was captured.

## Next

1. Capture a complete Android enroll/authenticate sequence (keymaster + HAT).
2. Implement the keymaster64 HMAC-key exchange and `SET_HMACKEY`.
3. Port the HAT/token handling for `SET_ACTIVE_GROUP`, `ENROLL` and
   `AUTHENTICATE`, then wire the commands to a libfprint/fingerprint daemon.

## Final fingerprint status

The complete vendor order is now implemented and tested:

```text
uff_spi region -> op2 uff_spi -> 408-byte spi init
release SPI -> uff_jv region -> op2 uff_jv
```

`uff_jv` still reports `fail to get sensor instance` unless the SPI app/region
is resident.  QSEEComCompatAppLoader accepts only one region/app slot; keeping
SPI resident makes the JV region load return result=16, while releasing SPI
loses the sensor instance.  qseecomtee APP_START returns SCM -EIO on this
platform.

The kernel module falls back to the stable `uff_jv`-only path.  Fingerprint is
blocked on a real multi-region QSEECom container or a combined sensor+fpcore
TA image.  The HBM panel control, reset handling, message-header fields,
region/op2 experiments, and the 408-byte SPI init blob are all preserved in
this directory and `giulia-patches/linux/`.
