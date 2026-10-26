// SPDX-License-Identifier: GPL-2.0-only
/*
 * Truly NT35595 (NT50358 TDDI timing controller) WXGA video panel used by
 * the Huawei NZONE S7 (GNT-AN00).
 *
 * Initialization sequence, timings and power GPIOs decoded from the stock
 * EMUI device tree (lcd-kit block "100_a01_6p517"):
 *   720x1600 VIDEO mode, 4 DSI lanes, 520 Mbps/lane, RGB888
 *   HSA=4 HBP=32 HFP=32  VSA=4 VBP=12 VFP=124  60Hz
 *   VSP gpio 137, VSN gpio 138, reset gpio 86 (active per lcd-kit: high)
 * Panel power-on/off command tables: see nzone_on/off_cmds below.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct nzone95 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset;
	bool enabled;
};

enum { SUP_VDD, SUP_VSP, SUP_VSN };

static const struct regulator_bulk_data nzone95_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "vsp" },
	{ .supply = "vsn" },
};

static inline struct nzone95 *to_nzone95(struct drm_panel *panel)
{
	return container_of(panel, struct nzone95, panel);
}

static void nzone95_init(struct nzone95 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* unlock */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x59);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xf1, 0xa5, 0xa6);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xf6, 0x3f);
	/* registers */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0,
				0x82, 0x81, 0x05, 0x04, 0x87, 0x86, 0x84, 0x85,
				0x66, 0x66, 0x33, 0x33, 0x20, 0x01, 0x01, 0x78,
				0x01, 0x01, 0x0f, 0x05, 0x04, 0x03, 0x02, 0x01,
				0x02, 0x03, 0x04, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb1,
				0x11, 0x42, 0x86, 0x00, 0x01, 0x01, 0x01, 0x66,
				0x01, 0x01, 0x04, 0x08, 0x54, 0x00, 0x00, 0x00,
				0x44, 0x40, 0x02, 0x01, 0x40, 0x02, 0x01, 0x40,
				0x02, 0x01, 0x40, 0x02, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb2,
				0x54, 0xc4, 0x82, 0x05, 0x40, 0x02, 0x01, 0x40,
				0x02, 0x01, 0x05, 0x05, 0x54, 0x0c, 0x0c, 0x0d,
				0x0b);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb3,
				0x02, 0x00, 0x00, 0x00, 0x00, 0x26, 0x26, 0x91,
				0xa2, 0x33, 0x44, 0x00, 0x26, 0x00, 0x18, 0x01,
				0x02, 0x08, 0x20, 0x30, 0x08, 0x09, 0x44, 0x20,
				0x40, 0x20, 0x40, 0x08, 0x09, 0x22, 0x33);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb4,
				0x0a, 0x02, 0xdc, 0x1d, 0x00, 0x02, 0x02, 0x02,
				0x02, 0x12, 0x10, 0x02, 0x02, 0x0e, 0x0c, 0x04,
				0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0xff, 0xff,
				0xfc, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb5,
				0x0b, 0x02, 0xdc, 0x1d, 0x00, 0x02, 0x02, 0x02,
				0x02, 0x13, 0x11, 0x02, 0x02, 0x0f, 0x0d, 0x05,
				0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0xff, 0xff,
				0xfc, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb8,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbb,
				0x01, 0x05, 0x09, 0x11, 0x0d, 0x19, 0x1d, 0x55,
				0x25, 0x69, 0x00, 0x21, 0x25);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbc,
				0x00, 0x00, 0x00, 0x00, 0x02, 0x20, 0xff, 0x00,
				0x03, 0x33, 0x01, 0x73, 0x33, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd,
				0xe9, 0x02, 0x4e, 0xcf, 0x72, 0xa4, 0x08, 0x44,
				0xae, 0x15);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbe,
				0x5a, 0x5a, 0x50, 0x46, 0x0c, 0x77, 0x43, 0x07,
				0x0e, 0x0e);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbf,
				0x07, 0x25, 0x07, 0x25, 0x7f, 0x00, 0x11, 0x04);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc0,
				0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff,
				0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc1,
				0xc0, 0x0c, 0x20, 0x7c, 0x04, 0x20, 0x20, 0x04,
				0x2a, 0x40, 0x36, 0x00, 0x07, 0xcf, 0xff, 0xff,
				0x99, 0x01, 0xc0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc2, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc3,
				0x06, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x81,
				0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc4,
				0x84, 0x01, 0x2b, 0x41, 0x00, 0x3c, 0x00, 0x03,
				0x03, 0x2e);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc5,
				0x03, 0x1c, 0xc0, 0xc0, 0x40, 0x10, 0x42, 0x44,
				0x08, 0x09, 0x14);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc6,
				0x87, 0x96, 0x2a, 0x29, 0x29, 0x31, 0x7f, 0x34,
				0x08, 0x04);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc7,
				0xfc, 0xad, 0x8a, 0x6e, 0x3e, 0x1e, 0xf0, 0x48,
				0x16, 0xf3, 0xce, 0xa2, 0xfc, 0xd0, 0xb3, 0x89,
				0x70, 0x4c, 0x1a, 0x7e, 0xc0, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc8,
				0xfc, 0xad, 0x8a, 0x6e, 0x3e, 0x1e, 0xf0, 0x48,
				0x16, 0xf3, 0xce, 0xa2, 0xfc, 0xd0, 0xb3, 0x89,
				0x70, 0x4c, 0x1a, 0x7e, 0xc0, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xcb, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd0,
				0x80, 0x0d, 0xff, 0x0f, 0x61);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd2, 0x42);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xfa, 0x45, 0x93, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xfe, 0xff, 0xff, 0xff, 0x40);
	/* lock */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xf1, 0x5a, 0x59);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa6);
	/* teardown: pixel format, sleep out, display on */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3a, 0x77);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
}

/* Backlight: DCS 0x51 (SET_DISPLAY_BRIGHTNESS), like the stock driver. */
static int nzone95_bl_update(struct backlight_device *bl)
{
	struct nzone95 *ctx = bl_get_data(bl);
	u8 val = backlight_get_brightness(bl);
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, (u8[]){ 0x51, val }, 2);
	if (ret < 0)
		return ret;
	/* BCTRL | DD | BL, as the stock panel driver sets it */
	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, (u8[]){ 0x53, 0x2c }, 2);

	return ret < 0 ? ret : 0;
}

static const struct backlight_ops nzone95_bl_ops = {
	.update_status = nzone95_bl_update,
};

static int nzone95_prepare(struct drm_panel *panel)
{
	struct nzone95 *ctx = to_nzone95(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(nzone95_supplies), ctx->supplies);
	if (ret)
		return ret;

	/* stock power-on stage: iovcc, delay 10ms, vsp/vsn, delay 20ms, reset */
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(2);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(20);

	return 0;
}

static int nzone95_enable(struct drm_panel *panel)
{
	struct nzone95 *ctx = to_nzone95(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	nzone95_init(ctx);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int nzone95_disable(struct drm_panel *panel)
{
	struct nzone95 *ctx = to_nzone95(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int nzone95_unprepare(struct drm_panel *panel)
{
	struct nzone95 *ctx = to_nzone95(panel);

	gpiod_set_value_cansleep(ctx->reset, 0);
	regulator_bulk_disable(ARRAY_SIZE(nzone95_supplies), ctx->supplies);
	return 0;
}

static const struct drm_display_mode nzone95_mode = {
	/*
	 * 720x1600@60, 4 lanes, RGB888, 520 Mbps/lane max.
	 *
	 * Timing verified against the live DSI registers that LK programs
	 * (read with the dsi_dump module):
	 *   DSI_SIZE_CON = 0x064002d0 -> WIDTH 720, HEIGHT 1600
	 *   VSA 4, VBP 12, VFP 124, VACT 1600 -> vtotal 1740
	 *   HSA_WC 4, HBP_WC 88, HFP_WC 84 bytes (RGB888)
	 * which matches this mode's htotal 788 / vtotal 1740 exactly.
	 *
	 * Pixel clock = 788 * 1740 * 60 = 82,267,200 Hz.
	 * mtk_dsi turns that into data_rate = clock * 24 / 4 lanes, i.e.
	 * 493.6 Mbps/lane, comfortably under the 520 Mbps/lane the panel
	 * accepts. The previous value of 161000 kHz asked for 966 Mbps/lane,
	 * 1.86x over spec, and the panel could not lock - only the left part
	 * of each line came out and the rest of the screen stayed black.
	 */
	/*
	 * Pixel clock chosen so mtk_dsi's data_rate lands exactly on the
	 * panel's rated lane rate:
	 *     data_rate = clock * 24bpp / 4 lanes
	 *     520 Mbps  = clock * 6  ->  clock = 86,666,667 Hz
	 * Running below spec (the 82267 that gives 60 Hz -> 493.6 Mbps) made
	 * the panel lose lock in large irregular patches.
	 */
	.clock = 86667,
	.hdisplay = 720,
	.hsync_start = 720 + 32,		/* hfront porch 32 */
	.hsync_end = 720 + 32 + 4,		/* hsync len 4 */
	.htotal = 720 + 32 + 4 + 32,		/* hback porch 32 -> 788 */
	.vdisplay = 1600,
	.vsync_start = 1600 + 124,		/* vfront porch 124 */
	.vsync_end = 1600 + 124 + 4,		/* vsync len 4 */
	.vtotal = 1600 + 124 + 4 + 12,		/* vback porch 12 -> 1740 */
	.width_mm = 68,
	.height_mm = 152,
};

static int nzone95_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &nzone95_mode);
}

static const struct drm_panel_funcs nzone95_panel_funcs = {
	.prepare = nzone95_prepare,
	.enable = nzone95_enable,
	.disable = nzone95_disable,
	.unprepare = nzone95_unprepare,
	.get_modes = nzone95_get_modes,
};

static int nzone95_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nzone95 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct nzone95, panel,
				   &nzone95_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(nzone95_supplies),
					    nzone95_supplies, &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "reset gpio\n");

	ctx->panel.prepare_prev_first = true;

	/* DCS backlight (0x51/0x53) provided by this driver itself */
	ctx->panel.backlight = devm_backlight_device_register(
		dev, dev_name(dev), dev, ctx, &nzone95_bl_ops,
		&(struct backlight_properties) {
			.type = BACKLIGHT_RAW,
			.scale = BACKLIGHT_SCALE_NON_LINEAR,
			.max_brightness = 255,
			.brightness = 200,
		});
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "backlight register\n");

	drm_panel_add(&ctx->panel);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/*
	 * Continuous HS clock, matching LK: DSI_TXRX_CTRL must read
	 * 0x0000003c (HSTX_CKLP_EN clear). With MIPI_DSI_CLOCK_NON_CONTINUOUS
	 * the driver sets BIT(16) and the panel loses part of every line.
	 */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM;
	dsi->hs_rate = 520000000;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "dsi attach failed\n");
	}

	return 0;
}

static void nzone95_remove(struct mipi_dsi_device *dsi)
{
	struct nzone95 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id nzone95_of_match[] = {
	{ .compatible = "truly,nt35595-nzone" },
	{ }
};
MODULE_DEVICE_TABLE(of, nzone95_of_match);

static struct mipi_dsi_driver nzone95_driver = {
	.probe = nzone95_probe,
	.remove = nzone95_remove,
	.driver = {
		.name = "panel-truly-nt35595-nzone",
		.of_match_table = nzone95_of_match,
	},
};
module_mipi_dsi_driver(nzone95_driver);

MODULE_DESCRIPTION("Truly NT35595 panel for Huawei NZONE S7 (GNT-AN00)");
MODULE_LICENSE("GPL");
