#!/bin/bash
# 试用不同的 ACCEL_MOUNT_MATRIX，改完 reload+trigger+强制重启 iio-sensor-proxy+打开自动旋转。
# 用法: sudo bash try-rotation.sh <0|1|2|3>
#   0 = 1,0,0;0,1,0;0,0,1        (单位阵)
#   1 = -1,0,0;0,-1,0;0,0,1      (绕Z 180°)
#   2 = 0,1,0;-1,0,0;0,0,1       (绕Z +90°)
#   3 = 0,-1,0;1,0,0;0,0,1       (绕Z -90°)
mats=(
	"1, 0, 0; 0, 1, 0; 0, 0, 1"
	"-1, 0, 0; 0, -1, 0; 0, 0, 1"
	"0, 1, 0; -1, 0, 0; 0, 0, 1"
	"0, -1, 0; 1, 0, 0; 0, 0, 1"
)
i=${1:-1}
m=${mats[$i]}
f=/usr/lib/udev/rules.d/90-giulia-ssc.rules

echo "== matrix[$i] = $m"
sudo sed -i "s#ACCEL_MOUNT_MATRIX}=\"[^\"]*\"#ACCEL_MOUNT_MATRIX}=\"$m\"#" "$f"
echo "rule : $(grep -o 'ACCEL_MOUNT_MATRIX}="[^"]*"' "$f")"
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --action=add
sleep 1
echo "env  : $(udevadm info /dev/fastrpc-adsp | grep -o 'ACCEL_MOUNT_MATRIX=.*')"

echo "== 强制重启 iio-sensor-proxy"
sudo systemctl stop --no-block iio-sensor-proxy 2>/dev/null
sleep 1
sudo pkill -9 -f '/usr/lib/iio-sensor-proxy' 2>/dev/null
sleep 1
sudo systemctl reset-failed iio-sensor-proxy 2>/dev/null
sudo systemctl start iio-sensor-proxy 2>/dev/null
sleep 2
echo "iio-sensor-proxy: $(systemctl is-active iio-sensor-proxy)"

timeout 5 kscreen-doctor output.DSI-1.autoRotatePolicy.always >/dev/null 2>&1
echo "== 现在把手机横过来(摄像头在左)，跑这个看朝向名:"
echo "   busctl --system get-property net.hadess.SensorProxy /net/hadess/SensorProxy net.hadess.SensorProxy AccelerometerOrientation"
