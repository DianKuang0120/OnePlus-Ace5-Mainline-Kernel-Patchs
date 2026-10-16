#!/usr/bin/env bash

set -uo pipefail

# Wait until boot finishes and devices are ready
sleep 3

# Enable gadget functions
mkdir -p /sys/kernel/config/usb_gadget/g1
cd /sys/kernel/config/usb_gadget/g1
echo "0x1d6b" > idVendor
echo "0x0104" > idProduct
mkdir -p configs/c.1 functions/acm.usb0 functions/ecm.usb0 functions/mass_storage.usb0
echo "/dev/disk/by-partlabel/linux_root" > functions/mass_storage.usb0/lun.0/file
echo 1 > functions/mass_storage.usb0/lun.0/ro
ln -s functions/acm.usb0 configs/c.1/
ln -s functions/ecm.usb0 configs/c.1/
ln -s functions/mass_storage.usb0 configs/c.1/
echo "$(ls /sys/class/udc/)" > UDC

# NOTE: setting the Bluetooth public address is intentionally NOT done here.
# This WCN7850 firmware rejects both btmgmt's "Set Public Address" (status
# 0x0b) and the kernel's qca_set_bdaddr, so the BT address cannot be changed;
# the old use_random_bt_mac() helper has been removed.

# # Save dmesg
# dmesg >/var/tmp/dmesg_latest.txt

exit 0
