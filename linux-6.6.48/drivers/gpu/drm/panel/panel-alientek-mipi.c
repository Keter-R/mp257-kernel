// SPDX-License-Identifier: GPL-2.0
/*
 *	Copyright (C) 2024 ALIENTEK - All Rights Reserved
 *  ALIENTEK MIPI LCD : 5.5 inch 720x1280, 1080x1920 and 10.1 inch 800x1280
 *  Alientek MIPI LCD : 5.5 inch use IC HX839X and 10.1 inch use IC ili9881
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <video/mipi_display.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

/* User Define command set */
#define UD_SETADDRESSMODE	0x36 /* Set address mode */
#define UD_SETSEQUENCE		0xB0 /* Set sequence */
#define UD_SETPOWER			0xB1 /* Set power */
#define UD_SETDISP			0xB2 /* Set display related register */
#define UD_SETCYC			0xB4 /* Set display waveform cycles */
#define UD_SETVCOM			0xB6 /* Set VCOM voltage */
#define UD_SETTE			0xB7 /* Set internal TE function */
#define UD_SETSENSOR		0xB8 /* Set temperature sensor */
#define UD_SETEXTC			0xB9 /* Set extension command */
#define UD_SETMIPI			0xBA /* Set MIPI control */
#define UD_SETOTP			0xBB /* Set OTP */
#define UD_SETREGBANK		0xBD /* Set register bank */
#define UD_SETDGCLUT		0xC1 /* Set DGC LUT */
#define UD_SETID			0xC3 /* Set ID */
#define UD_SETDDB			0xC4 /* Set DDB */
#define UD_SETCABC			0xC9 /* Set CABC control */
#define UD_SETCABCGAIN		0xCA
#define UD_SETPANEL			0xCC
#define UD_SETOFFSET		0xD2
#define UD_SETGIP0			0xD3 /* Set GIP Option0 */
#define UD_SETGIP1			0xD5 /* Set GIP Option1 */
#define UD_SETGIP2			0xD6 /* Set GIP Option2 */
#define UD_SETGIP3			0xD8 /* Set GIP Option2 */
#define UD_SETGPO			0xD9
#define UD_SETSCALING		0xDD
#define UD_SETIDLE			0xDF
#define UD_SETGAMMA			0xE0 /* Set gamma curve related setting */
#define UD_SETCHEMODE_DYN	0xE4
#define UD_SETCHE			0xE5
#define UD_SETCESEL			0xE6 /* Enable color enhance */
#define UD_SET_SP_CMD		0xE9
#define UD_SETREADINDEX		0xFE /* Set SPI Read Index */
#define UD_GETSPIREAD		0xFF /* SPI Read Command Data */

#define ILI9881C_SWITCH_PAGE_INSTR(_page)	\
	{					\
		.op = ILI9881C_SWITCH_PAGE,	\
		.arg = {			\
			.page = (_page),	\
		},				\
	}

#define ILI9881C_COMMAND_INSTR(_cmd, _data)		\
	{						\
		.op = ILI9881C_COMMAND,		\
		.arg = {				\
			.cmd = {			\
				.cmd = (_cmd),		\
				.data = (_data),	\
			},				\
		},					\
	}

enum alientek_lcd_select {
    atk_no_mipi = 1,
    atk_mipi_dsi_5x5_720x1280 = 2,
	atk_mipi_dsi_5x5_1080x1920 = 3,
	atk_mipi_dsi_10x1_800x1280 = 4,	
};

enum ili9881c_op {
	ILI9881C_SWITCH_PAGE,
	ILI9881C_COMMAND,
};

struct ili9881c_instr {
	enum ili9881c_op	op;

	union arg {
		struct cmd {
			u8	cmd;
			u8	data;
		} cmd;
		u8	page;
	} arg;
};

struct ili9881c_desc {
	const struct ili9881c_instr *init;
	const size_t init_length;
	const struct drm_display_mode *mode;
	const unsigned long mode_flags;
};

struct alientek_mipi {
	struct device *dev;	
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;
	const struct ili9881c_desc	*desc;
	struct regulator	*power;
	struct gpio_desc	*reset_gpio;
	bool prepared;
	bool enabled;
};

static const struct drm_display_mode atk_mipi_dsi_5x5_720x1280_mode = {
	//ALIENTEK MIPI LCD 720x1280	
	.clock 			= 61000,				/* LCD 像素时钟 */
	.hdisplay 		= 720,					/* LCD X轴像素个数 */
	.hsync_start 	= 720 + 50,				/* LCD X轴+hfp像素个数 */
	.hsync_end 		= 720 + 50 + 8,			/* LCD X轴+hfp+hspw像素个数 */
	.htotal 		= 720 + 50 + 8 + 52,	/* LCD X轴+hfp+hspw+hbp像素个数 */
	.vdisplay 		= 1280,					/* LCD Y轴像素个数 */
	.vsync_start 	= 1280 + 16,			/* LCD Y轴+vfp像素个数 */
	.vsync_end 		= 1280 + 16 + 6,		/* LCD Y轴+vfp+vspw像素个数 */
	.vtotal 		= 1280 + 16 + 6 + 15,	/* LCD Y轴+vfp+vspw+vbp像素个数 */
	.flags 			= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct drm_display_mode atk_mipi_dsi_5x5_1080x1920_mode = {
	//ALIENTEK MIPI LCD 1080x1920	
	.clock 			= 108000,				/* LCD 像素时钟 */
	.hdisplay 		= 1080,					/* LCD X轴像素个数 */
	.hsync_start 	= 1080 + 22,			/* LCD X轴+hfp像素个数 */
	.hsync_end 		= 1080 + 22 + 22,		/* LCD X轴+hfp+hspw像素个数 */
	.htotal 		= 1080 + 22 + 22 + 20,	/* LCD X轴+hfp+hspw+hbp像素个数 */
	.vdisplay 		= 1920,					/* LCD Y轴像素个数 */
	.vsync_start 	= 1920 + 9,			    /* LCD Y轴+vfp像素个数 */
	.vsync_end 		= 1920 + 9 + 7,		    /* LCD Y轴+vfp+vspw像素个数 */
	.vtotal 		= 1920 + 9 + 7 + 7,	    /* LCD Y轴+vfp+vspw+vbp像素个数 */
	.flags 			= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct drm_display_mode atk_mipi_dsi_10x1_800x1280_mode = {
	//ALIENTEK MIPI LCD 800x1280
	.clock			= 67000,				/* LCD 像素时钟 */
	.hdisplay		= 800,					/* LCD X轴像素个数 */
	.hsync_start	= 800 + 12,				/* LCD X轴+hfp像素个数 */
	.hsync_end		= 800 + 12 + 24,		/* LCD X轴+hfp+hspw像素个数 */
	.htotal			= 800 + 12 + 24 + 24,	/* LCD X轴+hfp+hspw+hbp像素个数 */
	.vdisplay		= 1280,					/* LCD Y轴像素个数 */
	.vsync_start	= 1280 + 7,				/* LCD Y轴+vfp像素个数 */
	.vsync_end		= 1280 + 7 + 2,			/* LCD Y轴+vfp+vspw像素个数 */
	.vtotal			= 1280 + 7 + 2 + 9,		/* LCD Y轴+vfp+vspw+vbp像素个数 */
	.flags 			= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,	
};

/*
 * The panel seems to accept some private DCS commands that map
 * directly to registers.
 *
 * It is organised by page, with each page having its own set of
 * registers, and the first page looks like it's holding the standard
 * DCS commands.
 *
 * So before any attempt at sending a command or data, we have to be
 * sure if we're in the right page or not.
 */
static int ili9881c_switch_page(struct alientek_mipi *ctx, u8 page)
{
	u8 buf[4] = { 0xff, 0x98, 0x81, page };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int ili9881c_send_cmd_data(struct alientek_mipi *ctx, u8 cmd, u8 data)
{
	u8 buf[2] = { cmd, data };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static const struct ili9881c_instr alientek_init_ili9881c[] = {
	ILI9881C_SWITCH_PAGE_INSTR(3),
	ILI9881C_COMMAND_INSTR(0x01, 0x00),
	ILI9881C_COMMAND_INSTR(0x02, 0x00),
	ILI9881C_COMMAND_INSTR(0x03, 0x53),
	ILI9881C_COMMAND_INSTR(0x04, 0xD3),
	ILI9881C_COMMAND_INSTR(0x05, 0x00),
	ILI9881C_COMMAND_INSTR(0x06, 0x0D),
	ILI9881C_COMMAND_INSTR(0x07, 0x08),
	ILI9881C_COMMAND_INSTR(0x08, 0x00),
	ILI9881C_COMMAND_INSTR(0x09, 0x00),
	ILI9881C_COMMAND_INSTR(0x0A, 0x00),
	ILI9881C_COMMAND_INSTR(0x0B, 0x00),
	ILI9881C_COMMAND_INSTR(0x0C, 0x00),
	ILI9881C_COMMAND_INSTR(0x0D, 0x00),
	ILI9881C_COMMAND_INSTR(0x0E, 0x00),
	ILI9881C_COMMAND_INSTR(0x0F, 0x28),

	ILI9881C_COMMAND_INSTR(0x10, 0x28),
	ILI9881C_COMMAND_INSTR(0x11, 0x00),
	ILI9881C_COMMAND_INSTR(0x12, 0x00),
	ILI9881C_COMMAND_INSTR(0x13, 0x00),
	ILI9881C_COMMAND_INSTR(0x14, 0x00),
	ILI9881C_COMMAND_INSTR(0x15, 0x00),
	ILI9881C_COMMAND_INSTR(0x16, 0x00),
	ILI9881C_COMMAND_INSTR(0x17, 0x00),
	ILI9881C_COMMAND_INSTR(0x18, 0x00),
	ILI9881C_COMMAND_INSTR(0x19, 0x00),
	ILI9881C_COMMAND_INSTR(0x1A, 0x00),
	ILI9881C_COMMAND_INSTR(0x1B, 0x00),
	ILI9881C_COMMAND_INSTR(0x1C, 0x00),
	ILI9881C_COMMAND_INSTR(0x1D, 0x00),
	ILI9881C_COMMAND_INSTR(0x1E, 0x40),
	ILI9881C_COMMAND_INSTR(0x1F, 0x80),

	ILI9881C_COMMAND_INSTR(0x20, 0x06),
	ILI9881C_COMMAND_INSTR(0x21, 0x01),
	ILI9881C_COMMAND_INSTR(0x22, 0x00),
	ILI9881C_COMMAND_INSTR(0x23, 0x00),
	ILI9881C_COMMAND_INSTR(0x24, 0x00),
	ILI9881C_COMMAND_INSTR(0x25, 0x00),
	ILI9881C_COMMAND_INSTR(0x26, 0x00),
	ILI9881C_COMMAND_INSTR(0x27, 0x00),
	ILI9881C_COMMAND_INSTR(0x28, 0x33),
	ILI9881C_COMMAND_INSTR(0x29, 0x33),
	ILI9881C_COMMAND_INSTR(0x2A, 0x00),
	ILI9881C_COMMAND_INSTR(0x2B, 0x00),
	ILI9881C_COMMAND_INSTR(0x2C, 0x00),
	ILI9881C_COMMAND_INSTR(0x2D, 0x00),
	ILI9881C_COMMAND_INSTR(0x2E, 0x00),
	ILI9881C_COMMAND_INSTR(0x2F, 0x00),

	ILI9881C_COMMAND_INSTR(0x30, 0x00),
	ILI9881C_COMMAND_INSTR(0x31, 0x00),
	ILI9881C_COMMAND_INSTR(0x32, 0x00),
	ILI9881C_COMMAND_INSTR(0x33, 0x00),
	ILI9881C_COMMAND_INSTR(0x34, 0x03),
	ILI9881C_COMMAND_INSTR(0x35, 0x00),
	ILI9881C_COMMAND_INSTR(0x36, 0x00),
	ILI9881C_COMMAND_INSTR(0x37, 0x00),
	ILI9881C_COMMAND_INSTR(0x38, 0x96),
	ILI9881C_COMMAND_INSTR(0x39, 0x00),
	ILI9881C_COMMAND_INSTR(0x3A, 0x00),
	ILI9881C_COMMAND_INSTR(0x3B, 0x00),
	ILI9881C_COMMAND_INSTR(0x3C, 0x00),
	ILI9881C_COMMAND_INSTR(0x3D, 0x00),
	ILI9881C_COMMAND_INSTR(0x3E, 0x00),
	ILI9881C_COMMAND_INSTR(0x3F, 0x00),

	ILI9881C_COMMAND_INSTR(0x40, 0x00),
	ILI9881C_COMMAND_INSTR(0x41, 0x00),
	ILI9881C_COMMAND_INSTR(0x42, 0x00),
	ILI9881C_COMMAND_INSTR(0x43, 0x00),
	ILI9881C_COMMAND_INSTR(0x44, 0x00),

	ILI9881C_COMMAND_INSTR(0x50, 0x00),
	ILI9881C_COMMAND_INSTR(0x51, 0x23),
	ILI9881C_COMMAND_INSTR(0x52, 0x45),
	ILI9881C_COMMAND_INSTR(0x53, 0x67),
	ILI9881C_COMMAND_INSTR(0x54, 0x89),
	ILI9881C_COMMAND_INSTR(0x55, 0xaB),
	ILI9881C_COMMAND_INSTR(0x56, 0x01),
	ILI9881C_COMMAND_INSTR(0x57, 0x23),
	ILI9881C_COMMAND_INSTR(0x58, 0x45),
	ILI9881C_COMMAND_INSTR(0x59, 0x67),
	ILI9881C_COMMAND_INSTR(0x5A, 0x89),
	ILI9881C_COMMAND_INSTR(0x5B, 0xAB),
	ILI9881C_COMMAND_INSTR(0x5C, 0xCD),
	ILI9881C_COMMAND_INSTR(0x5D, 0xEF),
	ILI9881C_COMMAND_INSTR(0x5E, 0x00),
	ILI9881C_COMMAND_INSTR(0x5F, 0x08),

	ILI9881C_COMMAND_INSTR(0x60, 0x08),
	ILI9881C_COMMAND_INSTR(0x61, 0x06),
	ILI9881C_COMMAND_INSTR(0x62, 0x06),
	ILI9881C_COMMAND_INSTR(0x63, 0x01),
	ILI9881C_COMMAND_INSTR(0x64, 0x01),
	ILI9881C_COMMAND_INSTR(0x65, 0x00),
	ILI9881C_COMMAND_INSTR(0x66, 0x00),
	ILI9881C_COMMAND_INSTR(0x67, 0x02),
	ILI9881C_COMMAND_INSTR(0x68, 0x15),
	ILI9881C_COMMAND_INSTR(0x69, 0x15),
	ILI9881C_COMMAND_INSTR(0x6A, 0x14),
	ILI9881C_COMMAND_INSTR(0x6B, 0x14),
	ILI9881C_COMMAND_INSTR(0x6C, 0x0D),
	ILI9881C_COMMAND_INSTR(0x6D, 0x0D),
	ILI9881C_COMMAND_INSTR(0x6E, 0x0C),
	ILI9881C_COMMAND_INSTR(0x6F, 0x0C),

	ILI9881C_COMMAND_INSTR(0x70, 0x0F),
	ILI9881C_COMMAND_INSTR(0x71, 0x0F),
	ILI9881C_COMMAND_INSTR(0x72, 0x0E),
	ILI9881C_COMMAND_INSTR(0x73, 0x0E),
	ILI9881C_COMMAND_INSTR(0x74, 0x02),
	ILI9881C_COMMAND_INSTR(0x75, 0x08),
	ILI9881C_COMMAND_INSTR(0x76, 0x08),
	ILI9881C_COMMAND_INSTR(0x77, 0x06),
	ILI9881C_COMMAND_INSTR(0x78, 0x06),
	ILI9881C_COMMAND_INSTR(0x79, 0x01),
	ILI9881C_COMMAND_INSTR(0x7A, 0x01),
	ILI9881C_COMMAND_INSTR(0x7B, 0x00),
	ILI9881C_COMMAND_INSTR(0x7C, 0x00),
	ILI9881C_COMMAND_INSTR(0x7D, 0x02),
	ILI9881C_COMMAND_INSTR(0x7E, 0x15),
	ILI9881C_COMMAND_INSTR(0x7F, 0x15),

	ILI9881C_COMMAND_INSTR(0x80, 0x14),
	ILI9881C_COMMAND_INSTR(0x81, 0x14),
	ILI9881C_COMMAND_INSTR(0x82, 0x0D),
	ILI9881C_COMMAND_INSTR(0x83, 0x0D),
	ILI9881C_COMMAND_INSTR(0x84, 0x0C),
	ILI9881C_COMMAND_INSTR(0x85, 0x0C),
	ILI9881C_COMMAND_INSTR(0x86, 0x0F),
	ILI9881C_COMMAND_INSTR(0x87, 0x0F),
	ILI9881C_COMMAND_INSTR(0x88, 0x0E),
	ILI9881C_COMMAND_INSTR(0x89, 0x0E),
	ILI9881C_COMMAND_INSTR(0x8A, 0x02),

	ILI9881C_SWITCH_PAGE_INSTR(4),
	ILI9881C_COMMAND_INSTR(0x6E, 0x2B),
	ILI9881C_COMMAND_INSTR(0x6F, 0x37),
	ILI9881C_COMMAND_INSTR(0x3A, 0x24),
	ILI9881C_COMMAND_INSTR(0x8D, 0x1A),
	ILI9881C_COMMAND_INSTR(0x87, 0xBA),
	ILI9881C_COMMAND_INSTR(0xB2, 0xD1),
	ILI9881C_COMMAND_INSTR(0x88, 0x0B),
	ILI9881C_COMMAND_INSTR(0x38, 0x01),
	ILI9881C_COMMAND_INSTR(0x39, 0x00),
	ILI9881C_COMMAND_INSTR(0xB5, 0x02),
	ILI9881C_COMMAND_INSTR(0x31, 0x25),
	ILI9881C_COMMAND_INSTR(0x3B, 0x98),

	ILI9881C_SWITCH_PAGE_INSTR(1),
	ILI9881C_COMMAND_INSTR(0x22, 0x0A),
	ILI9881C_COMMAND_INSTR(0x31, 0x00),
	ILI9881C_COMMAND_INSTR(0x53, 0x3D),
	ILI9881C_COMMAND_INSTR(0x55, 0x3D),
	ILI9881C_COMMAND_INSTR(0x50, 0xB5),
	ILI9881C_COMMAND_INSTR(0x51, 0xAD),
	ILI9881C_COMMAND_INSTR(0x60, 0x06),
	ILI9881C_COMMAND_INSTR(0x62, 0x20),

	ILI9881C_COMMAND_INSTR(0xA0, 0x00),
	ILI9881C_COMMAND_INSTR(0xA1, 0x21),
	ILI9881C_COMMAND_INSTR(0xA2, 0x35),
	ILI9881C_COMMAND_INSTR(0xA3, 0x19),
	ILI9881C_COMMAND_INSTR(0xA4, 0x1E),
	ILI9881C_COMMAND_INSTR(0xA5, 0x33),
	ILI9881C_COMMAND_INSTR(0xA6, 0x27),
	ILI9881C_COMMAND_INSTR(0xA7, 0x26),
	ILI9881C_COMMAND_INSTR(0xA8, 0xAF),
	ILI9881C_COMMAND_INSTR(0xA9, 0x1B),
	ILI9881C_COMMAND_INSTR(0xAA, 0x27),
	ILI9881C_COMMAND_INSTR(0xAB, 0x8D),
	ILI9881C_COMMAND_INSTR(0xAC, 0x1A),
	ILI9881C_COMMAND_INSTR(0xAD, 0x1B),
	ILI9881C_COMMAND_INSTR(0xAE, 0x50),
	ILI9881C_COMMAND_INSTR(0xAF, 0x26),
	ILI9881C_COMMAND_INSTR(0xB0, 0x2B),
	ILI9881C_COMMAND_INSTR(0xB1, 0x54),
	ILI9881C_COMMAND_INSTR(0xB2, 0x5E),
	ILI9881C_COMMAND_INSTR(0xB3, 0x23),

	ILI9881C_COMMAND_INSTR(0xC0, 0x00),
	ILI9881C_COMMAND_INSTR(0xC1, 0x21),
	ILI9881C_COMMAND_INSTR(0xC2, 0x35),
	ILI9881C_COMMAND_INSTR(0xC3, 0x19),
	ILI9881C_COMMAND_INSTR(0xC4, 0x1E),
	ILI9881C_COMMAND_INSTR(0xC5, 0x33),
	ILI9881C_COMMAND_INSTR(0xC6, 0x27),
	ILI9881C_COMMAND_INSTR(0xC7, 0x26),
	ILI9881C_COMMAND_INSTR(0xC8, 0xAF),
	ILI9881C_COMMAND_INSTR(0xC9, 0x1B),
	ILI9881C_COMMAND_INSTR(0xCA, 0x27),
	ILI9881C_COMMAND_INSTR(0xCB, 0x8D),
	ILI9881C_COMMAND_INSTR(0xCC, 0x1A),
	ILI9881C_COMMAND_INSTR(0xCD, 0x1B),
	ILI9881C_COMMAND_INSTR(0xCE, 0x50),
	ILI9881C_COMMAND_INSTR(0xCF, 0x26),
	ILI9881C_COMMAND_INSTR(0xD0, 0x2B),
	ILI9881C_COMMAND_INSTR(0xD1, 0x54),
	ILI9881C_COMMAND_INSTR(0xD2, 0x5E),
	ILI9881C_COMMAND_INSTR(0xD3, 0x23),
	ILI9881C_SWITCH_PAGE_INSTR(0),
	ILI9881C_COMMAND_INSTR(0x11, 0x00),
	ILI9881C_COMMAND_INSTR(0x29, 0x00),
	ILI9881C_COMMAND_INSTR(0x35, 0x00),
};

static inline struct alientek_mipi *panel_to_alientek_mipi(struct drm_panel *panel)
{
	return container_of(panel, struct alientek_mipi, panel);
}

static void hx839x_dcs_write_buf(struct alientek_mipi *ctx, const void *data,
				  size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	if (ret < 0)
		printk("MIPI DSI DCS write buffer failed: %d\n", ret);
}

#define dcs_write_seq(ctx, seq...)				\
({								\
	static const u8 d[] = { seq };				\
								\
	hx839x_dcs_write_buf(ctx, d, ARRAY_SIZE(d));		\
})

static void hx839x_init_sequence_720p(struct alientek_mipi *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	u8 mipi_data[] = {UD_SETMIPI, 0x60, 0x03, 0x68, 0x6B, 0xB2, 0xC0};

	dcs_write_seq(ctx, UD_SETADDRESSMODE, 0x01);

	dcs_write_seq(ctx, UD_SETEXTC, 0xFF, 0x83, 0x94);

	/* SETMIPI */
	mipi_data[1] = 0x60 | (dsi->lanes - 1);
	hx839x_dcs_write_buf(ctx, mipi_data, ARRAY_SIZE(mipi_data));

	dcs_write_seq(ctx, UD_SETPOWER, 0x48, 0x12, 0x72, 0x09, 0x32, 0x54,
		      0x71, 0x71, 0x57, 0x47);

	dcs_write_seq(ctx, UD_SETDISP, 0x00, 0x80, 0x64, 0x0C, 0x0D, 0x2F);

	dcs_write_seq(ctx, UD_SETCYC, 0x73, 0x74, 0x73, 0x74, 0x73, 0x74, 0x01,
		      0x0C, 0x86, 0x75, 0x00, 0x3F, 0x73, 0x74, 0x73, 0x74,
		      0x73, 0x74, 0x01, 0x0C, 0x86);

	dcs_write_seq(ctx, UD_SETGIP0, 0x00, 0x00, 0x07, 0x07, 0x40, 0x07, 0x0C,
		      0x00, 0x08, 0x10, 0x08, 0x00, 0x08, 0x54, 0x15, 0x0A,
		      0x05, 0x0A, 0x02, 0x15, 0x06, 0x05, 0x06, 0x47, 0x44,
		      0x0A, 0x0A, 0x4B, 0x10, 0x07, 0x07, 0x0C, 0x40);

	dcs_write_seq(ctx, UD_SETGIP1, 0x1C, 0x1C, 0x1D, 0x1D, 0x00, 0x01, 0x02,
		      0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
		      0x24, 0x25, 0x18, 0x18, 0x26, 0x27, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x20, 0x21, 0x18, 0x18, 0x18,
		      0x18);

	dcs_write_seq(ctx, UD_SETGIP2, 0x1C, 0x1C, 0x1D, 0x1D, 0x07, 0x06, 0x05,
		      0x04, 0x03, 0x02, 0x01, 0x00, 0x0B, 0x0A, 0x09, 0x08,
		      0x21, 0x20, 0x18, 0x18, 0x27, 0x26, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x25, 0x24, 0x18, 0x18, 0x18,
		      0x18);

	dcs_write_seq(ctx, UD_SETVCOM, 0x6E, 0x6E);

	dcs_write_seq(ctx, UD_SETGAMMA, 0x00, 0x0A, 0x15, 0x1B, 0x1E, 0x21,
		      0x24, 0x22, 0x47, 0x56, 0x65, 0x66, 0x6E, 0x82, 0x88,
		      0x8B, 0x9A, 0x9D, 0x98, 0xA8, 0xB9, 0x5D, 0x5C, 0x61,
		      0x66, 0x6A, 0x6F, 0x7F, 0x7F, 0x00, 0x0A, 0x15, 0x1B,
		      0x1E, 0x21, 0x24, 0x22, 0x47, 0x56, 0x65, 0x65, 0x6E,
		      0x81, 0x87, 0x8B, 0x98, 0x9D, 0x99, 0xA8, 0xBA, 0x5D,
		      0x5D, 0x62, 0x67, 0x6B, 0x72, 0x7F, 0x7F);
	dcs_write_seq(ctx, 0xC0, 0x1F, 0x31);
	dcs_write_seq(ctx, UD_SETPANEL, 0x03);
	dcs_write_seq(ctx, 0xD4, 0x02);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x02);
	dcs_write_seq(ctx, 0xD8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		      0xFF, 0xFF, 0xFF, 0xFF);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x00);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x01);
	dcs_write_seq(ctx, UD_SETPOWER, 0x00);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x00);
	dcs_write_seq(ctx, 0xBF, 0x40, 0x81, 0x50, 0x00, 0x1A, 0xFC, 0x01);
	dcs_write_seq(ctx, 0xC6, 0xED);
}

static void hx839x_init_sequence_1080p(struct alientek_mipi *ctx)
{
    dcs_write_seq(ctx, UD_SETEXTC, 0xFF, 0x83, 0x99);
    dcs_write_seq(ctx, UD_SETOFFSET, 0x77);
	dcs_write_seq(ctx, UD_SETPANEL, 0x04);
    dcs_write_seq(ctx, UD_SETPOWER, 0X02, 0X04, 0X74, 0X94, 0X01, 0X32,
                       0X33, 0X11, 0X11, 0XAB, 0X4D, 0X56, 0X73, 0X02, 0X02);
    dcs_write_seq(ctx, UD_SETDISP, 0x00, 0x80, 0x80, 0xAE, 0x05, 0x07, 0X5A, 
					   0X11, 0X00, 0X00, 0X10, 0X1E, 0X70, 0X03, 0XD4);
    dcs_write_seq(ctx, UD_SETCYC, 0X00, 0XFF, 0X02, 0XC0, 0X02, 0XC0, 0X00, 0X00, 0X08, 0X00, 0X04, 0X06, 0X00,
					   0X32, 0X04, 0X0A, 0X08, 0X21, 0X03, 0X01, 0X00, 0X0F, 0XB8, 0X8B, 0X02, 0XC0, 0X02,
					   0XC0, 0X00, 0X00, 0X08, 0X00, 0X04, 0X06, 0X00, 0X32, 0X04, 0X0A, 0X08, 0X01, 0X00,
					   0X0F, 0XB8, 0X01);
    dcs_write_seq(ctx, UD_SETGIP0, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X06, 0X00, 0X00, 0X10, 0X04, 0X00, 0X04,
					   0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X01, 0X00, 0X05, 0X05,
					   0X07, 0X00, 0X00, 0X00, 0X05, 0X40);
    dcs_write_seq(ctx, UD_SETGIP1, 0X18, 0X18, 0X19, 0X19, 0X18, 0X18, 0X21, 0X20, 0X01, 0X00, 0X07, 0X06, 0X05,
					   0X04, 0X03, 0X02, 0X18, 0X18, 0X18, 0X18, 0X18, 0X18, 0X2F, 0X2F, 0X30, 0X30, 0X31,
					   0X31, 0X18, 0X18, 0X18, 0X18);
    dcs_write_seq(ctx, UD_SETGIP2, 0X18, 0X18, 0X19, 0X19, 0X40, 0X40, 0X20, 0X21, 0X02, 0X03, 0X04, 0X05, 0X06,
					   0X07, 0X00, 0X01, 0X40, 0X40, 0X40, 0X40, 0X40, 0X40, 0X2F, 0X2F, 0X30, 0X30, 0X31,
					   0X31, 0X40, 0X40, 0X40, 0X40);
	dcs_write_seq(ctx, UD_SETGIP3, 0XA2, 0XAA, 0X02, 0XA0, 0XA2, 0XA8, 0X02, 0XA0, 0XB0, 0X00, 0X00, 0X00, 0XB0,
					   0X00, 0X00, 0X00);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x01);
	dcs_write_seq(ctx, UD_SETGIP3, 0XB0, 0X00, 0X00, 0X00, 0XB0, 0X00, 0X00, 0X00, 0XE2, 0XAA, 0X03, 0XF0, 0XE2,
					   0XAA, 0X03, 0XF0);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x02);
	dcs_write_seq(ctx, UD_SETGIP3, 0XE2, 0XAA, 0X03, 0XF0, 0XE2, 0XAA, 0X03, 0XF0);	
	dcs_write_seq(ctx, UD_SETREGBANK, 0x00);
    dcs_write_seq(ctx, UD_SETVCOM, 0X8D, 0X8D);
    dcs_write_seq(ctx, UD_SETGAMMA, 0X00, 0X0E, 0X19, 0X13, 0X2E, 0X39, 0X48, 0X44, 0X4D, 0X57, 0X5F, 0X66, 0X6C,
					   0X76, 0X7F, 0X85, 0X8A, 0X95, 0X9A, 0XA4, 0X9B, 0XAB, 0XB0, 0X5C, 0X58, 0X64, 0X77,
					   0X00, 0X0E, 0X19, 0X13, 0X2E, 0X39, 0X48, 0X44, 0X4D, 0X57, 0X5F, 0X66, 0X6C, 0X76,
					   0X7F, 0X85, 0X8A, 0X95, 0X9A, 0XA4, 0X9B, 0XAB, 0XB0, 0X5C, 0X58, 0X64, 0X77);
}

static int alientek_mipi_disable(struct drm_panel *panel)
{
	int ret;
	struct alientek_mipi *ctx = panel_to_alientek_mipi(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	if(!ctx->enabled)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if(ret)
		printk("failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if(ret)
		printk("failed to enter sleep mode: %d\n", ret);

	msleep(120);

	ctx->enabled = false;

	return 0;
}

static int alientek_mipi_unprepare(struct drm_panel *panel)
{
	int ret;
	struct alientek_mipi *ctx = panel_to_alientek_mipi(panel);

	if(!ctx->prepared)
		return 0;

	if(ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
	}

	ret = regulator_disable(ctx->power);
	if(ret < 0) {
		printk("failed to disable power: %d\n", ret);
		return ret;
	}

	ctx->prepared = false;

	return 0;
}

static int alientek_mipi_prepare(struct drm_panel *panel)
{
	int ret;
	struct alientek_mipi *ctx = panel_to_alientek_mipi(panel);

	if (ctx->prepared)
		return 0;

	ret = regulator_enable(ctx->power);
	if(ret) {
		printk("failed to enable power: %d\n", ret);
		return ret;
	}

	if(ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(55);
	}

	ctx->prepared = true;
	return 0;
}

static int alientek_mipi_enable(struct drm_panel *panel)
{
	int ret, i;
    int dsi_select_id;	
    struct device_node *np = NULL;
	const struct ili9881c_instr *instr;
	struct alientek_mipi *ctx = panel_to_alientek_mipi(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	if (ctx->enabled)
		return 0;

    np = of_find_node_by_name(NULL, "dsi_lcd_id");
	if(!np) {
		printk("no found dsi_lcd_id\n");
		return -ENODEV;
	}
    ret = of_property_read_u32(np,"dsi_select_id", &dsi_select_id);
	if(ret < 0)
		return -ENODEV;

    //printk("ATK-DEBUG:dsi_select_id = %d\n", dsi_select_id);

	if(dsi_select_id == atk_mipi_dsi_5x5_720x1280) {
		hx839x_init_sequence_720p(ctx);
	} else if(dsi_select_id == atk_mipi_dsi_5x5_1080x1920) {
		hx839x_init_sequence_1080p(ctx);
	} else if(dsi_select_id == atk_mipi_dsi_10x1_800x1280) {
		for(i = 0; i < ARRAY_SIZE(alientek_init_ili9881c); i++) {
			instr = &alientek_init_ili9881c[i];
			if(instr->op == ILI9881C_SWITCH_PAGE)
				ret = ili9881c_switch_page(ctx, instr->arg.page);
			else if(instr->op == ILI9881C_COMMAND)
				ret = ili9881c_send_cmd_data(ctx, instr->arg.cmd.cmd,
								instr->arg.cmd.data);
			if(ret)
				return ret;
		}
		ret = ili9881c_switch_page(ctx, 0);
		if(ret)
			return ret;
	} else {
		hx839x_init_sequence_720p(ctx);
	}

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if(ret < 0) {
		printk("failed to set tear ON (%d)\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if(ret)
		return ret;

	msleep(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if(ret)
		return ret;

	msleep(50);

	ctx->enabled = true;
	
	return 0;
}

static int alientek_mipi_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
    int ret;
    int dsi_select_id;	
	struct drm_display_mode *mode;
    struct device_node *np = NULL;

    np = of_find_node_by_name(NULL, "dsi_lcd_id");
	if(!np) {
		printk("no found dsi_lcd_id\n");
		return -ENODEV;
	}
    ret = of_property_read_u32(np,"dsi_select_id", &dsi_select_id);
	if(ret < 0) 
		return -ENODEV;

	if(dsi_select_id == atk_mipi_dsi_5x5_720x1280) {
		mode = drm_mode_duplicate(connector->dev, &atk_mipi_dsi_5x5_720x1280_mode);
		if(!mode) {
			printk("failed to add mode %ux%u@%u\n",
				atk_mipi_dsi_5x5_720x1280_mode.hdisplay, atk_mipi_dsi_5x5_720x1280_mode.vdisplay,
				drm_mode_vrefresh(&atk_mipi_dsi_5x5_720x1280_mode));
			return -ENOMEM;
		}
	} else if(dsi_select_id == atk_mipi_dsi_5x5_1080x1920) {
		mode = drm_mode_duplicate(connector->dev, &atk_mipi_dsi_5x5_1080x1920_mode);
		if(!mode) {
			printk("failed to add mode %ux%u@%u\n",
				atk_mipi_dsi_5x5_1080x1920_mode.hdisplay, atk_mipi_dsi_5x5_1080x1920_mode.vdisplay,
				drm_mode_vrefresh(&atk_mipi_dsi_5x5_1080x1920_mode));
			return -ENOMEM;
		}
	} else if(dsi_select_id == atk_mipi_dsi_10x1_800x1280) {
		mode = drm_mode_duplicate(connector->dev, &atk_mipi_dsi_10x1_800x1280_mode);
		if(!mode) {
			printk("failed to add mode %ux%u@%u\n",
				atk_mipi_dsi_10x1_800x1280_mode.hdisplay, atk_mipi_dsi_10x1_800x1280_mode.vdisplay,
				drm_mode_vrefresh(&atk_mipi_dsi_10x1_800x1280_mode));
			return -ENOMEM;
		}
	} else {
		mode = drm_mode_duplicate(connector->dev, &atk_mipi_dsi_5x5_720x1280_mode);
		if(!mode) {
			printk("failed to add mode %ux%u@%u\n",
				atk_mipi_dsi_5x5_720x1280_mode.hdisplay, atk_mipi_dsi_5x5_720x1280_mode.vdisplay,
				drm_mode_vrefresh(&atk_mipi_dsi_5x5_720x1280_mode));
			return -ENOMEM;
		}
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs alientek_mipi_drm_funcs = {
	.disable = alientek_mipi_disable,
	.unprepare = alientek_mipi_unprepare,
	.prepare = alientek_mipi_prepare,
	.enable = alientek_mipi_enable,
	.get_modes = alientek_mipi_get_modes,
};

static int alientek_mipi_probe(struct mipi_dsi_device *dsi)
{
	int ret;	
	struct device *dev = &dsi->dev;
	struct alientek_mipi *ctx;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if(!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if(IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		printk("failed to get reset GPIO: %d\n", ret);
		return ret;
	}

	ctx->power = devm_regulator_get(dev, "power");
	if(IS_ERR(ctx->power)) {
		ret = PTR_ERR(ctx->power);
		printk("cannot get regulator: %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(dev->of_node, "dsi-lanes", &dsi->lanes);
	if(ret) {
		printk("failed to get dsi-lanes property: %d\n", ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dev = dev;
	ctx->dsi = dsi;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
 
	drm_panel_init(&ctx->panel, dev, &alientek_mipi_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if(ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if(ret < 0) {
		printk("mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void alientek_mipi_remove(struct mipi_dsi_device *dsi)
{
	struct alientek_mipi *ctx = mipi_dsi_get_drvdata(dsi);
	
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id alientek_mipi_of_match[] = {
	{ .compatible = "alientek,mipi-lcd"},
	{ }
};
MODULE_DEVICE_TABLE(of, alientek_mipi_of_match);

static struct mipi_dsi_driver alientek_mipi_driver = {
	.probe = alientek_mipi_probe,
	.remove = alientek_mipi_remove,
	.driver = {
		.name = "panel-alientek-mipi",
		.of_match_table = alientek_mipi_of_match,
	},
};
module_mipi_dsi_driver(alientek_mipi_driver);

MODULE_AUTHOR("zengjianpai <marc07@qq.com>");
MODULE_DESCRIPTION("DRM Driver for ALIENTEK MIPI DSI panel");
MODULE_LICENSE("GPL v2");
