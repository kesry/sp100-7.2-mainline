# boot 镜像打包说明

本文说明本设备（MT6833 / GNT-AN00）的 boot 镜像结构、cmdline 与 rootfs 定位方式，以及打包时容易踩的坑。

## 1. boot 镜像结构

设备使用 Android header v2 格式，LK 直接解析，没有额外的 bootloader 改动。

| 字段 | 偏移 | 说明 |
|---|---|---|
| `ANDROID!` magic | 0x00 | 8 字节 |
| kernel_size | 0x08 | gzip 压缩后的内核大小 |
| ramdisk_size | 0x10 | 本设备为 0（不使用 ramdisk 槽） |
| page_size | 0x24 | 2048 |
| dtb_size | 0x670 | 附加 DTB 大小 |
| AVB footer | 0xec6000 | 保持不变，不要覆盖 |

布局：

```
[ 0x0000 header (2048) ]
[ kernel (gzip, 2K 对齐) ]
[ ramdisk (size 0, 占位) ]
[ dtb (2K 对齐) ]
[ zeros ... ]
[ 0xec6000 AVB footer ]
[ 填充到 0x2000000 = 32 MiB ]
```

关键：**内核在 gzip 之后仍按 2K 对齐**，DTB 的偏移要按 `page + align(kernel_gz) + align(ramdisk)` 计算，不要假设固定值。

## 2. 两处 DTB，只有一处真正生效

镜像里存在两个设备树：

1. **内置 DTB**：编译进内核 `.rodata`（`CONFIG_ARM64_USE_BUILTIN_DTB`）
2. **附加 DTB**：header v2 里 `dtb_size` 指向的那段

**LK 传给内核的是附加 DTB**，它的 `chosen/bootargs` 会覆盖内置的那份。只改内置 DTB 的 cmdline 对启动没有任何影响——这个坑实际踩过，浪费了多轮刷写。

改 cmdline 时两处都要同步，或者干脆只改附加 DTB。

检查方法：解压内核段拿内置 DTB，再按上面的偏移算附加 DTB，两份都 grep `root=` 确认。

## 3. 根文件系统定位：为什么用 initramfs

本设备的内核**内置 initramfs**（`CONFIG_INITRAMFS_SOURCE`），由一个静态链接的 `init` 程序负责找到并挂载根分区。

原因有两个：

- boot 镜像没有 ramdisk，LK 只在它自己的 DTB 里发布 `linux,initrd-start/end`，而那个 DTB 会被附加 DTB 覆盖；把 initramfs 编进内核才能保证一定运行。
- UFS 的 LUN 异步枚举，128 GB 磁盘可能表现为 `/dev/sdb` 或 `/dev/sdc`，写死 `root=/dev/sdXNN` 会在枚举顺序变化时挂死。

### 早期解析的限制

没有 initramfs 时，内核的 `early_lookup_bdev()` 只认这几种写法：

```
PARTUUID=<uuid>
PARTLABEL=<name>
/dev/<name>
<major>:<minor>
```

`root=UUID=` 和 `root=LABEL=` 不在其中（它们由发行版的 initramfs 脚本解析）。所以在这种"内核直接挂根"的场景里写 `root=LABEL=` 必然失败。

我们的 `init` 不依赖这些：它自己读每个块设备偏移 1024 处的 ext4 超级块，比对 `s_uuid`（偏移 0x68）和 `s_volume_name`（偏移 0x78），命中后挂载并 `chroot` 切换到 `/sbin/init`。

### init 里写死的身份

`init` 源码里包含目标文件系统的 UUID 和卷标。**切换根文件系统时必须改这三处并重新编译内核**：

```c
static const unsigned char root_uuid[16] = { ... };   /* ext4 s_uuid */
static const char root_label[] = "...";               /* ext4 s_volume_name */
#define FALLBACK_DEV "/dev/sdXNN"                     /* 最后的兜底 */
```

如果只改内核 cmdline 而不改这里，无论 cmdline 写什么，都会挂到 init 里写死的那个分区上。

## 4. 打包流程

```sh
# 1. 编译内核（改了 DTS 必须先删掉旧的内置 DTB 对象）
rm -f arch/arm64/kernel/builtin-dtb.o
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image -j$(nproc)

# 2. 用现有 boot 镜像作模板替换内核
#    工具：替换内核并保持 header/附加 DTB/footer 不变
python3 tools/replace_kernel.py <base_boot.img> <raw_Image> <out_boot.img>

# 3. 需要同时更新附加 DTB 时
python3 tools/swap_boot_dtb.py <out_boot.img> <new.dtb> <final_boot.img>
```

产物大小必须是 **33,554,432 字节**（boot 分区大小）。小于这个值时，刷入后分区尾部残留旧数据会导致 LK 校验失败、无法开机。

## 5. 刷写前的检查清单

按顺序确认，任一条不满足就不要刷：

1. 镜像大小 == 33,554,432
2. md5 与预期一致
3. 内置 DTB 和附加 DTB 的 `root=`/身份都指向目标分区
4. 目标文件系统的 UUID 确实存在于 `init` 里（如果走 initramfs）
5. 有可回退的旧镜像且已验证可用

## 6. 排查"黑屏重启"

现象：logo 和 bootloader 解锁提示正常，进入内核后立刻黑屏重启，屏幕上没有任何内核日志。

这个现象说明失败发生在控制台初始化之前。常见原因：

- 附加 DTB 换成了自己编译的版本，缺少原厂 DTB 中内核早期需要的节点
- 内核本身编译失败或配置不完整

**排查纪律：一次只改一个变量。** 先用原厂附加 DTB + 新内核验证，确认内核没问题，再单独验证自己的 DTB。两个变量一起改，无法判断是哪个引起的。

## 7. 文件系统身份不能重复

复制一个 rootfs 镜像去做另一套系统时，**必须先改身份**：

```sh
tune2fs -L <新卷标> -U <新UUID> <镜像>
```

否则两个分区的 UUID 和卷标完全相同，按 UUID/卷标查找时会命中先被扫到的那个，表现为"改了半天还是在启动旧系统"。
