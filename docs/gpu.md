# GPU：MT6833 上的 panfrost（Mali-G57 MC2）

本文说明本设备（MT6833 / GNT-AN00）的 Mali-G57 在主线 `drm/panfrost` 下的
打通过程、为什么最初只有黑屏而没有任何报错，以及怎么验证结果。文中数据全部
来自真机实测，不是从驱动源码推断的。

## 1. 结果

```
(II) modeset(0): glamor X acceleration enabled on Mali-G57 (Panfrost)
```

- 桌面由 GPU 合成（glamor），没有退回软件渲染。
- `gpu sched timeout`：0。
- `panfrost-job` 中断计数：桌面用几分钟后到数千。
- GPU / MMU fault：0。
- devfreq：`simple_ondemand`，空闲 390 MHz，持续负载 955 MHz。

## 2. 最费时间的故障形态

每个作业都被接受，然后永远不完成：

```
panfrost 13000000.gpu: gpu sched timeout, js=0, config=0x7b00, status=0x8,
                       head=0xa005180, tail=0xa005180
```

`status=0x8` 是 `DRM_PANFROST_EXCEPTION_ACTIVE`（见 `panfrost_device.h`；
注意这和 `panfrost_exception_infos[]` 的**数组下标**不是一回事，很容易读错）。
也就是说硬件认为作业正在运行。同时：

- `head == tail` —— 作业已经被从 ring 里取走。
- `JOB_INT_RAWSTAT == 0` —— 没有完成挂起。
- 三个 panfrost 中断计数（`gpu` / `mmu` / `job`）**恒为 0**。
- `gpu fault` / `mmu fault` 计数**恒为 0**。
- 软件渲染（`AccelMethod none`）完全正常。

作业在跑，却没有完成中断，也不报任何错误。

## 3. 根因：GPU 的 SRAM 从来没有上电

`drivers/pmdomain/mediatek/mt6833-pm-domains.h` 里 MFG1/MFG2/MFG3 的 SRAM
电源位是注释掉的：

```c
[MT6833_POWER_DOMAIN_MFG1] = {
	...
	/* .sram_pdn_bits = GENMASK(8, 8), */
	/* .sram_pdn_ack_bits = GENMASK(12, 12), */
	.caps = MTK_SCPD_DOMAIN_SUPPLY,
},
```

Mali 的 job manager 位于常开部分，会照常接受并启动作业；而 shader 和 tiler 的
SRAM 只有在 SPM 被告知要驱动哪些位时才会退出掉电。缺了这两位，执行单元没有
内存可用：作业永不结束，也不产生任何错误。

四个 MFG 域全部启用 `GENMASK(8, 8)` / `GENMASK(12, 12)` 后修复。同 SoC、同为
Linux v7.2 基线的 a22x 树里正是这两组值，这是能定位到根因的关键线索。

**不要用"注释掉 SRAM 位"的办法去绕过 `mfgN: ON TIMEOUT`。** 那只是把一个会
明确报错的上电失败，换成一个更难查的静默执行失败。

## 4. 第二个独立缺陷：MMU 中断永不再使能

```c
panfrost_mmu_suspend_irq()   ->  MMU_INT_MASK = 0    （runtime suspend 时）
panfrost_mmu_reset()         ->  只清 is_suspended，从不 unmask
panfrost_mmu_init()          ->  只注册 IRQ，从不 unmask
```

第一次 runtime suspend 之后 MMU 中断就永久被屏蔽，GPU 访问未映射地址时静默
失败、不产生 fault 中断。修复是在 `panfrost_mmu_reset()` 里重新使能：

```c
mmu_write(pfdev, MMU_INT_CLEAR, ~0);
mmu_write(pfdev, MMU_INT_MASK, ~pfdev->as_faulty_mask);
```

实测：`MMU_INT_MASK` 修复前 `0x00000000`，修复后 `0x00ff00ff`。这是通用缺陷，
与本板无关，适合上报上游。

## 5. devfreq：是自动的，但不响应负载

governor 本来就是 `simple_ondemand`，但频率几乎一直停在最低档，看起来像被钉死。
驱动传的是 `upthreshold=45, downdifferential=5`。桌面合成器在每个 50 ms 采样
窗口里只让 GPU 忙几个百分点，**45% 永远达不到**；偶发的脉冲能越过阈值，但只
维持几十毫秒——`trans_stat` 里只有寥寥几次切换、最高档累计约 100 ms。

把阈值降到 20/10 后变得响应：

```
空闲      : 390 MHz
持续负载  : 955 MHz，连续保持约 13.5 秒（glxgears）
撤掉负载  : 500 ms 内回落到 390 MHz
```

两个值都是模块参数，可以不改代码调整：

```sh
cat /sys/module/panfrost/parameters/upthreshold        # 20
cat /sys/module/panfrost/parameters/downdifferential   # 10
```

## 6. 怎么验证

不要用 `lsmod` 或 Xorg 日志里的 "glamor enabled" 作为判据——黑屏的时候这两项
就已经是"正常"的。用这些：

```sh
# 1. 作业必须完成
dmesg | grep -c 'sched timeout'          # 0
grep panfrost-job /proc/interrupts       # 数值大且在增长

# 2. X 确实在用 GPU
grep 'glamor X acceleration enabled' /var/log/Xorg.0.log

# 3. 频率跟着负载走
cat /sys/class/devfreq/13000000.gpu/cur_freq
cat /sys/class/devfreq/13000000.gpu/trans_stat
```

判断"屏幕上到底有没有内容"要读 debugfs 里的实时扫描缓冲，**不要用 `/dev/fb0`**
（fbdev 模拟缓冲不跟随 DRM/KMS，而且 `dmesg` 输出会经 fbcon 重绘 fb0，制造出
"画面变了"的假象）：

```sh
FB=$(grep -m1 'dma_addr=' /sys/kernel/debug/dri/*/state | grep -oE '0x[0-9a-f]+')
```

扫描缓冲是 DRAM，内核辅助模块必须用 `memremap()`，`ioremap()` 会失败。

## 7. 踩过的坑

### panfrost 是模块，只刷 boot 等于什么都没改

`CONFIG_DRM_PANFROST=m`。驱动在 rootfs 里的
`/usr/lib/modules/$KREL/kernel/drivers/gpu/drm/panfrost/panfrost.ko`，不在
`boot.img` 里。有好几轮"补丁没效果"，实际原因只是重新打包并刷了 boot。
改 panfrost 必须同步更新 rootfs 镜像里的模块（挂载镜像、拷贝 `.ko`、
`depmod -b`）。

**不要在 Xorg 占用 GPU 时热替换模块**，把文件准备好然后冷重启。

### 桌面在跑的时候反复跑 kmscube 会把 DSI 链路搞挂

`kmscube` 会抢占 DRM master 并做完整 modeset；在 `lightdm` 运行期间反复跑，
最终会让显示控制器卡死：

```
mediatek-drm: [drm] Wait DSI IRQ(0x00000002) Timeout
```

现象是屏幕全黑但其他一切"看起来正常"：framebuffer 有内容、`dpms: On`、背光
开着、X 在跑。只有重启能恢复。持续负载请用 X 内的 GL 程序（如 `glxgears`），
或者先停掉显示管理器、只跑一次 kmscube。

### `AccelMethod` 必须和 `kmsdev` 在同一个 Device 段

把 `Option "AccelMethod" "none"` 写到另一个 `xorg.conf.d/` 文件里、单独开一个
`Section "Device"`，会被静默忽略——X 仍然启用 glamor。另外，第二个
`Section "ServerFlags"` 是**替换**第一个而不是合并，会丢掉
`AutoAddGPU "false"` 之类的选项，导致 X 把 KMS 设备又加了一遍：

```
(II) modeset(G0): using drv /dev/dri/card2
(EE) Cannot run in framebuffer mode. Please specify busIDs for all framebuffer devices
```

只保留一个 `ServerFlags` 段，所有选项都写进去。

## 8. 寄存器偏移要查，不要猜

有两轮时间浪费在猜偏移上。真实值在 `panfrost_regs.h`：

```
GPU_ID 0x000  GPU_L2_FEATURES 0x004  GPU_CORE_FEATURES 0x008
GPU_INT_RAWSTAT 0x020  GPU_INT_CLEAR 0x024  GPU_INT_MASK 0x028
GPU_CMD 0x030  GPU_STATUS 0x034
L2_PRESENT_LO 0x120  L2_READY_LO 0x160  L2_PWRON_LO 0x1a0  L2_PWRTRANS_LO 0x220
SHADER_PRESENT_LO 0x100  SHADER_READY_LO 0x140  SHADER_PWRTRANS_LO 0x200
TILER_PRESENT_LO 0x110  TILER_READY_LO 0x150  TILER_PWRTRANS_LO 0x210
JOB_INT_RAWSTAT 0x1000  JOB_INT_MASK 0x1008
JS_BASE 0x1800  JS_SLOT_STRIDE 0x80
  head +0x00  tail +0x08  config +0x18  cmd +0x20  status +0x24
MMU_INT_RAWSTAT 0x2000  MMU_INT_MASK 0x2008
MMU_BASE 0x2400  MMU_AS_SHIFT 6
  AS_TRANSTAB_LO +0x00  AS_COMMAND +0x18  AS_FAULTSTATUS +0x1c
  AS_FAULTADDRESS_LO +0x20  AS_STATUS +0x28  AS_TRANSCFG_LO +0x30
```

`AS_STATUS` 的 bit 0（`AS_ACTIVE`）含义是"有命令在处理中"，**不是**"地址空间
已启用"。`wait_ready()` 轮询的是它变成 **0**，所以读到 0 是健康的空闲状态，
不是错误。

另外 `0x14` 是 `GPU_MMU_FEATURES`、`0x18` 是 `GPU_AS_PRESENT`——把这两个当成
"L2 上电/就绪"会得出一个很有把握但完全错误的结论。

## 9. 依赖的板级配置

```dts
gpu: gpu@13000000 {
	compatible = "mediatek,mt6833-mali", "mediatek,mt8370-mali",
		     "arm,mali-valhall-jm";
	interrupts = <GIC_SPI 366 ...>, <GIC_SPI 365 ...>, <GIC_SPI 364 ...>;
	interrupt-names = "job", "mmu", "gpu";
	clocks = <&mfgcfg_ml CLK_MFGCFG_BG3D>;
	mali-supply = <&mt6359_vproc1_buck_reg>;
	/* 四个 MFG 域缺一不可：MFG0/MFG1 是 MFG2/MFG3 的父岛 */
	power-domains = <&spm 0>, <&spm 1>, <&spm 2>, <&spm 3>;
	power-domain-names = "mfg0", "mfg1", "mfg2", "mfg3";
	operating-points-v2 = <&gpu_opp_table>;
};
```

两个板级要点：

- **不要写 `sram-supply`。** GPU 的 SRAM 电源轨靠 `regulator-always-on` 保持，
  把它声明成 panfrost 的 supply 会让驱动直接拒绝这个设备。
- **四个 MFG 域是必须的。** 只声明 MFG2/MFG3（a22x 的做法）在本机会失败：
  `mfg2: ON TIMEOUT` 之后 `Fatal error during GPU init`，因为 MFG0/MFG1 是父岛。

`panfrost_drv.c` 里的 `mediatek_mt6833_data` 必须配套：`num_pm_domains = 4`，
域名与 DTS 一致，否则 probe 报 `Incorrect number of power domains`。
