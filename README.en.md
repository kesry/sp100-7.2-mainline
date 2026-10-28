# Linux 7.2 for China Mobile NZONE S7 5G (GNT-AN00 / MT6833)

Mainline Linux port for the NZONE S7 5G (GNT-AN00), MediaTek MT6833
(Dimensity 700), 6 GB RAM, UFS 2.2.

It boots from the stock LK bootloader as an ordinary header-v2 `boot.img`.
No bootloader, modem, or other partition is replaced.

[中文说明](README.md) · [boot image packaging notes](docs/boot-packaging.md) · [GPU bring-up notes](docs/gpu.md)

## Status

| Area | State |
|---|---|
| Display | native `mediatek-drm`, DSI D-PHY, Truly NT35595 panel, colour correct |
| GPU | Mali-G57 MC2 via `panfrost`, MFG0..MFG3 power domains; glamor desktop composition works, 390-955 MHz scaling is automatic |
| KVM | `/dev/kvm`, runs at EL2 with VHE, GICv3 system-register interface |
| Wi-Fi | vendor gen4m driver |
| USB | RNDIS network + ACM serial console |
| Battery | MT6359 fuel gauge, MT6360 charger |
| Crash logs | `ramoops` + `pstore` |

Not working yet: touchscreen (hardware confirmed as Chipone ICNL9911C - the SPI
link and the power/reset sequence are proven, but the chip sits in ROM boot mode
without firmware), audio, camera, modem, and the DRM/KMS page-flip path used by
Wayland compositors (fbcon and direct framebuffer writes work). The PMIC RTC also
fails to probe for lack of an of_node, so time is kept by `fake-hwclock` plus
`systemd-timesyncd`.

## What this port adds

* `arch/arm64/boot/dts/mediatek/mt6833-nzone-gnt-an00.dts` — device tree
* `drivers/gpu/drm/panel/panel-truly-nt35595-nzone.c` — panel driver
* `drivers/pmdomain/mediatek/mt6833-pm-domains.h` — MFG0..MFG3 power domains
* changes in `mtk_dsi`, `mtk_disp_rdma`, `mtk_drm_drv`, `mtk-mutex`,
  `mtk-pm-domains`, `panfrost`, the MT6833 clock drivers, the MIPI DSI PHY,
  `ufs-mediatek` and `mt6397-core`

## Building

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
scripts/config --enable  ARM64_USE_BUILTIN_DTB
scripts/config --set-str ARM64_BUILTIN_DTB_NAME "mediatek/mt6833-nzone-gnt-an00"
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules dtbs
```

The DTB is embedded in the kernel image, so after editing the device tree
remove `arch/arm64/kernel/builtin-dtb.o` before rebuilding, otherwise the old
blob is reused.

See [boot packaging notes](docs/boot-packaging.md) for the boot image layout,
cmdline handling and how the root filesystem is located.

## Display notes

The DDP path is the short `RDMA0 -> DSI0` chain. When the driver takes over it
clears the mutex, so LK's longer pipeline cannot keep compositing its layers
over ours (that produced a static grey veil on top of everything).

Panel timing was read back from the registers LK programs:

| | |
|---|---|
| resolution | 720 x 1600 |
| htotal / vtotal | 788 / 1740 |
| pixel clock | 82,267 kHz → 493.6 Mbps/lane, 4 lanes, RGB888 |
| blanking | VSA 4, VBP 12, VFP 124 |
| HS clock | continuous (HSTX_CKLP_EN clear), as LK runs it |

## GPU notes

**All four MFG power domains are required.** MFG0/MFG1 are the parent islands and
must come up before MFG2/MFG3; declaring only the two core domains makes `mfg2`
report `ON TIMEOUT` and panfrost abort with `Fatal error during GPU init`.
`domain-supply` must sit on each power-domain child node, not on the
power-controller.

**The SRAM power bits are the critical part.** `sram_pdn_bits` /
`sram_pdn_ack_bits` must be enabled for MFG1..MFG3. The Mali job manager lives in
the always-on part and will happily accept and start a job, but the shader and
tiler SRAM only leaves power-down when SPM is told which bits to drive. Without
them every job stays at `JS_STATUS=ACTIVE`, never completes and never reports an
error - which shows up as a black glamor screen while software rendering is fine.
**Do not comment those bits out to work around an `ON TIMEOUT`**; that only turns
a loud power-up failure into a silent execution failure.

**Do not declare `sram-supply`.** The GPU SRAM rail is held on by
`regulator-always-on`; declaring it as a panfrost supply makes the driver reject
the device.

Two further issues were fixed along the way: panfrost never re-enables the MMU
interrupt after runtime resume (an unmapped access then fails silently), and the
`simple_ondemand` defaults of 45/5 are never reached by compositor workloads -
they are now 20/10 and exposed as module parameters.

Full analysis, verification method and pitfalls: [GPU bring-up notes](docs/gpu.md).

## Credits

Display and mutex take-over approach follows the
[a22x-mainline](https://github.com/Dodaskot/a22x-mainline) port for the Samsung
Galaxy A22 5G (same MT6833 SoC).

## License

Kernel code is GPL-2.0, matching upstream.
