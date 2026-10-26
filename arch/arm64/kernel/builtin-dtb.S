/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Embed the board device tree into the kernel image.
 *
 * The generic KERNEL_DTB() placement lives inside the __init region, which
 * free_initmem() frees and *unmaps*. Anything that reads the tree after that
 * point (notably /proc/device-tree) then faults in the freed region.
 *
 * Put it in .rodata instead, which stays mapped for the lifetime of the
 * kernel, and expose our own symbols so setup.c does not depend on
 * __dtb_start's section placement.
 */
	.section .rodata.mt6833_dtb, "a"
	.balign 8
	.global __mt6833_dtb_begin
__mt6833_dtb_begin:
	.incbin "arch/arm64/boot/dts/mediatek/mt6833-nzone-gnt-an00.dtb"
	.global __mt6833_dtb_end
__mt6833_dtb_end:
	.balign 8
