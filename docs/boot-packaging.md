# Boot image packaging

How the boot image is laid out on this device (MT6833 / GNT-AN00), how the
kernel command line is handled, how the root filesystem is located, and which
pitfalls cost real debugging time.

## 1. Layout

Android header v2, parsed directly by LK. No bootloader changes are involved.

| Field | Offset | Notes |
|---|---|---|
| `ANDROID!` magic | 0x00 | 8 bytes |
| kernel_size | 0x08 | gzip-compressed kernel size |
| ramdisk_size | 0x10 | 0 on this device (ramdisk slot unused) |
| page_size | 0x24 | 2048 |
| dtb_size | 0x670 | size of the appended DTB |
| AVB footer | 0xec6000 | keep byte for byte, never overwrite |

```
[ 0x0000 header (2048) ]
[ kernel (gzip, 2K aligned) ]
[ ramdisk (size 0, placeholder) ]
[ dtb (2K aligned) ]
[ zeros ... ]
[ 0xec6000 AVB footer ]
[ padding to 0x2000000 = 32 MiB ]
```

The kernel start is page aligned and the DTB offset must be computed as
`page + align(kernel_gz) + align(ramdisk)`; do not assume a fixed address.

## 2. Two DTBs, only one of them matters

The image carries two device trees:

1. **built-in DTB** compiled into the kernel `.rodata`
   (`CONFIG_ARM64_USE_BUILTIN_DTB`)
2. **appended DTB** pointed at by `dtb_size` in the header

**LK passes the appended DTB to the kernel**, and its `chosen/bootargs`
overrides the built-in one. Changing only the built-in DTB's cmdline has no
effect on boot - this was hit in practice and cost several flash cycles.

When changing the cmdline, update both, or only the appended one.

To check: decompress the kernel segment for the built-in DTB, compute the
appended offset as above, and grep `root=` in both.

## 3. Locating the root filesystem: why an initramfs

The kernel has a **built-in initramfs** (`CONFIG_INITRAMFS_SOURCE`) whose
statically linked `init` locates and mounts the root partition.

Two reasons:

- The boot image has no ramdisk, and LK only publishes
  `linux,initrd-start/end` inside its own DTB, which the appended DTB
  overrides. Building the initramfs into the kernel guarantees it always runs.
- UFS LUNs enumerate asynchronously, so the 128 GB disk appears as either
  `/dev/sdb` or `/dev/sdc` depending on probe order. A hard-coded
  `root=/dev/sdXNN` hangs whenever the order flips.

### Early lookup limitations

Without an initramfs, the kernel's `early_lookup_bdev()` understands only:

```
PARTUUID=<uuid>
PARTLABEL=<name>
/dev/<name>
<major>:<minor>
```

`root=UUID=` and `root=LABEL=` are **not** among them - those are resolved by a
distribution initramfs. In a "kernel mounts root directly" setup they always
fail.

Our `init` does not rely on them. It reads the ext4 superblock at offset 1024
of every block device and compares `s_uuid` (offset 0x68) and `s_volume_name`
(offset 0x78), then mounts the match and `chroot`s into `/sbin/init`.

### Hard-coded identity inside init

The `init` source contains the target filesystem's identity. **Changing the
root filesystem means changing these three and rebuilding the kernel:**

```c
static const unsigned char root_uuid[16] = { ... };   /* ext4 s_uuid */
static const char root_label[] = "...";               /* ext4 s_volume_name */
#define FALLBACK_DEV "/dev/sdXNN"                     /* last resort */
```

Changing only the kernel cmdline does nothing: whichever `root=` is written,
the boot ends up on the partition hard-coded here.

## 4. Packaging

```sh
# 1. build (after editing the DTS, drop the stale built-in DTB object first)
rm -f arch/arm64/kernel/builtin-dtb.o
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image -j$(nproc)

# 2. swap the kernel into an existing boot image, keeping header, appended DTB
#    and AVB footer intact
python3 tools/replace_kernel.py <base_boot.img> <raw_Image> <out_boot.img>

# 3. also replace the appended DTB when needed
python3 tools/swap_boot_dtb.py <out_boot.img> <new.dtb> <final_boot.img>
```

The result must be exactly **33,554,432 bytes** (the boot partition size). A
smaller file leaves stale bytes at the end of the partition, which makes LK's
verification fail and the device unbootable.

## 5. Pre-flash checklist

Verify in order; do not flash if any check fails:

1. image size == 33,554,432
2. md5 matches the expected value
3. both the built-in and appended DTB point at the intended partition
4. the target filesystem's identity is the one compiled into `init`
   (when using the initramfs)
5. a known-good image is available for rollback

## 6. Debugging "black screen and reboot"

Symptom: logo and bootloader unlock warning appear normally, then the device
goes black and reboots with no kernel output on screen.

That means the failure happens before console initialisation. Common causes:

- the appended DTB was replaced with a locally built one that lacks nodes the
  kernel needs early
- the kernel itself is misconfigured or failed to build correctly

**Change one variable at a time.** Verify a new kernel against the stock
appended DTB first, then test your own DTB separately. Changing both at once
makes the failure unattributable.

## 7. Duplicate filesystem identity

When copying a rootfs image to build a second system, **change its identity
first**:

```sh
tune2fs -L <new-label> -U <new-uuid> <image>
```

Otherwise both partitions answer to the same UUID and label, lookup hits
whichever is scanned first, and the symptom looks like "changing the boot
config does nothing".
