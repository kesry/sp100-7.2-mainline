#!/bin/bash
# Pack a rootfs tree into an ext4 image that can be dd'ed to the userdata
# partition. The image is deliberately a fixed size; growing it on the device
# is a single resize2fs.
#
# usage: make-rootfs-img.sh <rootfs-dir> <out.img> <size like 4G>
set -e
SRC="$1"
OUT="$2"
SIZE="$3"
[ -d "$SRC" ] || { echo "usage: $0 <rootfs-dir> <out.img> <size>"; exit 1; }

echo "=== create empty image ==="
rm -f "$OUT"
truncate -s "$SIZE" "$OUT"
mkfs.ext4 -q -F -L GNTUD -m 1 -O ^has_journal,^metadata_csum,^64bit "$OUT"

echo "=== mount and copy ==="
MNT=$(mktemp -d)
mount -o loop "$OUT" "$MNT"
umount_done=0
cleanup() {
    [ "$umount_done" = 0 ] && umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

# copy without crossing into the virtual filesystems; they are re-created
# empty below
tar -C "$SRC" -cpf - \
    --exclude='./dev' \
    --exclude='./proc' \
    --exclude='./sys' \
    --exclude='./run' \
    --exclude='./tmp' \
    . | tar -C "$MNT" -xpf -

mkdir -p "$MNT/dev" "$MNT/proc" "$MNT/sys" "$MNT/run" "$MNT/tmp"
chmod 1777 "$MNT/tmp"
sync

echo "=== ext4 label and uuid (AFTER umount) ==="
# tune2fs must never run while the image is mounted: the mounted superblock
# gets flushed back over the change on umount and the identity is silently
# lost (it happened on this project - CI images came out with random UUIDs).
umount "$MNT"
umount_done=1
# Fixed identity keeps images comparable. The kernel finds root via
# root=PARTLABEL=userdata (no initramfs since 0eed531d2), so the UUID is not
# a boot dependency; it is kept stable for tooling only.
tune2fs -L GNTUD -U 04eeefae-ae89-41dc-b463-f69bd5f964c4 "$OUT" >/dev/null

echo "=== verify ==="
e2fsck -fn "$OUT" | tail -2
tune2fs -l "$OUT" | grep -iE 'volume name|filesystem uuid'
mount -o loop,ro "$OUT" "$MNT"
ls "$MNT/usr/bin" >/dev/null && echo "tree OK"
umount "$MNT"
df -h "$OUT" | tail -1

sync
echo "wrote $OUT"
