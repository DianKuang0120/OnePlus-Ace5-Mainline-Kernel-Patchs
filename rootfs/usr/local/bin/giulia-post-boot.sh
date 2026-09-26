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

generate_bt_mac() {
    local mac first_byte fixed_byte
    mac=$(printf "%s:%s:%s:%s:%s:%s" $(od -An -N6 -tx1 /dev/urandom))
    first_byte="${mac%%:*}"
    fixed_byte=$(printf "%02x" "$(( (0x$first_byte & 0x3F) | 0xC0 ))")
    echo "${fixed_byte}${mac#$first_byte}"
}

use_random_bt_mac() {
    local cache_file=/var/lib/bluetooth/cached_mac
    local bt_mac

    if [[ -f "$cache_file" ]]; then
        bt_mac=$(cat "$cache_file" | tr -d '[:space:]')
        echo "Using cached bluetooth mac: $bt_mac"
    else
        bt_mac=$(generate_bt_mac) &&
        mkdir -p "$(dirname "$cache_file")" &&
        echo "$bt_mac" > "$cache_file" &&
        echo "Generated and cached new bluetooth mac: $bt_mac"
    fi

    btmgmt --index 0 public-addr "$bt_mac"
}

use_random_bt_mac

# # Save dmesg
# dmesg >/var/tmp/dmesg_latest.txt

exit 0
