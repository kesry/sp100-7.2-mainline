#!/bin/sh
# Composite USB gadget: RNDIS network + ACM serial console.
CONFIGFS=/sys/kernel/config/usb_gadget
mkdir -p $CONFIGFS/g1
cd $CONFIGFS/g1
echo 0x18d1 > idVendor
echo 0x4ee7 > idProduct
mkdir -p strings/0x409
echo "GNT-AN00 linux" > strings/0x409/serialnumber
echo "NZONE S7 mainline" > strings/0x409/product
mkdir -p configs/c.1/strings/0x409
echo "RNDIS+ACM" > configs/c.1/strings/0x409/configuration
mkdir -p functions/rndis.usb0
mkdir -p functions/acm.gs6
ln -sf functions/rndis.usb0 configs/c.1/
ln -sf functions/acm.gs6 configs/c.1/
UDC=$(ls /sys/class/udc | head -1)
echo "$UDC" > UDC

# NetworkManager reports the gadget netdev as "unmanaged", so nothing else
# configures it. Assign the address here, after the UDC bind has created usb0.
i=0
while [ ! -e /sys/class/net/usb0 ] && [ $i -lt 50 ]; do
	i=$((i+1))
	sleep 0.1
done
ip link set usb0 up 2>/dev/null
ip addr add 10.42.10.1/24 dev usb0 2>/dev/null
ip addr show usb0
