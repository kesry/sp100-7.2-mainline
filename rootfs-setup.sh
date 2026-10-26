#!/bin/bash
# Configure a freshly debootstrapped Debian rootfs for the GNT-AN00 device.
#
# Everything that cannot be done inside debootstrap: hostname, fstab, console
# getty, the usb-gadget RNDIS/ACM network, and the services the device needs
# at boot. Run with the rootfs tree as $1, from inside the CI container with
# root privileges.
set -e
R="$1"
[ -d "$R" ] || { echo "usage: $0 <rootfs-dir>"; exit 1; }

echo "=== hostname ==="
echo "GNT-AN00" > "$R/etc/hostname"

echo "=== fstab ==="
cat > "$R/etc/fstab" <<'EOF'
# The root device is found by the kernel's built-in initramfs and passed via
# the cmdline, so nothing here needs to name it.
configfs  /sys/kernel/config  configfs  defaults  0 0
EOF

echo "=== serial console on ttyGS0 (USB ACM) and tty0 ==="
mkdir -p "$R/etc/systemd/system/serial-getty@ttyGS0.service.d"
cat > "$R/etc/systemd/system/serial-getty@ttyGS0.service.d/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty -o '-p -- \\u' --keep-baud 115200,38400,9600 ttyGS0 vt100
Restart=always
EOF

echo "=== usb-gadget (RNDIS + ACM) ==="
mkdir -p "$R/usr/local/sbin"
cat > "$R/usr/local/sbin/usb-gadget.sh" <<'EOF'
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
EOF
chmod +x "$R/usr/local/sbin/usb-gadget.sh"

cat > "$R/etc/systemd/system/usb-gadget.service" <<'EOF'
[Unit]
Description=USB composite gadget (RNDIS network + ACM serial console)
DefaultDependencies=no
After=sys-kernel-config.mount
Wants=sys-kernel-config.mount
ConditionPathExists=/sys/class/udc

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/usb-gadget.sh
ExecStartPost=-/bin/systemctl restart serial-getty@ttyGS0.service

[Install]
WantedBy=multi-user.target
EOF

echo "=== usb0 static address ==="
mkdir -p "$R/etc/NetworkManager/system-connections"
cat > "$R/etc/NetworkManager/system-connections/usb0-static.nmconnection" <<'EOF'
[connection]
id=usb0-static
type=ethernet
interface-name=usb0
autoconnect=true
autoconnect-priority=100

[ethernet]
mtu=1500

[ipv4]
method=manual
address1=192.168.42.2/24
never-default=true

[ipv6]
method=ignore
EOF
chmod 600 "$R/etc/NetworkManager/system-connections/usb0-static.nmconnection"

echo "=== enable services ==="
cat > "$R/etc/writable" <<'EOF'
EOF
rm -f "$R/etc/writable"
SERVICES="systemd-pstore.service usb-gadget.service"
mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
mkdir -p "$R/etc/systemd/system/sysinit.target.wants"
ln -sf /lib/systemd/system/systemd-pstore.service "$R/etc/systemd/system/sysinit.target.wants/"
ln -sf /etc/systemd/system/usb-gadget.service "$R/etc/systemd/system/multi-user.target.wants/"

echo "=== root password: lock it, ssh key goes in authorized_keys when present ==="
chroot "$R" passwd -l root 2>/dev/null || true
if [ -f "$R/../root-authorized_keys" ]; then
    mkdir -p "$R/root/.ssh"
    cp "$R/../root-authorized_keys" "$R/root/.ssh/authorized_keys"
    chmod 600 "$R/root/.ssh/authorized_keys"
fi

echo "=== locales ==="
echo "en_US.UTF-8 UTF-8" > "$R/etc/locale.gen"

echo "done"
