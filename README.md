# Linux 7.2 for China Mobile NZONE S7 5G (GNT-AN00 / MT6833)

主线和桌面发行版移植到中国移动 NZONE S7 5G（GNT-AN00，MT6833 / 天玑 700，6 GB RAM，UFS 2.2）。

从原厂 LK 引导，使用普通 header v2 格式的 `boot.img` 启动。**不修改 bootloader、modem 或任何其他分区**。

[English README](README.en.md) · [boot 镜像打包说明](docs/boot-打包说明.md) · [GPU 打通记录](docs/gpu.md)

## 当前状态

| 功能 | 状态 |
|---|---|
| 显示 | 原生 `mediatek-drm`，DSI D-PHY，Truly NT35595 面板，颜色正确 |
| GPU | Mali-G57 MC2，`panfrost` 驱动，MFG0..MFG3 电源域；glamor 桌面合成可用，调频 390–955 MHz 自动 |
| KVM | `/dev/kvm` 可用，运行在 EL2 + VHE，GICv3 系统寄存器接口 |
| Wi-Fi | 厂商 gen4m 驱动 |
| USB | RNDIS 网络 + ACM 串口控制台 |
| 电池 | MT6359 电量计，MT6360 充电 |
| 崩溃日志 | `ramoops` + `pstore` |

尚未工作：触屏（硬件已确认是 Chipone ICNL9911C，SPI 通道与上电复位已打通，但芯片停在 ROM 引导模式缺固件）、音频、摄像头、modem，以及 Wayland 合成器使用的 DRM/KMS 页翻转路径（fbcon 和直接写 framebuffer 正常）。另外 PMIC 的 RTC 因缺少 of_node 无法 probe，时间靠 `fake-hwclock` 加 `systemd-timesyncd`。

## 本移植新增的内容

- `arch/arm64/boot/dts/mediatek/mt6833-nzone-gnt-an00.dts` — 设备树
- `drivers/gpu/drm/panel/panel-truly-nt35595-nzone.c` — 面板驱动
- `drivers/pmdomain/mediatek/mt6833-pm-domains.h` — MFG0..MFG3 电源域
- 修改了 `mtk_dsi`、`mtk_disp_rdma`、`mtk_drm_drv`、`mtk-mutex`、`mtk-pm-domains`、`panfrost`、MT6833 时钟驱动、MIPI DSI PHY、`ufs-mediatek`、`mt6397-core`

## 编译

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
scripts/config --enable  ARM64_USE_BUILTIN_DTB
scripts/config --set-str ARM64_BUILTIN_DTB_NAME "mediatek/mt6833-nzone-gnt-an00"
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules dtbs
```

设备树被编译进内核镜像。修改设备树后必须删掉 `arch/arm64/kernel/builtin-dtb.o` 再重编，否则会复用旧的内置 DTB。

boot 镜像的打包方式、cmdline 与根文件系统定位，见 [boot 打包说明](docs/boot-打包说明.md)。

## 显示部分说明

DDP 路径是短的 `RDMA0 -> DSI0` 链。驱动接管时清除 mutex，这样 LK 那条更长的管线无法继续把它的图层叠加在我们之上（否则会出现一层静态灰色蒙版）。

面板时序从 LK 配置的寄存器读回：

| 项 | 值 |
|---|---|
| 分辨率 | 720 × 1600 |
| htotal / vtotal | 788 / 1740 |
| 像素时钟 | 82,267 kHz → 493.6 Mbps/lane，4 lane，RGB888 |
| 消隐 | VSA 4，VBP 12，VFP 124 |
| HS 时钟 | 连续（HSTX_CKLP_EN 清零），与 LK 一致 |

## GPU 部分说明

**四个 MFG 电源域缺一不可。** MFG0/MFG1 是父岛，必须先上电，MFG2/MFG3 才能起来；
只声明两个核心域会让 `mfg2` 报 `ON TIMEOUT`，随后 panfrost 以
`Fatal error during GPU init` 退出。`domain-supply` 要写在每个 power-domain
子节点上，不能写在电源控制器节点上。

**关键是 SRAM 的上电位。** 电源域表里 MFG1..MFG3 的 `sram_pdn_bits` /
`sram_pdn_ack_bits` 必须是启用的。Mali 的 job manager 在常开部分，会照常接受并
启动作业；但 shader/tiler 的 SRAM 只有 SPM 被告知驱动哪些位时才退出掉电。缺了
这两位，作业永远停在 `JS_STATUS=ACTIVE`，既不完成也不报错，表现为 glamor 黑屏
而软件渲染正常。**不要用"注释掉 SRAM 位"的办法去绕过 `ON TIMEOUT`**，那只会把
明确的上电失败换成更难查的静默执行失败。

**不要写 `sram-supply`。** GPU 的 SRAM 电源轨靠 `regulator-always-on` 保持，
声明成 panfrost 的 supply 会让驱动拒绝这个设备。

另外修了两个独立问题：panfrost 在 runtime resume 后不再使能 MMU 中断（访问未映射
地址时静默失败）；`simple_ondemand` 的默认阈值 45/5 对桌面合成负载永远不触发，
已调到 20/10 并做成模块参数。

完整分析、验证方法和踩过的坑见 [GPU 打通记录](docs/gpu.md)。

## 致谢

显示与 mutex 接管的方法参考了 [a22x-mainline](https://github.com/Dodaskot/a22x-mainline)（三星 Galaxy A22 5G，同为 MT6833）。

## 许可

内核部分遵循 GPL-2.0，与上游一致。
