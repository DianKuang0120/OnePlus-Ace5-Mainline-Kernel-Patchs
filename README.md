# OnePlus 13R / Ace 5 Mainline

## Support

| Feature              | Description             | State |
|:---------------------|:------------------------|:-----:|
| Internal storage     |                         | ✅    |
| Side buttons         |                         | ✅    |
| Mute slider          | AK09970 hall + userspace daemon | ✅    |
| Proximity sensor     |                         | ❌    |
| Light sensor         |                         | ❌    |
| Accelerometer        |                         | ❌    |
| Gyroscope            |                         | ❌    |
| Magnetometer         |                         | ❌    |
| Fingerprint          |                         | ❌    |
| NFC                  | nxp-nci                 | ✅    |
| Thermals             |                         | ✅    |
| Battery              |                         | ✅    |
| USB host             |                         | ✅    |
| USB device           |                         | ✅    |
| USB power delivery   |                         | ✅    |
| Charging             |                         | ✅    |
| WLAN                 | Uses random MAC         | ✅    |
| CPU                  |                         | ✅    |
| Touchscreen          | Off-tree                | ✅    |
| Bluetooth            | Uses random MAC         | ✅    |
| GPS                  |                         | ❌    |
| Speakers             | Off-tree                | ✅    |
| Microphones          | Bottom MIC for now      | ⚠️    |
| GPU                  |                         | ✅    |
| Camera               | CAMSS up, sensor drivers missing | ❌    |
| Flash                | pm8350c flash-led       | ✅    |
| Calls                | Needs VoLTE/IMS; modem has no CS+PS | ❌    |
| SMS                  | Receive works; send fails (QMI WMS 56) | ⚠️ |
| Mobile Data          | IPA sm8650 consumer endpoints 17/22/24 | ✅ |
| Display              | Off-tree                | ✅    |
| Haptics              | driver + userspace event daemon | ✅    |