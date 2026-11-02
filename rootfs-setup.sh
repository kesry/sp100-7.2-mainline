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
# The kernel mounts root itself via root=PARTLABEL=userdata (there is no
# initramfs; it made user namespaces impossible - see commit 0eed531d2), so
# nothing here needs to name the root device.
configfs  /sys/kernel/config  configfs  defaults  0 0
EOF

echo "=== serial console + usb-gadget come from rootfs-overlay/ ==="
# The device-validated copies of serial-getty@ttyGS0.service.d/autologin.conf,
# /usr/local/sbin/usb-gadget.sh and /etc/systemd/system/usb-gadget.service live
# in rootfs-overlay/, taken from the image actually running on the phone. The
# inline versions here drifted behind that image, so they are gone: keep one
# copy in the overlay and fail loudly if it was not staged.
test -f "$R/etc/systemd/system/serial-getty@ttyGS0.service.d/autologin.conf" || {
    echo "missing overlay: serial-getty autologin drop-in"; exit 1; }
test -f "$R/usr/local/sbin/usb-gadget.sh" || {
    echo "missing overlay: usb-gadget.sh"; exit 1; }
test -f "$R/etc/systemd/system/usb-gadget.service" || {
    echo "missing overlay: usb-gadget.service"; exit 1; }

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
address1=10.42.10.1/24
never-default=true

[ipv6]
method=ignore
EOF
chmod 600 "$R/etc/NetworkManager/system-connections/usb0-static.nmconnection"

echo "=== sshd: allow root password login on the USB link ==="
mkdir -p "$R/etc/ssh/sshd_config.d"
cat > "$R/etc/ssh/sshd_config.d/99-gnt-debug.conf" <<'EOF'
# Debug access over the USB RNDIS link. Not on a public network; drop this file
# once the install is trusted.
PermitRootLogin yes
PasswordAuthentication yes
EOF
chmod 644 "$R/etc/ssh/sshd_config.d/99-gnt-debug.conf"

echo "=== NetworkManager: manage every interface ==="
# Without this NM leaves the gadget netdev (usb0) "unmanaged" and nothing
# assigns it an address, so the USB link comes up dead.
mkdir -p "$R/etc/NetworkManager/conf.d"
cat > "$R/etc/NetworkManager/conf.d/99-manage-all.conf" <<'EOF'
[device]
managed=1
EOF
cat > "$R/etc/NetworkManager/conf.d/99-ignore-extra-wifi.conf" <<'EOF'
# The MTK gen4m driver always creates wlan1/p2p0/swlan0 alongside wlan0.
# They stay down; hide them so nmcli and applets only show the usable STA.
[keyfile]
unmanaged-devices=interface-name:wlan1;interface-name:swlan0;interface-name:p2p0;interface-name=sit0
EOF

echo "=== enable services (manual wants symlinks, no dbus needed) ==="
WANTS="$R/etc/systemd/system"
mkdir -p "$WANTS/multi-user.target.wants" "$WANTS/sysinit.target.wants" "$WANTS/basic.target.wants"

# stock units
mkdir -p "$WANTS/shutdown.target.wants" "$WANTS/network-online.target.wants"
ln -sf /lib/systemd/system/systemd-pstore.service "$WANTS/sysinit.target.wants/"
ln -sf /lib/systemd/system/fake-hwclock-load.service "$WANTS/sysinit.target.wants/"
ln -sf /lib/systemd/system/fake-hwclock-save.service "$WANTS/shutdown.target.wants/"
ln -sf /lib/systemd/system/NetworkManager.service "$WANTS/multi-user.target.wants/"
ln -sf /lib/systemd/system/wpa_supplicant.service "$WANTS/multi-user.target.wants/"
ln -sf /lib/systemd/system/ssh.service "$WANTS/multi-user.target.wants/sshd.service"
ln -sf /lib/systemd/system/NetworkManager-wait-online.service "$WANTS/network-online.target.wants/"

# board units from the overlay (native-drm stays a manual unit, exactly as
# the reference image: it is not in multi-user.target.wants there)
for u in usb-gadget gnt-conn gnt-conn-prep gnt-wmt-launcher fb-console-visible \
         spread-device-irqs ssh-host-keys-firstboot; do
    ln -sf "/etc/systemd/system/$u.service" "$WANTS/multi-user.target.wants/"
done
ln -sf /etc/systemd/system/ufs-hungtask-tune.service "$WANTS/basic.target.wants/"

echo "=== root login: root/root, no ssh host keys, empty machine-id ==="
# The board is a development unit: root/root over the USB link, serial console
# autologin. passwd -l here was wrong - the image shipped with the account
# locked and PasswordAuthentication on but no usable password.
echo 'root:root' | chroot "$R" chpasswd || {
    echo "setting root password failed"; exit 1; }

# openssh-keygen ran during package install; ship no keys. The firstboot unit
# regenerates them on the device, so every flashed image does not share one
# host key.
rm -f "$R"/etc/ssh/ssh_host_*_key "$R"/etc/ssh/ssh_host_*_key.pub
if [ -f "$R/../root-authorized_keys" ]; then
    mkdir -p "$R/root/.ssh"
    cp "$R/../root-authorized_keys" "$R/root/.ssh/authorized_keys"
    chmod 600 "$R/root/.ssh/authorized_keys"
fi

# fresh machine-id per boot; an unseeded empty file is the official marker
: > "$R/etc/machine-id"
: > "$R/etc/.journal-stamp" 2>/dev/null || true
rm -f "$R/var/lib/dbus/machine-id"

# drop the package caches; the artifact stays a seed image, apt update works
rm -rf "$R/var/cache/apt/archives" "$R/var/lib/apt/lists"
mkdir -p "$R/var/cache/apt/archives/partial" "$R/var/lib/apt/lists/partial"

echo "=== locales ==="
echo "en_US.UTF-8 UTF-8" > "$R/etc/locale.gen"

echo "done"
