// SPDX-License-Identifier: GPL-2.0+
/*
 * Mediatek Watchdog Driver
 *
 * Copyright (C) 2014 Matthias Brugger
 *
 * Matthias Brugger <matthias.bgg@gmail.com>
 *
 * Based on sunxi_wdt.c
 */

#include <dt-bindings/reset/mt2712-resets.h>
#include <dt-bindings/reset/mediatek,mt6735-wdt.h>
#include <dt-bindings/reset/mediatek,mt6795-resets.h>
#include <dt-bindings/reset/mt7986-resets.h>
#include <dt-bindings/reset/mt8183-resets.h>
#include <dt-bindings/reset/mt8186-resets.h>
#include <dt-bindings/reset/mt8188-resets.h>
#include <dt-bindings/reset/mt8192-resets.h>
#include <dt-bindings/reset/mt8195-resets.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/reboot.h>
#include <linux/interrupt.h>

#define WDT_MAX_TIMEOUT		31
#define WDT_MIN_TIMEOUT		2
#define WDT_LENGTH_TIMEOUT(n)	((n) << 5)

#define WDT_LENGTH		0x04
#define WDT_LENGTH_KEY		0x8

#define WDT_RST			0x08
#define WDT_RST_RELOAD		0x1971

#define WDT_MODE		0x00
#define WDT_MODE_EN		(1 << 0)
#define WDT_MODE_EXT_POL_LOW	(0 << 1)
#define WDT_MODE_EXT_POL_HIGH	(1 << 1)
#define WDT_MODE_EXRST_EN	(1 << 2)
#define WDT_MODE_IRQ_EN		(1 << 3)
#define WDT_MODE_AUTO_START	(1 << 4)
#define WDT_MODE_DUAL_EN	(1 << 6)
#define WDT_MODE_CNT_SEL	(1 << 8)
#define WDT_MODE_KEY		0x22000000

#define WDT_SWRST		0x14
#define WDT_SWRST_KEY		0x1209

#define WDT_SWSYSRST		0x18U
#define WDT_SWSYS_RST_KEY	0x88000000

#define WDT_SWSYSRST_EN		0xfc

#define DRV_NAME		"mtk-wdt"
#define DRV_VERSION		"1.0"

#define MT7988_TOPRGU_SW_RST_NUM	24

static bool nowayout = WATCHDOG_NOWAYOUT;
static unsigned int timeout;
/* Explicitly armed for short suspend bring-up tests; off for normal sleep. */
static bool suspend_test_failsafe;
module_param(suspend_test_failsafe, bool, 0600);
MODULE_PARM_DESC(suspend_test_failsafe,
		"Keep an already running watchdog armed during short suspend tests");

struct mtk_wdt_dev {
	struct watchdog_device wdt_dev;
	void __iomem *wdt_base;
	spinlock_t lock; /* protects WDT_SWSYSRST reg */
	struct reset_controller_dev rcdev;
	bool disable_wdt_extrst;
	bool reset_by_toprgu;
	bool has_swsysrst_en;
};

struct mtk_wdt_data {
	int toprgu_sw_rst_num;
	bool has_swsysrst_en;
};

static const struct mtk_wdt_data mt2712_data = {
	.toprgu_sw_rst_num = MT2712_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt6735_data = {
	.toprgu_sw_rst_num = MT6735_TOPRGU_RST_NUM,
};

static const struct mtk_wdt_data mt6795_data = {
	.toprgu_sw_rst_num = MT6795_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt7986_data = {
	.toprgu_sw_rst_num = MT7986_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt7988_data = {
	.toprgu_sw_rst_num = MT7988_TOPRGU_SW_RST_NUM,
	.has_swsysrst_en = true,
};

static const struct mtk_wdt_data mt8183_data = {
	.toprgu_sw_rst_num = MT8183_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8186_data = {
	.toprgu_sw_rst_num = MT8186_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8188_data = {
	.toprgu_sw_rst_num = MT8188_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8192_data = {
	.toprgu_sw_rst_num = MT8192_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8195_data = {
	.toprgu_sw_rst_num = MT8195_TOPRGU_SW_RST_NUM,
};

/**
 * toprgu_reset_sw_en_unlocked() - enable/disable software control for reset bit
 * @data: Pointer to instance of driver data.
 * @id: Bit number identifying the reset to be enabled or disabled.
 * @enable: If true, enable software control for that bit, disable otherwise.
 *
 * Context: The caller must hold lock of struct mtk_wdt_dev.
 */
static void toprgu_reset_sw_en_unlocked(struct mtk_wdt_dev *data,
					unsigned long id, bool enable)
{
	u32 tmp;

	tmp = readl(data->wdt_base + WDT_SWSYSRST_EN);
	if (enable)
		tmp |= BIT(id);
	else
		tmp &= ~BIT(id);

	writel(tmp, data->wdt_base + WDT_SWSYSRST_EN);
}

static int toprgu_reset_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	unsigned int tmp;
	unsigned long flags;
	struct mtk_wdt_dev *data =
		 container_of(rcdev, struct mtk_wdt_dev, rcdev);

	spin_lock_irqsave(&data->lock, flags);

	if (assert && data->has_swsysrst_en)
		toprgu_reset_sw_en_unlocked(data, id, true);

	tmp = readl(data->wdt_base + WDT_SWSYSRST);
	if (assert)
		tmp |= BIT(id);
	else
		tmp &= ~BIT(id);
	tmp |= WDT_SWSYS_RST_KEY;
	writel(tmp, data->wdt_base + WDT_SWSYSRST);

	if (!assert && data->has_swsysrst_en)
		toprgu_reset_sw_en_unlocked(data, id, false);

	spin_unlock_irqrestore(&data->lock, flags);

	return 0;
}

static int toprgu_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return toprgu_reset_update(rcdev, id, true);
}

static int toprgu_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return toprgu_reset_update(rcdev, id, false);
}

static int toprgu_reset(struct reset_controller_dev *rcdev,
			unsigned long id)
{
	int ret;

	ret = toprgu_reset_assert(rcdev, id);
	if (ret)
		return ret;

	return toprgu_reset_deassert(rcdev, id);
}

static const struct reset_control_ops toprgu_reset_ops = {
	.assert = toprgu_reset_assert,
	.deassert = toprgu_reset_deassert,
	.reset = toprgu_reset,
};

static int toprgu_register_reset_controller(struct platform_device *pdev,
					    int rst_num)
{
	int ret;
	struct mtk_wdt_dev *mtk_wdt = platform_get_drvdata(pdev);

	spin_lock_init(&mtk_wdt->lock);

	mtk_wdt->rcdev.owner = THIS_MODULE;
	mtk_wdt->rcdev.nr_resets = rst_num;
	mtk_wdt->rcdev.ops = &toprgu_reset_ops;
	mtk_wdt->rcdev.of_node = pdev->dev.of_node;
	ret = devm_reset_controller_register(&pdev->dev, &mtk_wdt->rcdev);
	if (ret != 0)
		dev_err(&pdev->dev,
			"couldn't register wdt reset controller: %d\n", ret);
	return ret;
}

static int mtk_wdt_restart(struct watchdog_device *wdt_dev,
			   unsigned long action, void *data)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;
	u32 reg;

	wdt_base = mtk_wdt->wdt_base;

	/* Enable reset in order to issue a system reset instead of an IRQ */
	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_IRQ_EN;
	writel(reg | WDT_MODE_KEY, wdt_base + WDT_MODE);

	while (1) {
		writel(WDT_SWRST_KEY, wdt_base + WDT_SWRST);
		mdelay(5);
	}

	return 0;
}


static int mtk_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;

	iowrite32(WDT_RST_RELOAD, wdt_base + WDT_RST);

	return 0;
}

static int mtk_wdt_set_timeout(struct watchdog_device *wdt_dev,
				unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	wdt_dev->timeout = timeout;
	/*
	 * In dual mode, irq will be triggered at timeout / 2
	 * the real timeout occurs at timeout
	 */
	if (wdt_dev->pretimeout)
		wdt_dev->pretimeout = timeout / 2;

	/*
	 * One bit is the value of 512 ticks
	 * The clock has 32 KHz
	 */
	reg = WDT_LENGTH_TIMEOUT((timeout - wdt_dev->pretimeout) << 6)
			| WDT_LENGTH_KEY;
	iowrite32(reg, wdt_base + WDT_LENGTH);

	mtk_wdt_ping(wdt_dev);

	return 0;
}

/*
 * MTK_WDT_HIGH_PRIO_RESTART
 *
 * machine_restart() -> do_kernel_restart() walks the restart notifier chain in
 * descending priority order. PSCI registers psci_sys_reset at priority 129,
 * and the watchdog core registers this driver's ->restart at
 * SYS_OFF_PRIO_DEFAULT (0) - so the firmware call always gets the first shot.
 *
 * On this board that firmware call intermittently never returns: the kernel
 * sits inside the SMC, "Reboot failed -- System halted" is never printed
 * because mtk_wdt_restart()'s loop is never reached, the watchdog has already
 * been poked by systemd-shutdown, and the machine only comes back when
 * somebody holds the power button.
 *
 * mtk_wdt_restart() itself cannot get stuck - it writes WDT_SWRST in a loop -
 * so run it *before* PSCI instead of after it. SYS_OFF_PRIO_HIGH (192) beats
 * psci_sys_reset's 129. Combined with EXRST_EN being set in mtk_wdt_start(),
 * TOP RGU can actually assert the external system reset.
 */
static int mtk_wdt_high_prio_restart(struct sys_off_data *data)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(data->dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	/*
	 * MTK_WDT_ARM_BEFORE_PSCI
	 *
	 * Do not reset the SoC here. Just make sure the watchdog is armed with
	 * a short timeout, then let the chain continue to psci_sys_reset().
	 *
	 * The problem being solved: the PSCI reset SMC intermittently never
	 * returns. Anything that runs *after* it never runs at all, and
	 * systemd-shutdown has already poked the watchdog, so nothing is left
	 * that can reset the machine - it needs a hand on the power button.
	 *
	 * Arming the counter before the call fixes that without changing the
	 * fast path: if PSCI resets the SoC the watchdog never expires; if the
	 * SMC hangs, the TOP RGU counter runs out on its own and resets the
	 * SoC from hardware. Nothing in that path goes through firmware or a
	 * software register write, so it cannot get stuck the same way.
	 *
	 * WDT_MODE_EN | WDT_MODE_EXRST_EN is the combination measured to
	 * actually assert the external reset on this board (stock bootloader
	 * leaves 0x7d).
	 */
	mtk_wdt_set_timeout(&mtk_wdt->wdt_dev, 10);

	reg = ioread32(wdt_base + WDT_MODE);
	reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	reg |= WDT_MODE_EN | WDT_MODE_EXRST_EN;
	writel(reg | WDT_MODE_KEY, wdt_base + WDT_MODE);

	mtk_wdt_ping(&mtk_wdt->wdt_dev);

	/*
	 * Return NOTIFY_DONE so the rest of the chain - in particular
	 * psci_sys_reset() - still runs. The watchdog is now a real deadline
	 * behind it.
	 */
	return NOTIFY_DONE;
}


static void mtk_wdt_init(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;

	wdt_base = mtk_wdt->wdt_base;

	if (readl(wdt_base + WDT_MODE) & WDT_MODE_EN) {
		set_bit(WDOG_HW_RUNNING, &wdt_dev->status);
		mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	}
}

static int mtk_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_EN;
	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	return 0;
}

static int mtk_wdt_start(struct watchdog_device *wdt_dev)
{
	u32 reg;
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	int ret;

	ret = mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	if (ret < 0)
		return ret;

	reg = ioread32(wdt_base + WDT_MODE);
	if (wdt_dev->pretimeout)
		reg |= (WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	else
		reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	/*
	 * MTK_WDT_ENABLE_EXTRST
	 *
	 * Set EXRST_EN unless the board explicitly disabled it.
	 *
	 * The previous code only ever *cleared* this bit, never set it. Since
	 * arch/arm64/kernel/setup.c clears WDT_MODE outright at boot
	 * (CONFIG_MT6833_DISABLE_WDT) and mtk_wdt_start() then only ORs in
	 * EN, the register ended up as:
	 *
	 *   WDT_MODE = 0x00000001     EN=1, EXRST_EN=0
	 *
	 * measured on hardware with a /dev/mem peek. With EXRST_EN clear the
	 * TOP RGU never asserts the external system reset, so an expired
	 * watchdog cannot reset the SoC at all - which is why a wedged
	 * machine_restart() stayed wedged forever even though the watchdog
	 * looked armed.
	 *
	 * The stock bootloader leaves WDT_MODE = 0x7d
	 * (EN | EXRST_EN | IRQ_EN | AUTO_START | bit5 | DUAL_EN), i.e. with
	 * EXRST_EN set, and that is the only configuration with evidence of
	 * actually resetting this SoC.
	 */
	if (mtk_wdt->disable_wdt_extrst)
		reg &= ~WDT_MODE_EXRST_EN;
	else
		reg |= WDT_MODE_EXRST_EN;
	if (mtk_wdt->reset_by_toprgu)
		reg |= WDT_MODE_CNT_SEL;
	reg |= (WDT_MODE_EN | WDT_MODE_KEY);
	iowrite32(reg, wdt_base + WDT_MODE);

	return 0;
}

static int mtk_wdt_set_pretimeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdd);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg = ioread32(wdt_base + WDT_MODE);

	if (timeout && !wdd->pretimeout) {
		wdd->pretimeout = wdd->timeout / 2;
		reg |= (WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	} else if (!timeout && wdd->pretimeout) {
		wdd->pretimeout = 0;
		reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	} else {
		return 0;
	}

	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	return mtk_wdt_set_timeout(wdd, wdd->timeout);
}

static irqreturn_t mtk_wdt_isr(int irq, void *arg)
{
	struct watchdog_device *wdd = arg;

	watchdog_notify_pretimeout(wdd);

	return IRQ_HANDLED;
}

static const struct watchdog_info mtk_wdt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_info mtk_wdt_pt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_PRETIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops mtk_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= mtk_wdt_start,
	.stop		= mtk_wdt_stop,
	.ping		= mtk_wdt_ping,
	.set_timeout	= mtk_wdt_set_timeout,
	.set_pretimeout	= mtk_wdt_set_pretimeout,
	.restart	= mtk_wdt_restart,
};

static int mtk_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_wdt_dev *mtk_wdt;
	const struct mtk_wdt_data *wdt_data;
	int err, irq;

	mtk_wdt = devm_kzalloc(dev, sizeof(*mtk_wdt), GFP_KERNEL);
	if (!mtk_wdt)
		return -ENOMEM;

	platform_set_drvdata(pdev, mtk_wdt);

	mtk_wdt->wdt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mtk_wdt->wdt_base))
		return PTR_ERR(mtk_wdt->wdt_base);

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		err = devm_request_irq(&pdev->dev, irq, mtk_wdt_isr, 0, "wdt_bark",
				       &mtk_wdt->wdt_dev);
		if (err)
			return err;

		mtk_wdt->wdt_dev.info = &mtk_wdt_pt_info;
		mtk_wdt->wdt_dev.pretimeout = WDT_MAX_TIMEOUT / 2;
	} else {
		if (irq == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		mtk_wdt->wdt_dev.info = &mtk_wdt_info;
	}

	mtk_wdt->wdt_dev.ops = &mtk_wdt_ops;
	mtk_wdt->wdt_dev.timeout = WDT_MAX_TIMEOUT;
	mtk_wdt->wdt_dev.max_hw_heartbeat_ms = WDT_MAX_TIMEOUT * 1000;
	mtk_wdt->wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
	mtk_wdt->wdt_dev.parent = dev;

	watchdog_init_timeout(&mtk_wdt->wdt_dev, timeout, dev);
	watchdog_set_nowayout(&mtk_wdt->wdt_dev, nowayout);
	watchdog_set_restart_priority(&mtk_wdt->wdt_dev, 128);

	watchdog_set_drvdata(&mtk_wdt->wdt_dev, mtk_wdt);

	mtk_wdt_init(&mtk_wdt->wdt_dev);

	/*
	 * MTK_WDT_KEEP_ARMED_ON_REBOOT
	 *
	 * Deliberately NOT calling watchdog_stop_on_reboot().
	 *
	 * That helper registers a reboot notifier that calls ->stop() when it
	 * sees SYS_DOWN - and SYS_RESTART is literally defined as SYS_DOWN in
	 * include/linux/reboot.h, so it fires on every reboot, not only on
	 * poweroff. mtk_wdt_stop() then clears WDT_MODE_EN and the watchdog is
	 * disarmed before machine_restart() is even reached.
	 *
	 * That is fatal here, because machine_restart() ends in
	 * do_kernel_restart(), and the second handler in that chain -
	 * mtk_wdt_restart(), priority 128 below PSCI's 129 - is an infinite
	 * loop:
	 *
	 *     while (1) {
	 *             writel(WDT_SWRST_KEY, wdt_base + WDT_SWRST);
	 *             mdelay(5);
	 *     }
	 *
	 * It only clears WDT_MODE_IRQ_EN, never re-arms EN. So when the PSCI
	 * SYSTEM_RESET call returns instead of resetting the SoC, the kernel
	 * spins in that loop with the watchdog already switched off and nothing
	 * left that can reset the machine. "Reboot failed -- System halted" is
	 * never printed because the loop never exits.
	 *
	 * Leaving the watchdog armed makes that fallback work: the PSCI call
	 * still gets the first shot at a fast reset, and if it fails the TOP
	 * RGU expires a few seconds later and resets the SoC. Combined with
	 * EXRST_EN being set in mtk_wdt_start(), which the driver previously
	 * only ever cleared, the watchdog can now actually assert the external
	 * system reset.
	 *
	 * This is safe for data: systemd-shutdown only reaches
	 * machine_restart() after unmounting every filesystem, remounting the
	 * root read-only, syncing and flushing the SCSI caches.
	 */
	watchdog_stop_ping_on_suspend(&mtk_wdt->wdt_dev);
	err = devm_watchdog_register_device(dev, &mtk_wdt->wdt_dev);
	if (unlikely(err))
		return err;

	/*
	 * MTK_WDT_ARM_BEFORE_PSCI - see the comment on
	 * mtk_wdt_high_prio_restart(). Registered at priority 192 so it runs
	 * before psci_sys_reset (129); it arms the watchdog and returns, the
	 * reset itself is still done by PSCI when that works.
	 */
	/*
	 * MTK_WDT_RESTART_AFTER_PSCI
	 *
	 * Register the TOP RGU restart handler just below psci_sys_reset's
	 * priority of 129, so the intended order is: PSCI gets the first
	 * attempt at a fast reset, TOP RGU resets the SoC if it fails.
	 *
	 * priority 192 (SYS_OFF_PRIO_HIGH) was tried first and does stop the
	 * wedging, but it makes every reboot a cold reset because TOP RGU
	 * reinitialises DRAM - ~56 s instead of ~32 s. Running PSCI first
	 * keeps the fast path and leaves the watchdog as a fallback, which is
	 * what it should be.
	 *
	 * This only works because reboot=warm makes psci_sys_reset() issue
	 * SYSTEM_RESET2 instead of SYSTEM_RESET; the latter is the call that
	 * intermittently never returns on this board.
	 */
	err = devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART,
					    SYS_OFF_PRIO_HIGH,
					    mtk_wdt_high_prio_restart, NULL);
	if (err) {
		dev_warn(dev, "failed to register high priority restart handler: %d\n",
			 err);
	} else {
		dev_info(dev, "registered high priority restart handler\n");
	}

	dev_info(dev, "Watchdog enabled (timeout=%d sec, nowayout=%d)\n",
		 mtk_wdt->wdt_dev.timeout, nowayout);

	wdt_data = of_device_get_match_data(dev);
	if (wdt_data) {
		err = toprgu_register_reset_controller(pdev,
						       wdt_data->toprgu_sw_rst_num);
		if (err)
			return err;

		mtk_wdt->has_swsysrst_en = wdt_data->has_swsysrst_en;
	}

	mtk_wdt->disable_wdt_extrst =
		of_property_read_bool(dev->of_node, "mediatek,disable-extrst");

	mtk_wdt->reset_by_toprgu =
		of_property_read_bool(dev->of_node, "mediatek,reset-by-toprgu");

	return 0;
}

static int mtk_wdt_suspend(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&mtk_wdt->wdt_dev) ||
	    watchdog_hw_running(&mtk_wdt->wdt_dev)) {
		if (READ_ONCE(suspend_test_failsafe)) {
			/* The core still pauses its worker. A hung test therefore
			 * expires the hardware timer rather than needing a key reset.
			 */
			mtk_wdt_start(&mtk_wdt->wdt_dev);
			mtk_wdt_ping(&mtk_wdt->wdt_dev);
			dev_info(dev, "suspend test watchdog armed: %u seconds, mode=%#x\n",
				 mtk_wdt->wdt_dev.timeout,
				 readl(mtk_wdt->wdt_base + WDT_MODE));
			return 0;
		}
		mtk_wdt_stop(&mtk_wdt->wdt_dev);
		dev_dbg(dev, "watchdog stopped for system suspend: mode=%#x\n",
			readl(mtk_wdt->wdt_base + WDT_MODE));
	}

	return 0;
}

static int mtk_wdt_resume(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&mtk_wdt->wdt_dev) ||
	    watchdog_hw_running(&mtk_wdt->wdt_dev)) {
		mtk_wdt_start(&mtk_wdt->wdt_dev);
		mtk_wdt_ping(&mtk_wdt->wdt_dev);
		dev_dbg(dev, "watchdog restored after system resume: mode=%#x\n",
			readl(mtk_wdt->wdt_base + WDT_MODE));
	}

	return 0;
}

static const struct of_device_id mtk_wdt_dt_ids[] = {
	{ .compatible = "mediatek,mt2712-wdt", .data = &mt2712_data },
	{ .compatible = "mediatek,mt6589-wdt" },
	{ .compatible = "mediatek,mt6735-wdt", .data = &mt6735_data },
	{ .compatible = "mediatek,mt6795-wdt", .data = &mt6795_data },
	{ .compatible = "mediatek,mt7986-wdt", .data = &mt7986_data },
	{ .compatible = "mediatek,mt7988-wdt", .data = &mt7988_data },
	{ .compatible = "mediatek,mt8183-wdt", .data = &mt8183_data },
	{ .compatible = "mediatek,mt8186-wdt", .data = &mt8186_data },
	{ .compatible = "mediatek,mt8188-wdt", .data = &mt8188_data },
	{ .compatible = "mediatek,mt8192-wdt", .data = &mt8192_data },
	{ .compatible = "mediatek,mt8195-wdt", .data = &mt8195_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_wdt_dt_ids);

static DEFINE_SIMPLE_DEV_PM_OPS(mtk_wdt_pm_ops,
				mtk_wdt_suspend, mtk_wdt_resume);

static struct platform_driver mtk_wdt_driver = {
	.probe		= mtk_wdt_probe,
	.driver		= {
		.name		= DRV_NAME,
		.pm		= pm_sleep_ptr(&mtk_wdt_pm_ops),
		.of_match_table	= mtk_wdt_dt_ids,
	},
};

module_platform_driver(mtk_wdt_driver);

module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Brugger <matthias.bgg@gmail.com>");
MODULE_DESCRIPTION("Mediatek WatchDog Timer Driver");
MODULE_VERSION(DRV_VERSION);
