// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/backlight.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>
#include <linux/delay.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>

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
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#endif

#include "dsi-panel-mot-boe-nt37701a-655-fhdp-video-144hz-lhbm-alpha.h"
#include "manaus-hbm-brightness-mapping-to-1200nits.h"

#define SUPPORT_144HZ_REFRESH_RATE
//#define DSC_DISABLE
//#define LCM_VDO_MODE
#define DSC_10BIT
#ifdef DSC_10BIT
#define DSC_BITS	10
#define DSC_CFG		2088//2088//40
#else
#define DSC_BITS	8
#define DSC_CFG		34
#endif

extern unsigned int mipi_drive_volt;

enum panel_version{
	PANEL_V1 = 1,
	PANEL_V2,
};

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	bool prepared;
	bool enabled;
	bool lhbm_en;

	int error;
	unsigned int hbm_mode;
	unsigned int dc_mode;
	unsigned int current_bl;
	enum panel_version version;
	unsigned int current_fps;
};

struct lcm *g_ctx = NULL;

#define lcm_dcs_write_seq(ctx, seq...)                                         \
	({                                                                     \
		const u8 d[] = { seq };                                        \
		BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64,                           \
				 "DCS sequence too big for stack");            \
		lcm_dcs_write(ctx, d, ARRAY_SIZE(d));                          \
	})

#define lcm_dcs_write_seq_static(ctx, seq...)  \
({\
	static const u8 d[] = { seq };\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

#ifdef PANEL_SUPPORT_READBACK
static int lcm_dcs_read(struct lcm *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_info(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret,
			 cmd);
		ctx->error = ret;
	}

	return ret;
}

static void lcm_panel_get_data(struct lcm *ctx)
{
	u8 buffer[3] = { 0 };
	static int ret;

	pr_info("%s+\n", __func__);

	if (ret == 0) {
		ret = lcm_dcs_read(ctx, 0x0A, buffer, 1);
		pr_info("%s  0x%08x\n", __func__, buffer[0] | (buffer[1] << 8));
		dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif

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
		dev_info(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

static void lcm_panel_init(struct lcm *ctx)
{
    pr_info("%s+\n", __func__);
    lcm_dcs_write_seq_static(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
    lcm_dcs_write_seq_static(ctx, 0xB5, 0x94, 0x42);
    lcm_dcs_write_seq_static(ctx, 0x6F, 0x05);
    lcm_dcs_write_seq_static(ctx, 0xB5, 0x7F, 0x00, 0x29, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x6F, 0x0B);
    lcm_dcs_write_seq_static(ctx, 0xB5, 0x00, 0x2c, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x6F, 0x10);
    lcm_dcs_write_seq_static(ctx, 0xB5, 0x29, 0x29, 0x29, 0x29, 0x29);
    lcm_dcs_write_seq_static(ctx, 0x6F, 0x16);
    lcm_dcs_write_seq_static(ctx, 0xB5, 0x0b, 0x19, 0x00, 0x00, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x6F, 0x1b);
    lcm_dcs_write_seq_static(ctx, 0xB5, 0x0b, 0x1a, 0x00, 0x00, 0x00);

    lcm_dcs_write_seq_static(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
#ifndef LCM_VDO_MODE
    lcm_dcs_write_seq_static(ctx, 0xC0, 0x6E, 0xB3, 0x20);
#endif
    lcm_dcs_write_seq_static(ctx, 0xB7, 0x33, 0x33, 0x33, 0x33, 0x33, 0x21, 0x0f, 0xed, 0xca, 0x86, 0x42, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x6F, 0x0C);
    lcm_dcs_write_seq_static(ctx, 0xB7, 0x05, 0x55, 0x55, 0x15, 0x00, 0x00, 0x00);
    lcm_dcs_write_seq_static(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x08);
    lcm_dcs_write_seq_static(ctx, 0xE1, 0x00);
    lcm_dcs_write_seq_static(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x80);
    lcm_dcs_write_seq_static(ctx, 0x6F, 0x1D);
    lcm_dcs_write_seq_static(ctx, 0xF2, 0x05);
    lcm_dcs_write_seq_static(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x81);

    lcm_dcs_write_seq_static(ctx, 0x35);
    lcm_dcs_write_seq_static(ctx, 0x53, 0x20);
    lcm_dcs_write_seq_static(ctx, 0x51, 0x00, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x2A, 0x00, 0x00, 0x04, 0x37);
    lcm_dcs_write_seq_static(ctx, 0x2B, 0x00, 0x00, 0x09, 0x5F);

    lcm_dcs_write_seq_static(ctx, 0x82, 0xB2);
    lcm_dcs_write_seq_static(ctx, 0x88, 0x01, 0x02, 0x1C, 0x08, 0x78);
#ifdef DSC_DISABLE
	lcm_dcs_write_seq_static(ctx, 0x03, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x90, 0x02);
#else
    lcm_dcs_write_seq_static(ctx, 0x03, 0x01);
    lcm_dcs_write_seq_static(ctx, 0x90, 0x01);
#endif

#ifdef DSC_10BIT
    lcm_dcs_write_seq_static(ctx, 0x91, 0xAB, 0x28, 0x00, 0x0C, 0xC2, 0x00, 0x03, 0x1C, 0x01, 0x7E, 0x00, 0x0F, 0x08, 0xBB, 0x04, 0x3D, 0x10, 0xF0);
#else
    lcm_dcs_write_seq_static(ctx, 0x91, 0x89, 0x28, 0x00, 0x0C, 0xC2, 0x00, 0x03, 0x1C, 0x01, 0x7E, 0x00, 0x0F, 0x08, 0xBB, 0x04, 0x3D, 0x10, 0xF0);
#endif
    lcm_dcs_write_seq_static(ctx, 0x2C);
    lcm_dcs_write_seq_static(ctx, 0x8B, 0x80);



#ifndef LCM_VDO_MODE
    //FrameRate 60Hz:0x01  120HZ:0x02
    lcm_dcs_write_seq_static(ctx, 0x2F, 0x01);
#else
    //FrameRate 60Hz:0x01  120HZ:0x02
    lcm_dcs_write_seq_static(ctx, 0x2F, 0x01);
    lcm_dcs_write_seq_static(ctx, 0x3B, 0x00, 0x07, 0x0d, 0x79);
#endif

    /* Sleep Out */
    lcm_dcs_write_seq_static(ctx, 0x11);
    msleep(120);
    /* Display On */
    lcm_dcs_write_seq_static(ctx, 0x29);

    lcm_dcs_write_seq_static(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x08);
    lcm_dcs_write_seq_static(ctx, 0xE1, 0x21);

	pr_info("%s-, boe\n", __func__);
}

static int lcm_disable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int gate_ic_Power_on(struct drm_panel *panel, int enabled)
{
	struct lcm *ctx = panel_to_lcm(panel);
	bool gpio_status;
	struct gpio_desc *pm_en_pin;
	int i;

	gpio_status = enabled ? 1:0;
	if (gpio_status) {
		for (i=0; i < 3; i++) {
			pm_en_pin = NULL;
			pm_en_pin = devm_gpiod_get_index(ctx->dev, "pm-enable", i, GPIOD_OUT_HIGH);
			if (IS_ERR(pm_en_pin)) {
				pr_err("cannot get bias-gpios %d %ld\n", i, PTR_ERR(pm_en_pin));
				return PTR_ERR(pm_en_pin);
			}
			gpiod_set_value(pm_en_pin, gpio_status);
			devm_gpiod_put(ctx->dev, pm_en_pin);
			usleep_range(1000, 1001);
		}
	} else {
		for (i=2; i >= 0; i--) {
			pm_en_pin = NULL;
			pm_en_pin = devm_gpiod_get_index(ctx->dev, "pm-enable", i, GPIOD_OUT_LOW);
			if (IS_ERR(pm_en_pin)) {
				pr_err("cannot get bias-gpios %d %ld\n", i, PTR_ERR(pm_en_pin));
				return PTR_ERR(pm_en_pin);
			}
			gpiod_set_value(pm_en_pin, gpio_status);
			devm_gpiod_put(ctx->dev, pm_en_pin);
			usleep_range(1000, 1001);
		}
	}
	return 0;
}


static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);


	pr_info("%s+\n", __func__);
	if (!ctx->prepared)
		return 0;

	lcm_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_OFF);
	lcm_dcs_write_seq_static(ctx, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(120);

	ctx->error = 0;
	ctx->prepared = false;

	return 0;
}

static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	pr_info("%s+\n", __func__);
	if (ctx->prepared)
		return 0;

	ctx->hbm_mode = 0;
	ctx->dc_mode = 0;
	ctx->current_fps = 120;

	// lcd reset L->H -> L -> L
	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_LOW);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 10001);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(1000, 1001);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(1000, 1001);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 10001);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
	// end

	lcm_panel_init(ctx);

	ret = ctx->error;
	if (ret < 0) goto error;

	ctx->prepared = true;
#ifdef PANEL_SUPPORT_READBACK
	lcm_panel_get_data(ctx);
#endif

	pr_info("%s-\n", __func__);
	return ret;
error:
	lcm_unprepare(panel);
	return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}


#ifdef LCM_VDO_MODE
#define HFP (32)
#define HSA (32)
#define HBP (32)
#define VFP (3449)
#define VSA (2)
#define VBP (5)
#define HACT (1080)
#define VACT (2400)
#define PLL_CLOCK (413199)	//1176*2440*144
#define DISP_PLL_CLK (574)

static const struct drm_display_mode switch_mode_144hz = {
	.clock		= PLL_CLOCK,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + 33,
	.vsync_end	= VACT + 33 + VSA,
	.vtotal		= VACT + 33 + VSA + VBP,
};

static const struct drm_display_mode switch_mode_120hz = {
	.clock		= PLL_CLOCK,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + 521,
	.vsync_end	= VACT + 521 + VSA,
	.vtotal		= VACT + 521 + VSA + VBP,
};

static const struct drm_display_mode switch_mode_90hz = {
	.clock		= PLL_CLOCK,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + 1497,
	.vsync_end	= VACT + 1497 + VSA,
	.vtotal		= VACT + 1497 + VSA + VBP,
};

static const struct drm_display_mode switch_mode_60hz = {
	.clock = PLL_CLOCK,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + 3449,
	.vsync_end	= VACT + 3449 + VSA,
	.vtotal		= VACT + 3449 + VSA + VBP,
};

#if defined(CONFIG_MTK_PANEL_EXT)
static struct mtk_panel_params ext_params_60hz = {
	.pll_clk = DISP_PLL_CLK,
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},

	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.switch_en = 1,
		.dfps_cmd_grp_table[0] = {2, {0x2F, 0x01} },
		.dfps_cmd_grp_table[1] = {5, {0x3B, 0x00, 0x07, 0x0d, 0x79} },
		.dfps_cmd_grp_size = 2,
	},
	.data_rate = DISP_PLL_CLK * 2,
	.change_fps_by_vfp_send_cmd = 1,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-vid-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};

static struct mtk_panel_params ext_params_90hz = {
	.pll_clk = DISP_PLL_CLK,
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},
	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.switch_en = 1,
		.dfps_cmd_grp_table[0] = {2, {0x2F, 0x03} },
		.dfps_cmd_grp_table[1] = {5, {0x3B, 0x00, 0x07, 0x05, 0xD9} },
		.dfps_cmd_grp_size = 2,
	},
	.data_rate = DISP_PLL_CLK * 2,
	.change_fps_by_vfp_send_cmd = 1,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-vid-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};
static struct mtk_panel_params ext_params_120hz = {
	.pll_clk = DISP_PLL_CLK,
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},
	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.switch_en = 1,
		.dfps_cmd_grp_table[0] = {2, {0x2F, 0x02} },
		.dfps_cmd_grp_table[1] = {5, {0x3B, 0x00, 0x07, 0x02, 0x09} },
		.dfps_cmd_grp_size = 2,
	},
	.data_rate = DISP_PLL_CLK * 2,
	.change_fps_by_vfp_send_cmd = 1,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-vid-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};

static struct mtk_panel_params ext_params_144hz = {
	.pll_clk = DISP_PLL_CLK,
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},
	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.switch_en = 1,
		.dfps_cmd_grp_table[0] = {2, {0x2F, 0x04} },
		.dfps_cmd_grp_table[1] = {5, {0x3B, 0x00, 0x07, 0x00, 0x21} },
		.dfps_cmd_grp_size = 2,
	},
	.data_rate = DISP_PLL_CLK * 2,
	.change_fps_by_vfp_send_cmd = 1,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-vid-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};
#endif
#else
#define HFP (4)
#define HSA (4)
#define HBP (4)
#define HACT (1080)
#define VFP (20)
#define VSA (2)
#define VBP (8)
#define VACT (2400)
#define PLL_CLOCK (330)

static const struct drm_display_mode switch_mode_144hz = {
	.clock		= 382112,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + VFP,
	.vsync_end	= VACT + VFP + VSA,
	.vtotal		= VACT + VFP + VSA + VBP,
};

static const struct drm_display_mode switch_mode_120hz = {
	.clock		= 318428,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + VFP,
	.vsync_end	= VACT + VFP + VSA,
	.vtotal		= VACT + VFP + VSA + VBP,
};

static const struct drm_display_mode switch_mode_90hz = {
	.clock		= 238821,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + VFP,
	.vsync_end	= VACT + VFP + VSA,
	.vtotal		= VACT + VFP + VSA + VBP,
};

static const struct drm_display_mode switch_mode_60hz = {
	.clock = 159214,
	.hdisplay	= HACT,
	.hsync_start	= HACT + HFP,
	.hsync_end	= HACT + HFP + HSA,
	.htotal		= HACT + HFP + HSA + HBP,
	.vdisplay	= VACT,
	.vsync_start	= VACT + VFP,
	.vsync_end	= VACT + VFP + VSA,
	.vtotal		= VACT + VFP + VSA + VBP,
};

#if defined(CONFIG_MTK_PANEL_EXT)
static struct mtk_panel_params ext_params_60hz = {
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},

	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.data_rate = 440,
	},
	.data_rate = 440,
	.lp_perline_en = 0,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-cmd-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};

static struct mtk_panel_params ext_params_90hz = {

	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},
	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.data_rate = 630,
	},
	.data_rate = 630,
	.lp_perline_en = 0,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-cmd-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};
static struct mtk_panel_params ext_params_120hz = {

	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},
	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.data_rate = 820,
	},
	.data_rate = 820,
	.lp_perline_en = 0,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-cmd-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};

static struct mtk_panel_params ext_params_144hz = {

	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.physical_width_um = 68256,
	.physical_height_um = 151680,
	.lcm_index = 3,

	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 0,
		.rgb_swap = 0,
		.dsc_cfg = DSC_CFG,
		.rct_on = 1,
		.bit_per_channel = DSC_BITS,
		.dsc_line_buf_depth = 11,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 12,
		.slice_width = 1080,
		.chunk_size = 1080,
		.xmit_delay = 512,
		.dec_delay = 796,
		.scale_value = 32,
		.increment_interval = 382,
		.decrement_interval = 15,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 2235,
		.slice_bpg_offset = 1085,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 7,
		.flatness_maxqp = 16,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 15,
		.rc_quant_incr_limit1 = 15,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,

		.pps_list = {
			.count = 3,
			.dsc_pps_params[0] = {
				.dsc_pps_idx = 17,
				.dsc_pps_para = 0xD209D9E9,
			},
			.dsc_pps_params[1] = {
				.dsc_pps_idx = 18,
				.dsc_pps_para = 0xD22BD229,
			},
			.dsc_pps_params[2] = {
				.dsc_pps_idx = 19,
				.dsc_pps_para = 0x0000D271,
			},
		},
	},
	.max_bl_level = 3514,
	.hbm_type = HBM_MODE_DCS_ONLY,

	.dyn_fps = {
		.data_rate = 1000,
	},
	.data_rate = 1000,
	.lp_perline_en = 0,

	.panel_cellid_reg = 0xac,
	.panel_cellid_offset_reg = 0x6f,
	.panel_cellid_len = 23,

	.panel_ver = 1,
	.panel_id = 0x010b1591,
	.panel_name = "panel-boe-nt37701a-cmd-144hz",
	.panel_supplier = "boe-nt37701a",

	.check_panel_feature = 1,
};
#endif
#endif

static int panel_ata_check(struct drm_panel *panel)
{
	/* Customer test by own ATA tool */
	return 1;
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb, void *handle,
				 unsigned int level)
{
	char bl_tb0[] = { 0x51, 0x0f, 0x59};
	struct lcm *ctx = g_ctx;
	unsigned int bl_level = level;
	unsigned int hbm_bl_index = 0;

	if (bl_level >= 3515) {
		hbm_bl_index= bl_level-3515;
		if (hbm_bl_index >= ARRAY_SIZE(hbm_bl_mapping))
			hbm_bl_index = ARRAY_SIZE(hbm_bl_mapping) -1;
		bl_level = hbm_bl_mapping[hbm_bl_index];
	}

	if ((ctx->hbm_mode) && bl_level) {
		pr_info("hbm_mode = %d, skip backlight(%d)\n", ctx->hbm_mode, bl_level);
		return 0;
	}

	if (!(ctx->current_bl && bl_level)) pr_info("backlight changed from %u to %u\n", ctx->current_bl, bl_level);
	else pr_debug("backlight changed from %u to %u\n", ctx->current_bl, bl_level);

	bl_tb0[1] = (u8)((bl_level>>8)&0xF);
	bl_tb0[2] = (u8)(bl_level&0xFF);

	if (!cb)
		return -1;

	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
	ctx->current_bl = bl_level;

	return 0;
}

static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);

	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

struct drm_display_mode *get_mode_by_id(struct drm_connector *connector,
	unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	list_for_each_entry(m, &connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}
	return NULL;
}

static int mtk_panel_ext_param_set(struct drm_panel *panel,
			struct drm_connector *connector, unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id(connector, mode);
	struct lcm *ctx = panel_to_lcm(panel);

	if (drm_mode_vrefresh(m) == 144) {
		ext->params = &ext_params_144hz;
		ctx->current_fps = 144;
	} else if (drm_mode_vrefresh(m) == 120) {
		ctx->current_fps = 120;
		ext->params = &ext_params_120hz;
	} else if (drm_mode_vrefresh(m) == 90) {
		ext->params = &ext_params_90hz;
		ctx->current_fps = 90;
	} else if (drm_mode_vrefresh(m) == 60) {
		ext->params = &ext_params_60hz;
		ctx->current_fps = 60;
	} else {
		ret = 1;
	}

	return ret;
}

#ifndef LCM_VDO_MODE
static void mode_switch_to_144(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	if (stage == BEFORE_DSI_POWERDOWN) {
		struct lcm *ctx = panel_to_lcm(panel);

		lcm_dcs_write_seq_static(ctx, 0x2F, 0x04);
	}
}

static void mode_switch_to_120(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	if (stage == BEFORE_DSI_POWERDOWN) {
		struct lcm *ctx = panel_to_lcm(panel);

		lcm_dcs_write_seq_static(ctx, 0x2F, 0x02);
		mdelay(6);
	}
}

static void mode_switch_to_90(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	if (stage == BEFORE_DSI_POWERDOWN) {
		struct lcm *ctx = panel_to_lcm(panel);

		lcm_dcs_write_seq_static(ctx, 0x2F, 0x03);
		mdelay(8);
	}
}
static void mode_switch_to_60(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	if (stage == BEFORE_DSI_POWERDOWN) {
		struct lcm *ctx = panel_to_lcm(panel);

		lcm_dcs_write_seq_static(ctx, 0x2F, 0x01);
		mdelay(11);
	}
}

static int mode_switch(struct drm_panel *panel,
		struct drm_connector *connector, unsigned int cur_mode,
		unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id(connector, dst_mode);

	if (cur_mode == dst_mode)
		return ret;

	pr_info("%s cur_mode = %d dst_mode %d\n", __func__, cur_mode, dst_mode);

	if (drm_mode_vrefresh(m) == 60) { /*switch to 60 */
		mode_switch_to_60(panel, stage);
	} else if (drm_mode_vrefresh(m) == 90) { /*switch to 90 */
		mode_switch_to_90(panel, stage);
	} else if (drm_mode_vrefresh(m) == 120) { /*switch to 120 */
		mode_switch_to_120(panel, stage);
	} else if (drm_mode_vrefresh(m) == 144) { /*switch to 144 */
		mode_switch_to_144(panel, stage);
	} else
		ret = 1;

	return ret;
}
#endif

static struct mtk_panel_para_table panel_lhbm_on[] = {
	{3, {0x51, 0x0F, 0xFF}},
	{4, {0x87, 0x1F, 0xFF, 0x05}},
};

static struct mtk_panel_para_table panel_lhbm_off[] = {
	{4, {0x87, 0x0F, 0xFF, 0x00}},
	{3, {0x51, 0x0D, 0xBA}},
};

static void set_lhbm_alpha(unsigned int bl_level, uint32_t on)
{
	struct mtk_panel_para_table *pAlphaTable;
	struct mtk_panel_para_table *pDbvTable;
	unsigned int alpha = 0;
	//unsigned int dbv = 0;

	if(on) {
		pAlphaTable = &panel_lhbm_on[1];

		if (bl_level >= ARRAY_SIZE(lhbm_alpha))
			bl_level = ARRAY_SIZE(lhbm_alpha) -1;

		alpha = lhbm_alpha[bl_level];

		pAlphaTable->para_list[1] = (alpha >> 8) & 0xFF;
		pAlphaTable->para_list[2] = alpha & 0xFF;
		pr_info("%s: backlight %d alpha %x(0x%x, 0x%x)\n", __func__, bl_level, alpha, pAlphaTable->para_list[1], pAlphaTable->para_list[2]);
	} else {
		pDbvTable = &panel_lhbm_off[1];
		pDbvTable->para_list[1] = (bl_level >> 8) & 0xFF;
		pDbvTable->para_list[2] = bl_level & 0xFF;
		pr_info("%s: backlight restore %d dbv(0x%x, 0x%x)\n", __func__, bl_level,
			pDbvTable->para_list[1], pDbvTable->para_list[2]);
	}
}

static int panel_lhbm_set_cmdq(void *dsi, dcs_grp_write_gce cb, void *handle, uint32_t on, uint32_t bl_level)
{
	unsigned int para_count = 0;
	struct mtk_panel_para_table *pTable;

	set_lhbm_alpha(bl_level, on);
	if (on) {
		para_count = sizeof(panel_lhbm_on) / sizeof(struct mtk_panel_para_table);
		pTable = panel_lhbm_on;
		cb(dsi, handle, pTable, para_count);
	} else {
		para_count = sizeof(panel_lhbm_off) / sizeof(struct mtk_panel_para_table);
		pTable = panel_lhbm_off;
		cb(dsi, handle, pTable, para_count);
		usleep_range(5000, 5010);
		cb(dsi, handle, pTable, para_count);
	}
	pr_info("%s: para_count %d\n", __func__, para_count);
	return 0;

}

static int pane_hbm_set_cmdq(struct lcm *ctx, void *dsi, dcs_grp_write_gce cb, void *handle, uint32_t hbm_state)
{
	struct mtk_panel_para_table hbm_on_table = {3, {0x51, 0x0F, 0xFF}};

	if (hbm_state > 2) return -1;
	switch (hbm_state)
	{
		case 0:
			if (ctx->lhbm_en)
				panel_lhbm_set_cmdq(dsi, cb, handle, 0, ctx->current_bl);
			break;
		case 1:
			cb(dsi, handle, &hbm_on_table, 1);
			break;
		case 2:
			if (ctx->lhbm_en)
				panel_lhbm_set_cmdq(dsi, cb, handle, 1, ctx->current_bl);
			else
				cb(dsi, handle, &hbm_on_table, 1);
			break;
		default:
			break;
	}

	return 0;
}

static struct mtk_panel_para_table panel_dc_off[] = {
   {6, {0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00}},
   {2, {0xB2, 0x11}},
   {2, {0x6F, 0x0F}},
   {9, {0xB2,0x60,0x50,0x36,0x8D,0x56,0x8D,0x2F,0xFF}},
   {37, {0xB3,0x00,0x01,0x01,0x58,0x01,0x58,0x02,0x19,0x02,0x19,0x03,0x09,0x03,0x09,0x04,0x43,0x04,0x43,0x06,0x8D,0x06,0x8D,0x06,0x8E,0x06,0x8E,0x0B,0x31,0x0B,0x31,0x0D,0xBA,0x0D,0xBA,0x0F,0xFF}},
};

static struct mtk_panel_para_table panel_dc_on[] = {
   {6, {0xF0,0x55,0xAA,0x52,0x08,0x00}},
   {2, {0xB2, 0x91}},
   {2, {0x6F, 0x0F}},
   {9, {0xB2,0x60,0x50,0x30,0x00,0x50,0x00,0x5F,0xFF}},
   {37, {0xB3,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x8E,0x0B,0x31,0x0B,0x31,0x0D,0xBA,0x0D,0xBA,0x0F,0xFF}},
};

static int pane_dc_set_cmdq(void *dsi, dcs_grp_write_gce cb, void *handle, uint32_t dc_state)
{
	unsigned int para_count = 0;
	struct mtk_panel_para_table *pTable;

	if (dc_state) {
		para_count = sizeof(panel_dc_on) / sizeof(struct mtk_panel_para_table);
		pTable = panel_dc_on;
	} else {
		para_count = sizeof(panel_dc_off) / sizeof(struct mtk_panel_para_table);
		pTable = panel_dc_off;
	}
	cb(dsi, handle, pTable, para_count);
	return 0;
}

static int panel_feature_set(struct drm_panel *panel, void *dsi,
			      dcs_grp_write_gce cb, void *handle, struct panel_param_info param_info)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret = 0;

	if ((!cb) || (!ctx->enabled)) {
		ret = -1;
	} else {
		pr_info("%s: set feature %d to %d\n", __func__, param_info.param_idx, param_info.value);

		switch (param_info.param_idx) {
			case PARAM_CABC:
			case PARAM_ACL:
				ret = -1;
				break;
			case PARAM_HBM:
				ctx->hbm_mode = param_info.value;
				pane_hbm_set_cmdq(ctx, dsi, cb, handle, param_info.value);
				break;
			case PARAM_DC:
				pane_dc_set_cmdq(dsi, cb, handle, param_info.value);
				ctx->dc_mode = param_info.value;
				break;
			default:
				ret = -1;
				break;
		}

		pr_info("%s: set feature %d to %d success\n", __func__, param_info.param_idx, param_info.value);
	}
	return ret;
}

static int panel_feature_get(struct drm_panel *panel, struct panel_param_info *param_info)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret = 0;

	switch (param_info->param_idx) {
		case PARAM_CABC:
		case PARAM_ACL:
			ret = -1;
			break;
		case PARAM_HBM:
			param_info->value = ctx->hbm_mode;
			break;
		case PARAM_DC:
			param_info->value = ctx->dc_mode;
			break;
		default:
			ret = -1;
			break;
	}
	return ret;
}

static int panel_hbm_waitfor_fps_valid(struct drm_panel *panel, unsigned int timeout_ms)
{
	struct lcm *ctx = panel_to_lcm(panel);
	unsigned int count = timeout_ms;
	unsigned int poll_interval = 1;

	if (count == 0) return 0;
	pr_info("%s+, fps = %d \n", __func__, ctx->current_fps);
	while((ctx->current_fps != 120)) {
		if (!count) {
			pr_warn("%s: it is timeout, and current_fps = %d\n", __func__, ctx->current_fps);
			break;
		} else if (count > poll_interval) {
			usleep_range(poll_interval * 1000, poll_interval *1000);
			count -= poll_interval;
		} else {
			usleep_range(count * 1000, count *1000);
			count = 0;
		}
	}
	pr_info("%s-, fps = %d \n", __func__, ctx->current_fps);
	return 0;
}

static int panel_ext_init_power(struct drm_panel *panel)
{
	int ret;
	ret = gate_ic_Power_on(panel, 1);
	return ret;
}

static int panel_ext_powerdown(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	pr_info("%s+\n", __func__);
	if (ctx->prepared)
	    return 0;

	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
	usleep_range(1000, 1001);

	gate_ic_Power_on(panel, 0);

	return 0;
}

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.init_power = panel_ext_init_power,
	.power_down = panel_ext_powerdown,
	.ata_check = panel_ata_check,
	.ext_param_set = mtk_panel_ext_param_set,
#ifndef LCM_VDO_MODE
	.mode_switch = mode_switch,
#endif
	.panel_feature_set = panel_feature_set,
	.panel_feature_get = panel_feature_get,
	.panel_hbm_waitfor_fps_valid = panel_hbm_waitfor_fps_valid,
};

static int lcm_get_modes(struct drm_panel *panel,
					struct drm_connector *connector)
{
	struct drm_display_mode *mode_1;
	struct drm_display_mode *mode_2;
	struct drm_display_mode *mode_3;
	struct drm_display_mode *mode_4;

	mode_1 = drm_mode_duplicate(connector->dev, &switch_mode_60hz);
	if (!mode_1) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			 switch_mode_60hz.hdisplay, switch_mode_60hz.vdisplay,
			 drm_mode_vrefresh(&switch_mode_60hz));
		return -ENOMEM;
	}
	drm_mode_set_name(mode_1);
	mode_1->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode_1);

	mode_2 = drm_mode_duplicate(connector->dev, &switch_mode_90hz);
	if (!mode_2) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			switch_mode_90hz.hdisplay, switch_mode_90hz.vdisplay,
			drm_mode_vrefresh(&switch_mode_90hz));
		return -ENOMEM;
	}
	drm_mode_set_name(mode_2);
	mode_2->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode_2);

	mode_3 = drm_mode_duplicate(connector->dev, &switch_mode_120hz);
	if (!mode_3) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			switch_mode_120hz.hdisplay, switch_mode_120hz.vdisplay,
			drm_mode_vrefresh(&switch_mode_120hz));
		return -ENOMEM;
	}
	drm_mode_set_name(mode_3);
	mode_3->type = DRM_MODE_TYPE_DRIVER| DRM_MODE_TYPE_PREFERRED;;
	drm_mode_probed_add(connector, mode_3);

	mode_4 = drm_mode_duplicate(connector->dev, &switch_mode_144hz);
	if (!mode_4) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			switch_mode_144hz.hdisplay, switch_mode_144hz.vdisplay,
			drm_mode_vrefresh(&switch_mode_144hz));
		return -ENOMEM;
	}
	drm_mode_set_name(mode_4);
	mode_4->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode_4);

	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 152;
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
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;
	const u32 *val;

	pr_info("%s+\n", __func__);

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
		pr_info("%s+ skip probe due to not current lcm(node: %s)\n", __func__, dev->of_node->name);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);
	g_ctx = ctx;
	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
#ifndef LCM_VDO_MODE
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;
#else
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE
			 | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;
#endif

	mipi_drive_volt = 0x6;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_info(dev, "cannot get reset-gpios %ld\n",
			 PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);
	ctx->prepared = true;
	ctx->enabled = true;
	drm_panel_init(&ctx->panel, dev, &lcm_drm_funcs, DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	val = of_get_property(dev->of_node, "reg", NULL);
	ctx->version = val ? be32_to_cpup(val) : 1;

	pr_info("%s: panel version 0x%x\n", __func__, ctx->version);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params_60hz, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;

#endif
	ctx->hbm_mode = 0;
	ctx->dc_mode = 0;
	ctx->current_fps = 60;

	ctx->lhbm_en = of_property_read_bool(dev->of_node, "lhbm-enable");

	pr_info("%s- lcm,tm, vtdr6115,cmd,120hz, lhbm_en = %d\n", __func__, ctx->lhbm_en);

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
	{
		.compatible = "boe,nt37701a,cmd,144hz",
	},
	{}
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "panel-boe-nt37701a-cmd-144hz",
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("MEDIATEK");
MODULE_DESCRIPTION("tianma vtdr6115 AMOLED VDO SPR Panel Driver");
MODULE_LICENSE("GPL v2");
