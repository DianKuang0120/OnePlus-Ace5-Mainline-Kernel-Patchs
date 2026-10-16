# giulia SSC 传感器（FastRPC + libssc）

把 giulia（OnePlus 13R / Ace 5，SM8650）的 **加速度 / 陀螺 / 磁强计 / 光照** 在主线
上跑起来。方案完全复刻自 [Osilvfe](https://github.com/Osilvfe) 在
**caihong（OnePlus Pad Pro，同 SM8650）** 上验证过的做法。

## 为什么不是"直连 IIO 驱动"

giulia 的物理运动传感器由 **ADSP 里的 SSC（传感器执行环境）** 独占驱动，AP 侧
I2C/SPI 总线**不属于 AP**：

- Android 下游 registry（`/odm/etc/sensor/config/lanai_mtp_*.json`）里：

  | 传感器 | 芯片 | 总线 | 地址 |
  |---|---|---|---|
  | 磁强计 | `mmc56x3x` | I2C instance **2** | `0x30` |
  | ALS/PS(+CCT) | `tcs3720` | I2C instance **2** | `0x39` |
  | 加速度/陀螺 | `icm456xx` | I2C instance **3** | `0x69` |

  （`sensor_config.json` 机型 23851 明确：accel/gyro=`icm456xx`，mag=`mmc56x3x`，
  架构 `qcom_sensor_see`。）
- SSC bus_instance → AP 总线映射（Osilvfe 从 vendor QUPv3 描述得出并在 caihong 实测）：
  **SSC I2C instance 2 = 主线 `i2c2` @ `0x00a88000`（QUPv3 SE2）**，
  **SSC SPI instance 3 = 主线 `spi3` @ `0x00a8c000`**。
- 主线里直接 `i2c_hub_1/6/9`（984000/998000/9a4000）一读寄存器就 **同步外部中止**
  （TZ/固件归属，非 ADSP 运行、非时钟）。

所以正确做法是：**不碰总线**，用 FastRPC 把厂商 registry 喂给 ADSP 的 sensorspd，
再用 **libssc** 从用户态取数据。你朋友的 `mmc5633.c` 就是主线驱动本身、
"optional only"，不是让传感器工作的关键。

## 目录

| 文件 | 说明 |
|---|---|
| `build-giulia-ssc.sh` | 组装 `bundle-giulia/`（HexagonFS root + 二进制） |
| `install-giulia-ssc.sh` | 安装到运行中的 rootfs 并 `systemctl enable --now` |
| `giulia-ssc.service` | `hexagonrpcd -f /dev/fastrpc-adsp -d adsp -s -R ...` |
| `90-giulia-ssc.rules` | udev：把 `fastrpc-adsp` 事件接到 iio-sensor-proxy（含 mount matrix） |
| `iio-sensor-proxy.giulia-ssc.conf` | iio-sensor-proxy 依赖 giulia-ssc |
| `hexagonrpc/` | `linux-msm/hexagonrpc` @ PR27 `40e50412812a` + 可写-registry 补丁 |
| `bundle-giulia/` | 产物（构建缓存，可不提交） |

## 依赖 / 前置

- 内核：`CONFIG_QCOM_FASTRPC=y`、`CONFIG_QCOM_Q6V5_PAS=m`(ADSP)、
  `CONFIG_RPMSG_QCOM_GLINK{,_SMEM}`、`CONFIG_QCOM_SYSMON=m`、`CONFIG_QRTR=y`（均已在 giulia 配置）。
- 固件：`/lib/firmware/qcom/sm8650/oneplus/giulia/adsp.mbn` + `adsp_dtb.mbn`。
- 用户态：`libssc` + `iio-sensor-proxy`（本机已有 `libssc 0.4.4` / `iio-sensor-proxy 3.9`，
  正是 Osilvfe 验证的版本）。
- 构建：`meson ninja json-c`（有）；本机即 aarch64，**原生编译**，无需交叉。
- 素材：`/home/DianKuang/android_extract/{vendor_ext,odm_ext}`（已解出的 vendor/odm）。

## 构建

```sh
cd /home/DianKuang/Mainline-Kernel/ssc
# hexagonrpc（若 hexagonrpc/ 不存在）：
#   git clone --filter=blob:none https://github.com/linux-msm/hexagonrpc.git
#   cd hexagonrpc && git fetch origin refs/pull/27/head
#   git checkout --detach 40e50412812adce9dddc2be9a4510ebce05eef89
#   git apply --unidiff-zero ../caihong-hexagonrpc.patch
# 原生编译
(cd hexagonrpc && meson setup build --buildtype=release && meson compile -C build)
# 组装 bundle
./build-giulia-ssc.sh
```

`build-giulia-ssc.sh` 会：按 vendor `json.lst` + odm `json_list` 收集配置 →
`sscregistrygen -p MTP -s 577`（vendor 先、odm 覆盖）生成 registry（约 379 个文件）→
组装 HexagonFS root（`sensors/{config,odm-config,registry,persist/registry,sns_reg.conf}`、
`dsp/adsp/`、`socinfo/`）。

## 安装 / 运行（**重启进主线后**，需要 root）

```sh
cd /home/DianKuang/Mainline-Kernel/ssc
sudo ./install-giulia-ssc.sh
```

或手动：把 `bundle-giulia/bin/hexagonrpcd` → `/usr/local/libexec/giulia-ssc/`、
`libhexagonrpc.so.0.5` → `/usr/local/lib/giulia-ssc/`、`bundle-giulia/root` →
`/var/lib/giulia-ssc/root`，安装 service/udev/drop-in，`systemctl enable --now giulia-ssc`。

## 验证

```sh
systemctl status giulia-ssc --no-pager
journalctl -u giulia-ssc -n 80 --no-pager      # 期望 registry DIR/parsed_file_list.csv 生成
find /var/lib/giulia-ssc/root -name DIR -o -name parsed_file_list.csv -o -name sns_reg_version

# 直接读传感器（ssccli 用法是 --sensor <名>，没有 --list）
ssccli --sensor accelerometer                  # 期望 Z≈+9.8（平放桌面）
ssccli --sensor magnetometer
ssccli --sensor compass
ssccli --sensor light
```

`iio-sensor-proxy` 由 udev 触发；**如果它没起**（`systemctl is-active iio-sensor-proxy` = inactive），
重新触发一次 udev（我们的 `90-giulia-ssc.rules` 在其自带 `80-` 规则之后用 `=` 覆盖，
会补上 `ssc-accel`）：

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --action=add
udevadm info /dev/fastrpc-adsp | grep IIO_SENSOR   # 期望含 "ssc-accel ssc-light ssc-compass"
systemctl status iio-sensor-proxy --no-pager
monitor-sensor --all                               # iio-sensor-proxy 的 D-Bus 输出
```

- `systemctl stop giulia-ssc` 后再 `start` 应能恢复（registry 已持久化）。
- 若 ADSP 未起：`ls /dev/fastrpc-adsp`；`cat /sys/class/remoteproc/remoteproc*/name`。

## 已知待调

- **加速计方向矩阵：已定为单位阵**（`1,0,0; 0,1,0; 0,0,1`）。
  实测（2026-10-16）：平放屏幕朝上 `ssccli --sensor accelerometer` = `Z≈+9.88`，
  竖直竖屏顶朝上 = `Y≈+9.8` → SSC 输出已是标准设备系，无需变换。
  （caihong 是平板、自然方向不同，所以它需要 90° 矩阵；giulia 竖屏手机不需要。）
- **光照（`ssccli --sensor light`）在 giulia 上不发布**：OPLUS 的
  `sensor_config.json` 写死 `android.sensor.light = module_ignore / no_aftersale`
  （只用了 tcs3720 的 proximity）。所以 `ssccli --sensor light` 报
  `Unable to initialize light sensor: UNKNOWN` 属**预期**，非移植问题。
- **陀螺仪**：SSC 有数据，但 `ssccli`/`iio-sensor-proxy` 都没有陀螺读数通道；
  需要的话用 libssc 的 raw 接口或 Osilvfe 的 `ssc-inspect` 读。
- **pocket / smart_prox 等 OPLUS 虚拟传感器**：SSC 只发布标准 SUID；厂商私有类型需另接。

## 实测结果（2026-10-16，主线 7.2.9-giulia）

| 传感器 | `ssccli --sensor` | 结果 |
|---|---|---|
| 加速度 | `accelerometer` | ✅ `X=0.48 Y=-0.04 Z=9.88 m/s²`（平放） |
| 磁强计 | `magnetometer` | ✅ `X≈19.8 Y≈23.2 Z≈-120.7 µT` |
| 罗盘 | `compass` | ✅ `312°…` |
| 接近 | `proximity` | ✅ `NEAR/FAR`（tcs3720） |
| 光照 | `light` | ❌ 原厂 `module_ignore`，SSC 不发布 |

## 回退

```sh
sudo systemctl disable --now giulia-ssc
sudo rm -f /usr/lib/systemd/system/giulia-ssc.service \
           /usr/lib/udev/rules.d/90-giulia-ssc.rules \
           /usr/lib/systemd/system/iio-sensor-proxy.service.d/giulia-ssc.conf
sudo rm -rf /usr/local/libexec/giulia-ssc /usr/local/lib/giulia-ssc /var/lib/giulia-ssc
sudo udevadm control --reload-rules && sudo systemctl daemon-reload
```
