#!/bin/sh
# Bring up the MT6833 Wi-Fi stack.
# The vendor modules are installed in the kernel module tree
# (/usr/lib/modules/<ver>/extra) and indexed by depmod, so modprobe resolves
# the wmt_drv dependency automatically - no hardcoded paths.
[ -d /sys/module/wmt_chrdev_wifi ] || modprobe wmt_chrdev_wifi || exit 1
[ -d /sys/module/wlan_6833_axi ]   || modprobe wlan_6833_axi   || exit 1
sleep 2
[ -c /dev/wmtWifi ] || { echo "no /dev/wmtWifi"; exit 1; }
# "1" is a no-op when already powered; a failure here still leaves wlan0 usable
echo 1 > /dev/wmtWifi || echo "power-on reported an error (continuing)"
i=0
while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 15 ]; do i=$((i+1)); sleep 1; done
[ -e /sys/class/net/wlan0 ] || { echo "wlan0 never appeared"; exit 1; }
ip link set wlan0 up || true
exit 0
