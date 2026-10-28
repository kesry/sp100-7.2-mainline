#!/bin/bash
# ============================================================================
# pack-boot.sh - one-click boot.img build for GNT-AN00 (MT6833)
#
# Encapsulates every pitfall we hit, so nobody has to re-read the docs:
#
#  1. The DTB is compiled INTO the kernel (CONFIG_ARM64_USE_BUILTIN_DTB). After
#     editing the DTS the stale arch/arm64/kernel/builtin-dtb.o must be removed
#     or the old blob is reused. This script always removes it.
#
#  2. The boot image carries TWO device trees. LK passes the APPENDED one, so
#     its chosen/bootargs is what actually decides root=. We therefore keep the
#     factory appended DTB from boot-template/ untouched; only the builtin one
#     follows our DTS. Do not "fix" the appended one with our own DTB - that
#     caused a black-screen reboot once.
#
#  3. The AVB footer region (0xec6000 .. 32 MiB) must stay byte-identical to
#     the factory image or LK refuses the image. It comes from boot-template/.
#
#  4. The gzip'd kernel must fit under the AVB footer. The budget is
#     header(2K) + kernel + appended DTB(159,485) <= 0xec6000. This script
#     refuses to write an image that overflows.
#
#  5. The built-in initramfs init locates the root filesystem by ext4 UUID /
#     LABEL. If it does not match the rootfs you intend to boot, the device
#     falls through to recovery. This script checks the identity string.
#
#  6. Never cp multi-GB files on this host (it has taken the machine down);
#     everything here is small and in-place.
#
# usage: pack-boot.sh [-o out.img] [--expect-root LABEL=GNTUD]
# ============================================================================
set -e

WORK=/root/workspace
LINUX=$WORK/publish
TEMPLATE=$WORK/publish/boot-template
PACKER=$WORK/publish/tools/build_boot.py
OUT=$WORK/out/boot_new.img
EXPECT_ROOT=""
EXPECT_ID="GNTUD"          # initramfs init must reference this

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT="$2"; shift 2 ;;
        --expect-root) EXPECT_ROOT="$2"; shift 2 ;;
        --expect-id)   EXPECT_ID="$2";   shift 2 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }

say "=============================================================="
say " GNT-AN00 boot.img packer"
say "=============================================================="

say ""
say "=== 0. prerequisites ==="
[ -d "$LINUX" ]        || { say "  missing $LINUX"; exit 1; }
[ -d "$TEMPLATE" ]     || { say "  missing $TEMPLATE"; exit 1; }
[ -f "$PACKER" ]       || { say "  missing $PACKER"; exit 1; }
for f in header-2048.bin stock-appended.dtb avb-tail.bin.gz; do
    [ -f "$TEMPLATE/$f" ] || { say "  missing template $f"; exit 1; }
done
say "  tree:     $LINUX"
say "  template: $TEMPLATE"
say "  packer:   $PACKER"

say ""
say "=== 1. kernel config sanity ==="
cd "$LINUX"
for sym in ARM64_USE_BUILTIN_DTB EXT4_FS SCSI_UFS_MEDIATEK; do
    v=$(grep -E "^CONFIG_${sym}=" .config | cut -d= -f2)
    [ -n "$v" ] || { say "  CONFIG_$sym missing - wrong .config?"; exit 1; }
    say "  CONFIG_$sym=$v"
done
say "  CONFIG_ARM64_BUILTIN_DTB_NAME=$(grep -E '^CONFIG_ARM64_BUILTIN_DTB_NAME=' .config | cut -d'"' -f2)"

say ""
say "=== 1b. power check (a full rebuild takes 15-25 min) ==="
if [ -x "$WORK/check_power.sh" ]; then
    "$WORK/check_power.sh" || { say "  aborted: battery too low for a full build"; exit 1; }
else
    say "  check_power.sh not found - skipping"
fi

say ""
say "=== 2. rebuild (always drop the stale builtin DTB object) ==="
rm -f arch/arm64/kernel/builtin-dtb.o
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     KCFLAGS=-DMT6833_REVIVE_PROBE Image -j"$(nproc)" > /tmp/packboot-build.log 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    say "  BUILD FAILED - last errors:"
    grep -iE 'error|Error' /tmp/packboot-build.log | grep -v '^make' | head -10 | sed 's/^/    /'
    exit 1
fi
IMG_SIZE=$(stat -c%s arch/arm64/boot/Image)
say "  Image: $IMG_SIZE bytes"

say ""
say "=== 3. pack ==="
mkdir -p "$(dirname "$OUT")"
python3 "$PACKER" arch/arm64/boot/Image "$OUT" | sed 's/^/  /'

say ""
say "=== 4. hard checks ==="
FAIL=0
SZ=$(stat -c%s "$OUT")
if [ "$SZ" = "33554432" ]; then say "  size 33,554,432 .............. ok"
else say "  size $SZ != 33554432 ......... FAIL"; FAIL=1; fi

python3 - "$OUT" "$TEMPLATE" "$EXPECT_ROOT" "$EXPECT_ID" <<'PY'
import struct, sys, zlib

img, tmpl, expect_root, expect_id = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
d = open(img, 'rb').read()
t = open(tmpl + '/header-2048.bin', 'rb').read()
stock = open(tmpl + '/stock-appended.dtb', 'rb').read()
tailgz = open(tmpl + '/avb-tail.bin.gz', 'rb').read()
tail = zlib.decompress(tailgz)
bad = []

def chk(name, cond, detail=''):
    print(f"  {name:34s} {'ok' if cond else 'FAIL'} {detail}")
    if not cond:
        bad.append(name)

chk('ANDROID! magic', d[:8] == b'ANDROID!')
page = struct.unpack_from('<I', d, 36)[0]
chk('page size 2048', page == 2048)
ks = struct.unpack_from('<I', d, 8)[0]
rs = struct.unpack_from('<I', d, 16)[0]
ds = struct.unpack_from('<I', d, 0x670)[0]

# header: only kernel_size may differ from the template
diff = [i for i in range(2048) if d[i] != t[i]]
chk('header differs only in kernel_size', all(i in (8, 9, 10, 11) for i in diff),
    f'({len(diff)} bytes)')

raw = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(d[page:page + ks])
chk('kernel ARMd magic', raw[0x38:0x3c] == b'ARMd')

d_off = page + ((ks + 2047) // 2048) * 2048
chk('appended DTB untouched (factory)', d[d_off:d_off + ds] == stock,
    f'({ds} bytes)')
chk('AVB tail untouched (factory)', d[0xec6000:] == tail)

end = d_off + ((ds + 2047) // 2048) * 2048
chk('payload under AVB footer', end <= 0xec6000,
    f'(end 0x{end:x}, headroom {0xec6000 - end} bytes)')

# builtin DTB cmdline
magic = struct.pack('>I', 0xd00dfeed)
sites, i = [], 0
while True:
    i = raw.find(magic, i)
    if i < 0:
        break
    sites.append(i); i += 4
best = None
for s in sites:
    size = struct.unpack_from('>I', raw, s + 4)[0]
    if 8 < size < 4 * 1024 * 1024 and s + size <= len(raw):
        if best is None or size > best[1]:
            best = (s, size)
if best:
    txt = raw[best[0]:best[0] + best[1]].decode('latin1')
    import re
    m = re.search(r'root=[^\x00]{0,120}', txt)
    cmd = m.group(0) if m else 'none'
    print(f"  builtin cmdline: {cmd}")
    if expect_root:
        chk('builtin root= matches', expect_root in cmd)
else:
    chk('builtin DTB found', False)

# initramfs identity
found = False
ok_id = False
i = 0
while True:
    i = raw.find(b'\x1f\x8b\x08', i)
    if i < 0:
        break
    try:
        cpio = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(raw[i:i + 8 * 1024 * 1024])
    except Exception:
        i += 3
        continue
    if cpio[:6] == b'070701':
        found = True
        ok_id = expect_id.encode() in cpio
        break
    i += 3
chk('initramfs embedded', found)
chk(f'initramfs references {expect_id}', ok_id)

print(f"  gzip kernel: {ks:,} bytes")
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] || FAIL=1

say ""
say "=== 5. result ==="
if [ $FAIL -ne 0 ]; then
    say "  PACK FAILED - do not flash this image"
    exit 1
fi
ls -lh "$OUT" | sed 's/^/  /'
md5sum "$OUT" | sed 's/^/  /'
say ""
say "  flash with:  sh /root/flashboot /root/$(basename "$OUT") <md5-above>"
say "  PACK_OK"
