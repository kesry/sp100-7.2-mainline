#!/usr/bin/env python3
"""Build a bootable boot.img from source artifacts.

Assembles an Android header-v2 boot image for MT6833 (GNT-AN00):

    [ 0x0000 header (2048 bytes, from template) ]
    [ kernel (gzip, 2K aligned) ]
    [ ramdisk slot (size 0) ]
    [ appended DTB (MTK LK wrapper + standard FDT inside) ]
    [ zeros ]
    [ 0xec6000 AVB footer, copied verbatim from the template ]
    [ padding to 32 MiB ]

The template pieces (header, stock appended DTB with its 64-byte LK wrapper,
AVB footer) ship in the repository under boot-template/ because they cannot be
regenerated from source: the LK wrapper bytes and the AVB footer come from the
factory image.

usage: build_boot.py <raw_Image> <out_boot.img> [--dtb <wrapper.dtb>]
"""
import gzip
import os
import zlib
import struct
import sys

PAGE = 2048
IMG_SIZE = 32 * 1024 * 1024        # boot partition size
FOOTER_OFF = 0xEC6000              # AVB footer offset inside the image
HERE = os.path.dirname(os.path.abspath(__file__))
def _find_template():
    """boot-template/ may sit next to this script or one level up."""
    here = os.path.join(HERE, "boot-template")
    up = os.path.join(HERE, os.pardir, "boot-template")
    for cand in (here, up):
        if os.path.isdir(cand):
            return os.path.abspath(cand)
    return os.path.abspath(here)


TEMPLATE = _find_template()


def align(n, a=PAGE):
    return (n + a - 1) // a * a


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    image, out = sys.argv[1], sys.argv[2]
    dtb_override = None
    if "--dtb" in sys.argv:
        dtb_override = sys.argv[sys.argv.index("--dtb") + 1]

    global TEMPLATE
    if "--template" in sys.argv:
        TEMPLATE = os.path.abspath(sys.argv[sys.argv.index("--template") + 1])

    header = open(os.path.join(TEMPLATE, "header-2048.bin"), "rb").read()
    assert header[:8] == b"ANDROID!", "bad header template"
    tail = open(os.path.join(TEMPLATE, "avb-tail.bin.gz"), "rb").read()
    tail = zlib.decompress(tail)
    assert len(tail) == IMG_SIZE - FOOTER_OFF, "tail size mismatch"

    dtb_path = dtb_override or os.path.join(TEMPLATE, "stock-appended.dtb")
    dtb = open(dtb_path, "rb").read()

    raw = open(image, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        print("ERROR: pass the raw Image, not a gzip")
        return 1
    if raw[0x38:0x3C] != b"ARMd":
        print("WARNING: arm64 Image magic missing at 0x38")

    kz = bytearray(gzip.compress(raw, compresslevel=9, mtime=0))
    kz[9] = 3                                   # gzip OS byte: Unix

    k_off = PAGE
    d_off = k_off + align(len(kz))
    end = d_off + align(len(dtb))
    if end > FOOTER_OFF:
        print(f"ERROR: payload end 0x{end:x} over AVB footer 0x{FOOTER_OFF:x}")
        return 1

    out_b = bytearray(IMG_SIZE)
    out_b[0:PAGE] = header
    out_b[k_off:k_off + len(kz)] = kz
    out_b[d_off:d_off + len(dtb)] = dtb
    out_b[FOOTER_OFF:] = tail

    struct.pack_into("<I", out_b, 8, len(kz))   # kernel_size
    struct.pack_into("<I", out_b, 16, 0)        # ramdisk_size
    struct.pack_into("<I", out_b, 0x670, len(dtb))  # dtb_size

    open(out, "wb").write(out_b)
    print(f"kernel  : raw {len(raw):,} -> gzip {len(kz):,} at 0x{k_off:x}")
    print(f"dtb     : {len(dtb):,} bytes at 0x{d_off:x} ({os.path.basename(dtb_path)})")
    print(f"footer  : AVB tail {len(tail):,} bytes at 0x{FOOTER_OFF:x}")
    print(f"wrote   : {out} ({len(out_b):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
