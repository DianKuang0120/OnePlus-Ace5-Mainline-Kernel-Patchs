# giulia-patches

Out-of-tree support for the **OnePlus 13R / Ace 5** (codename `giulia`,
`qcom,sm8650`) on mainline Linux.

Two kinds of things live here:

1. **`linux/*.patch`** — a numbered, ordered series against a pristine
   `v7.2.8` tree.  Apply in order with `patch -p1`.
2. **`linux/<path>`** — the current contents of files that the series
   (or later work) touches, so the tree can be populated by copying as
   well as by patching.  This is what `build_giulia.sh` rsyncs in
   (`arch/`, `drivers/`, `iio/`, `sound/`, `include/`).

## Series

| # | subject |
|---|---------|
| 0001 | arm64: dts: qcom: sm8650: add OnePlus Giulia |
| 0002 | input: add Synaptics TCM touchscreen driver |
| 0003 | drm: panel-boe-aa577: add BOE AA577 panel driver |
| 0004 | ASoC: codecs: sia91xx: add SIA9177 audio codec driver |
| 0005 | power: supply: add OPLUS sm8650 pmic-glink |
| 0006 | power: supply: add SC8547 charge-pump driver + Makefile |
| 0007 | iio: magnetometer: add AK09970 build support |
| 0008 | arm64: dts: qcom: sm8650: fix PCIe iommu map cells |
| 0009 | input: misc: add Qualcomm PM8550B HV haptics |

Files not yet folded into the series but present as overrides include the
alert-slider driver (`iio/magnetometer/ak09970-slider.c`), the full
downstream haptics driver (`input/misc/qcom-hv-haptics*.c`,
`include/trace/events/qcom_haptics.h`) and the DTS.

## Build

    cd linux                      # pristine v7.2.8
    for p in ../giulia-patches/linux/*.patch; do patch -p1 -i "$p"; done
    cp -a ../giulia-patches/linux/{arch,drivers,iio,sound,include} .
    make ARCH=arm64 LLVM=1 CC="ccache clang" CXX="ccache clang++" O=out -j8 Image modules
    make ARCH=arm64 LLVM=1 CC="ccache clang" CXX="ccache clang++" O=out -j8 dtbs

The user's `../giulia_29-07/build_giulia.sh` wraps this (rsync overrides,
apply patches, merge `giulia.config`, build, install).

## Status

Working: display/panel (60/90/120 Hz), touchscreen, side buttons, alert
slider, haptics (kernel driver + a user-space event daemon), NFC
(kernel), thermals, battery, USB, charging (PD), WLAN, Bluetooth, CPU,
speakers, GPU, flash.

Known gaps / parked:

* **Camera** — CAMSS/CCI pipeline works, but the sensors (main IMX906 =
  LYT-700, ultrawide IMX355, front imx480/s5k3p9) don't answer on the
  CCI bus from the DT-only setup; the accurate I2C address / power
  sequence lives in the downstream `imx355_mipi_raw` driver, which is
  not in the available source.  See `SESSION_HANDOFF.md` §23.
* **Mobile data / calls / SMS** — needs an IPA routing/filter engine
  that mainline `drivers/net/ipa` does not implement.  See §12–18.
* **Sensors (accel/gyro/ALS/PS), GPS, fingerprint** — gated behind the
  SLPI / secure world.
* **Fast charging (SVOOC)**, **top microphone**.

The running support table is `linux/README.md`; the running log of every
session is `../SESSION_HANDOFF.md`.
