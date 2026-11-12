// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_modes.h>
#include <linux/delay.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#include "../mediatek/mediatek_v2/mtk_log.h"
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "include/panel-jdi-panorama.h"
#endif

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

#define LCM_NAME "panel-jdi_panorama"

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;

	int error;
};

#define lcm_dcs_write_seq(ctx, seq...)                         \
	({                                                         \
		const u8 d[] = {seq};                                  \
		BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64,                   \
				 "DCS sequence too big for stack");            \
		lcm_dcs_write(ctx, d, ARRAY_SIZE(d));                  \
	})

#define lcm_dcs_write_seq_static(ctx, seq...)                  \
	({                                                         \
		static const u8 d[] = {seq};                           \
		lcm_dcs_write(ctx, d, ARRAY_SIZE(d));                  \
	})

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

static void lcm_dcs_write(struct lcm *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

static int lcm_dcs_read(struct lcm *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void lcm_panel_get_data(struct lcm *ctx)
{
	u8 panel_id[6] = { 0 };
	const char *stage;
	int ret;

	ret = lcm_dcs_read(ctx, 0xA1, panel_id, sizeof(panel_id));
	if (ret < 0)
		return;

	dev_info(ctx->dev, "panel manufacturing date: %4d/%02d/%02d\n", 2010 + (panel_id[0] >> 4), panel_id[0] & 0xF, panel_id[1] >> 3);
	dev_info(ctx->dev, "panel vendor id: %d\n", panel_id[1] & 7); // 1 = JDI
	dev_info(ctx->dev, "panel MDL: %d\n", panel_id[2] >> 6);
	dev_info(ctx->dev, "panel inspection equipment: %d\n", panel_id[4] >> 4);
	dev_info(ctx->dev, "panel glass manufacturing site: %d\n", panel_id[4] & 0xF);
	dev_info(ctx->dev, "panel manufacturer rev : %d\n", panel_id[5] >> 4);
	switch ((panel_id[5] >> 2) & 0x3)
	{
		case 0: stage = "TS"; break;
		case 1: stage = "ES"; break;
		case 2: stage = "CS"; break;
		case 3: stage = "MP"; break;
	}
	dev_info(ctx->dev, "panel rim status: %s%d\n", stage, (panel_id[5] & 0x3) + 1);
	dev_info(ctx->dev, "panel serial: %d\n", panel_id[3] | ((panel_id[2] & 0x3F) << 8));
}

static void lcm_panel_init(struct lcm *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(60);
	gpiod_set_value(ctx->reset_gpio, 0);
	mdelay(200);
	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(60);

	lcm_panel_get_data(ctx);

	lcm_dcs_write_seq_static(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2c); // set control display
	mdelay(10);
	mipi_dsi_dcs_set_column_address(dsi, 0, FRAME_WIDTH - 1); // set column address: 0 - 1440
	mdelay(10);
	mipi_dsi_dcs_set_page_address(dsi, 0, FRAME_HEIGHT - 1); // set page address: 0 - 1440
	mdelay(10);
	lcm_dcs_write_seq_static(ctx, MIPI_DCS_SET_ADDRESS_MODE, 0x00); // set address mode
	mdelay(10);
	mipi_dsi_dcs_set_pixel_format(dsi, (MIPI_DCS_PIXEL_FMT_24BIT << 4) | MIPI_DCS_PIXEL_FMT_24BIT); // set pixel format
	mdelay(10);
	lcm_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x80); // set display brightness
	mdelay(10);
	lcm_dcs_write_seq_static(ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x00); // set power save
	mdelay(10);
	lcm_dcs_write_seq_static(ctx, MIPI_DCS_SET_CABC_MIN_BRIGHTNESS, 0x00); // set CABC min brightness
	mdelay(10);
	mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mdelay(10);
	mipi_dsi_dcs_exit_sleep_mode(dsi); // exit sleep mode
	mdelay(125);
	mipi_dsi_dcs_set_display_on(dsi); // set display on
	mdelay(10);
}

static int lcm_disable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	pr_err("%s\n", __func__);
	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	if (!ctx->prepared)
		return 0;

	pr_err("%s\n", __func__);
	mipi_dsi_dcs_set_tear_off(dsi);
	mdelay(10);
	mipi_dsi_dcs_set_display_off(dsi);
	mdelay(10);
	mipi_dsi_dcs_enter_sleep_mode(dsi);
	mdelay(150);

	gpiod_set_value(ctx->reset_gpio, 0);
	
	ctx->error = 0;
	ctx->prepared = false;
	
	return 0;
}

static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	pr_err("%s\n", __func__);
	if (ctx->prepared)
		return 0;

	lcm_panel_init(ctx);

	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
#endif

	return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	pr_err("%s\n", __func__);
	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}


#define PCLK_MODE0_IN_KHZ \
      ((FRAME_WIDTH + HFP + HSA + HBP) * (FRAME_HEIGHT + VFP + VSA + VBP) * FPS / 1000)

static const struct drm_display_mode default_mode = {
	.clock = PCLK_MODE0_IN_KHZ,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + HFP,
	.hsync_end = FRAME_WIDTH + HFP + HSA,
	.htotal = FRAME_WIDTH + HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + VFP,
	.vsync_end = FRAME_HEIGHT + VFP + VSA,
	.vtotal = FRAME_HEIGHT + VFP + VSA + VBP,
};


#if defined(CONFIG_MTK_PANEL_EXT)

static struct mtk_panel_params ext_params = {
	.pll_clk = PLL_CLK,
	.data_rate = PLL_CLK*2,
	.data_rate_khz = PLL_CLK*2*1000,
	.vfp_low_power = 750,
	.cust_esd_check = 0,
	.esd_check_enable = 0,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0A, .count = 1, .para_list[0] = 0x9C,
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.ssc_enable = 0,
	.phy_timcon = {
		.clk_hs_post = 0x23,
		.clk_hs_prpr = 0x2a,
	},
	.dyn = {
		.switch_en = 1,
		.pll_clk = PLL_CLK,
	},
};

static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);

	gpiod_set_value(ctx->reset_gpio, on);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	/* Customer test by own ATA tool */
	return 1;
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
		void *handle, unsigned int level)
{
	char bl_tb0[] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0xFF };

	pr_err("%s level %d\n", __func__,level);
	bl_tb0[1] = level;

	if (!cb)
		return -1;

	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
	

	return 0;
}

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.ata_check = panel_ata_check,
};
#endif

static int lcm_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = PHYSICAL_WIDTH / 1000;
	connector->display_info.height_mm = PHYSICAL_HEIGHT / 1000;

	return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;

	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_info("No panel connected,skip probe lcm\n");
				return -ENODEV;
			}
			pr_info("device node name:%s\n", remote_node->name);
		}
	}
	if (remote_node != dev->of_node) {
		pr_info("%s+ skip probe due to not current lcm\n", __func__);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET 
					| MIPI_DSI_CLOCK_NON_CONTINUOUS;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);

	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);

	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	
	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel, dev, &lcm_drm_funcs, DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);

	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif
	
	pr_info("%s-\n", __func__);

	return ret;
}

static int lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);
#if defined(CONFIG_MTK_PANEL_EXT)
	struct mtk_panel_ctx *ext_ctx = find_panel_ctx(&ctx->panel);
#endif

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_detach(ext_ctx);
	mtk_panel_remove(ext_ctx);
#endif
	
	return 0;
}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "jdi,panorama", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver q30_lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = LCM_NAME,
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(q30_lcm_driver);

MODULE_AUTHOR("Tai-Hua Tseng <tai-hua.tseng@mediatek.com>");
MODULE_DESCRIPTION("hodafonehw_panel_general vdo Panel Driver");
MODULE_LICENSE("GPL v2");
