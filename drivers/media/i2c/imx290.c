// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX290 CMOS Image Sensor Driver
 *
 * Copyright (C) 2019 FRAMOS GmbH.
 *
 * Copyright (C) 2019 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX290_STANDBY 0x3000
#define IMX290_REGHOLD 0x3001
#define IMX290_XMSTA 0x3002
#define IMX290_BLKLEVEL_LOW 0x300a
#define IMX290_BLKLEVEL_HIGH 0x300b
#define IMX290_GAIN 0x3014
#define IMX290_PGCTRL 0x308c

#define IMX290_PGCTRL_REGEN BIT(0)
#define IMX290_PGCTRL_THRU BIT(1)
#define IMX290_PGCTRL_MODE(n) ((n) << 4)

#define V4L2_CID_IMX290_CG_SWITCH	(V4L2_CID_USER_BASE | 0x1000)

enum {
	COND_25_FPS = BIT(0),
	COND_30_FPS = BIT(1),
	COND_50_FPS = BIT(2),
	COND_60_FPS = BIT(3),
	COND_25_30_FPS = COND_25_FPS | COND_30_FPS,
	COND_50_60_FPS = COND_50_FPS | COND_60_FPS,
	COND_FPS_msk = (COND_25_FPS | COND_30_FPS |
			COND_50_FPS | COND_60_FPS),

	COND_2_LANES = BIT(4),
	COND_4_LANES = BIT(5),
	COND_LANES_msk = COND_2_LANES | COND_4_LANES,

	COND_INCK_37 = BIT(6),
	COND_INCK_74 = BIT(7),
	COND_INCK_msk = COND_INCK_37 | COND_INCK_74,
};

enum imx290_fps {
	FPS_25,
	FPS_30,
	FPS_50,
	FPS_60,
};

enum imx290_inck {
	INCK_37,
	INCK_74,
};

enum imx290_type {
	IMX290_TYPE_290,
	IMX290_TYPE_327,
};

static const char * const imx290_supply_name[] = {
	"vdda",
	"vddd",
	"vdddo",
};

#define IMX290_NUM_SUPPLIES ARRAY_SIZE(imx290_supply_name)

struct imx290_regval {
	u16 reg;
	u8 val;
	u8 cond;
};

struct imx290_mode {
	u32 width;
	u32 height;
	u8 link_freq_index;

	const struct imx290_regval *data;
	u32 data_size;
};

struct imx290 {
	struct device *dev;
	struct clk *xclk;
	struct regmap *regmap;
	u8 nlanes;
	u8 bpp;
	enum imx290_fps fps;
	enum imx290_inck inck;
	enum imx290_type type;

	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_mbus_framefmt current_format;
	const struct imx290_mode *current_mode;

	struct regulator_bulk_data supplies[IMX290_NUM_SUPPLIES];
	struct gpio_desc *rst_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;

	uint8_t reg_3007;
	unsigned int vmax;

	struct mutex lock;
};

struct imx290_pixfmt {
	u32 code;
	u8 bpp;
};

struct imx290_driver_data {
	enum imx290_type type;
	unsigned int max_gain;
};

static const struct imx290_pixfmt imx290_formats[] = {
	{ MEDIA_BUS_FMT_SRGGB10_1X10, 10 },
	{ MEDIA_BUS_FMT_SRGGB12_1X12, 12 },
};

static const struct regmap_config imx290_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
};

static const char * const imx290_test_pattern_menu[] = {
	"Disabled",
	"Sequence Pattern 1",
	"Horizontal Color-bar Chart",
	"Vertical Color-bar Chart",
	"Sequence Pattern 2",
	"Gradation Pattern 1",
	"Gradation Pattern 2",
	"000/555h Toggle Pattern",
};

static const struct imx290_regval imx290_global_init_settings[] = {
	/* frsel */
	{ 0x3009, 0x01, COND_50_60_FPS },
	{ 0x3009, 0x02, COND_25_30_FPS },

	/* repetition */
	{ 0x3405, 0x10, COND_25_30_FPS | COND_2_LANES },
	{ 0x3405, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3405, 0x20, COND_25_30_FPS | COND_4_LANES },
	{ 0x3405, 0x10, COND_50_60_FPS | COND_4_LANES },

	{ 0x3040, 0x00 },
	{ 0x3041, 0x00 },
	{ 0x303c, 0x00 },
	{ 0x303d, 0x00 },
	{ 0x3042, 0x9c },
	{ 0x3043, 0x07 },
	{ 0x303e, 0x49 },
	{ 0x303f, 0x04 },
	{ 0x304b, 0x0a },
};

static const struct imx290_regval imx290_1080p_settings[] = {
	/* mode settings */
	{ 0x3007, 0x00 },

	/* hmax */
	{ 0x301c, 0xa0, COND_25_FPS },
	{ 0x301d, 0x14, COND_25_FPS },

	{ 0x301c, 0x30, COND_30_FPS },
	{ 0x301d, 0x11, COND_30_FPS },

	{ 0x301c, 0x50, COND_50_FPS },
	{ 0x301d, 0x0a, COND_50_FPS },

	{ 0x301c, 0x98, COND_60_FPS },
	{ 0x301d, 0x08, COND_60_FPS },

	/* vmax */
	{ 0x3018, 0x65 },
	{ 0x3019, 0x04 },
	{ 0x301a, 0x00 },

	{ 0x303a, 0x0c },
	{ 0x3414, 0x0a },
	{ 0x3472, 0x80 },
	{ 0x3473, 0x07 },
	{ 0x3418, 0x38 },
	{ 0x3419, 0x04 },

	{ 0x3012, 0x64 },
	{ 0x3013, 0x00 },

	{ 0x305c, 0x18, COND_INCK_37 },
	{ 0x305d, 0x03, COND_INCK_37 },
	{ 0x305e, 0x20, COND_INCK_37 },
	{ 0x305f, 0x01, COND_INCK_37 },
	{ 0x315e, 0x1a, COND_INCK_37 },
	{ 0x3164, 0x1a, COND_INCK_37 },
	{ 0x3444, 0x20, COND_INCK_37 },
	{ 0x3445, 0x25, COND_INCK_37 },
	{ 0x3480, 0x49, COND_INCK_37 },

	{ 0x305c, 0x0c, COND_INCK_74 },
	{ 0x305d, 0x03, COND_INCK_74 },
	{ 0x305e, 0x10, COND_INCK_74 },
	{ 0x305f, 0x01, COND_INCK_74 },
	{ 0x315e, 0x1b, COND_INCK_74 },
	{ 0x3164, 0x1b, COND_INCK_74 },
	{ 0x3444, 0x40, COND_INCK_74 },
	{ 0x3445, 0x4a, COND_INCK_74 },
	{ 0x3480, 0x92, COND_INCK_74 },

	/* data rate settings */

	/* mipi timing - 2 lane, 25/30 fps */
	{ 0x3446, 0x57, COND_25_30_FPS | COND_2_LANES },
	{ 0x3447, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3448, 0x37, COND_25_30_FPS | COND_2_LANES },
	{ 0x3449, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x344a, 0x1f, COND_25_30_FPS | COND_2_LANES },
	{ 0x344b, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x344c, 0x1f, COND_25_30_FPS | COND_2_LANES },
	{ 0x344d, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x344e, 0x1f, COND_25_30_FPS | COND_2_LANES },
	{ 0x344f, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3450, 0x77, COND_25_30_FPS | COND_2_LANES },
	{ 0x3451, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3452, 0x1f, COND_25_30_FPS | COND_2_LANES },
	{ 0x3453, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3454, 0x17, COND_25_30_FPS | COND_2_LANES },
	{ 0x3455, 0x00, COND_25_30_FPS | COND_2_LANES },

	/* mipi timing - 2 lane, 50/60 fps */
	{ 0x3446, 0x77, COND_50_60_FPS | COND_2_LANES },
	{ 0x3447, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3448, 0x67, COND_50_60_FPS | COND_2_LANES },
	{ 0x3449, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x344a, 0x47, COND_50_60_FPS | COND_2_LANES },
	{ 0x344b, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x344c, 0x37, COND_50_60_FPS | COND_2_LANES },
	{ 0x344d, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x344e, 0x3f, COND_50_60_FPS | COND_2_LANES },
	{ 0x344f, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3450, 0xff, COND_50_60_FPS | COND_2_LANES },
	{ 0x3451, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3452, 0x3f, COND_50_60_FPS | COND_2_LANES },
	{ 0x3453, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3454, 0x37, COND_50_60_FPS | COND_2_LANES },
	{ 0x3455, 0x00, COND_50_60_FPS | COND_2_LANES },

	/* mipi timing - 4 lane, 25/30 fps */
	{ 0x3446, 0x47, COND_25_30_FPS | COND_4_LANES },
	{ 0x3447, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3448, 0x1f, COND_25_30_FPS | COND_4_LANES },
	{ 0x3449, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x344a, 0x17, COND_25_30_FPS | COND_4_LANES },
	{ 0x344b, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x344c, 0x0f, COND_25_30_FPS | COND_4_LANES },
	{ 0x344d, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x344e, 0x17, COND_25_30_FPS | COND_4_LANES },
	{ 0x344f, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3450, 0x47, COND_25_30_FPS | COND_4_LANES },
	{ 0x3451, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3452, 0x0f, COND_25_30_FPS | COND_4_LANES },
	{ 0x3453, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3454, 0x0f, COND_25_30_FPS | COND_4_LANES },
	{ 0x3455, 0x00, COND_25_30_FPS | COND_4_LANES },

	/* mipi timing - 2 lane, 50/60 fps */
	{ 0x3446, 0x57, COND_50_60_FPS | COND_4_LANES },
	{ 0x3447, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3448, 0x37, COND_50_60_FPS | COND_4_LANES },
	{ 0x3449, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x344a, 0x1f, COND_50_60_FPS | COND_4_LANES },
	{ 0x344b, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x344c, 0x1f, COND_50_60_FPS | COND_4_LANES },
	{ 0x344d, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x344e, 0x1f, COND_50_60_FPS | COND_4_LANES },
	{ 0x344f, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3450, 0x77, COND_50_60_FPS | COND_4_LANES },
	{ 0x3451, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3452, 0x1f, COND_50_60_FPS | COND_4_LANES },
	{ 0x3453, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3454, 0x17, COND_50_60_FPS | COND_4_LANES },
	{ 0x3455, 0x00, COND_50_60_FPS | COND_4_LANES },
};

static const struct imx290_regval imx290_720p_settings[] = {
	/* mode settings */
	{ 0x3007, 0x10 },

	/* hmax */
	{ 0x301c, 0xf0, COND_25_FPS },
	{ 0x301d, 0x1e, COND_25_FPS },

	{ 0x301c, 0xc8, COND_30_FPS },
	{ 0x301d, 0x19, COND_30_FPS },

	{ 0x301c, 0x78, COND_50_FPS },
	{ 0x301d, 0x0f, COND_50_FPS },

	{ 0x301c, 0xe4, COND_60_FPS },
	{ 0x301d, 0x0c, COND_60_FPS },

	/* vmax */
	{ 0x3018, 0xee },
	{ 0x3019, 0x02 },
	{ 0x301a, 0x00 },

	{ 0x303a, 0x06 },
	{ 0x3414, 0x04 },
	{ 0x3472, 0x00 },
	{ 0x3473, 0x05 },
	{ 0x3418, 0xd0 },
	{ 0x3419, 0x02 },

	{ 0x3012, 0x64 },
	{ 0x3013, 0x00 },

	{ 0x305c, 0x20, COND_INCK_37 },
	{ 0x305d, 0x00, COND_INCK_37 },
	{ 0x305e, 0x20, COND_INCK_37 },
	{ 0x305f, 0x01, COND_INCK_37 },
	{ 0x315e, 0x1a, COND_INCK_37 },
	{ 0x3164, 0x1a, COND_INCK_37 },
	{ 0x3444, 0x20, COND_INCK_37 },
	{ 0x3445, 0x25, COND_INCK_37 },
	{ 0x3480, 0x49, COND_INCK_37 },

	{ 0x305c, 0x10, COND_INCK_74 },
	{ 0x305d, 0x00, COND_INCK_74 },
	{ 0x305e, 0x10, COND_INCK_74 },
	{ 0x305f, 0x01, COND_INCK_74 },
	{ 0x315e, 0x1b, COND_INCK_74 },
	{ 0x3164, 0x1b, COND_INCK_74 },
	{ 0x3444, 0x40, COND_INCK_74 },
	{ 0x3445, 0x4a, COND_INCK_74 },
	{ 0x3480, 0x92, COND_INCK_74 },

	/* data rate settings */
	/* mipi timing - 2 lane, 25/30 fps */
	{ 0x3446, 0x4f, COND_25_30_FPS | COND_2_LANES },
	{ 0x3447, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3448, 0x2f, COND_25_30_FPS | COND_2_LANES },
	{ 0x3449, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x344a, 0x17, COND_25_30_FPS | COND_2_LANES },
	{ 0x344b, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x344c, 0x17, COND_25_30_FPS | COND_2_LANES },
	{ 0x344d, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x344e, 0x17, COND_25_30_FPS | COND_2_LANES },
	{ 0x344f, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3450, 0x57, COND_25_30_FPS | COND_2_LANES },
	{ 0x3451, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3452, 0x17, COND_25_30_FPS | COND_2_LANES },
	{ 0x3453, 0x00, COND_25_30_FPS | COND_2_LANES },
	{ 0x3454, 0x17, COND_25_30_FPS | COND_2_LANES },
	{ 0x3455, 0x00, COND_25_30_FPS | COND_2_LANES },

	/* mipi timing - 2 lane, 50/60 fps */
	{ 0x3446, 0x67, COND_50_60_FPS | COND_2_LANES },
	{ 0x3447, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3448, 0x57, COND_50_60_FPS | COND_2_LANES },
	{ 0x3449, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x344a, 0x2f, COND_50_60_FPS | COND_2_LANES },
	{ 0x344b, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x344c, 0x27, COND_50_60_FPS | COND_2_LANES },
	{ 0x344d, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x344e, 0x2f, COND_50_60_FPS | COND_2_LANES },
	{ 0x344f, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3450, 0xbf, COND_50_60_FPS | COND_2_LANES },
	{ 0x3451, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3452, 0x2f, COND_50_60_FPS | COND_2_LANES },
	{ 0x3453, 0x00, COND_50_60_FPS | COND_2_LANES },
	{ 0x3454, 0x27, COND_50_60_FPS | COND_2_LANES },
	{ 0x3455, 0x00, COND_50_60_FPS | COND_2_LANES },

	/* mipi timing - 4 lane, 25/30 fps */
	{ 0x3446, 0x47, COND_25_30_FPS | COND_4_LANES },
	{ 0x3447, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3448, 0x17, COND_25_30_FPS | COND_4_LANES },
	{ 0x3449, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x344a, 0x0f, COND_25_30_FPS | COND_4_LANES },
	{ 0x344b, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x344c, 0x0f, COND_25_30_FPS | COND_4_LANES },
	{ 0x344d, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x344e, 0x0f, COND_25_30_FPS | COND_4_LANES },
	{ 0x344f, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3450, 0x2b, COND_25_30_FPS | COND_4_LANES },
	{ 0x3451, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3452, 0x0b, COND_25_30_FPS | COND_4_LANES },
	{ 0x3453, 0x00, COND_25_30_FPS | COND_4_LANES },
	{ 0x3454, 0x0f, COND_25_30_FPS | COND_4_LANES },
	{ 0x3455, 0x00, COND_25_30_FPS | COND_4_LANES },

	/* mipi timing - 2 lane, 50/60 fps */
	{ 0x3446, 0x4f, COND_50_60_FPS | COND_4_LANES },
	{ 0x3447, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3448, 0x2f, COND_50_60_FPS | COND_4_LANES },
	{ 0x3449, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x344a, 0x17, COND_50_60_FPS | COND_4_LANES },
	{ 0x344b, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x344c, 0x17, COND_50_60_FPS | COND_4_LANES },
	{ 0x344d, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x344e, 0x17, COND_50_60_FPS | COND_4_LANES },
	{ 0x344f, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3450, 0x57, COND_50_60_FPS | COND_4_LANES },
	{ 0x3451, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3452, 0x17, COND_50_60_FPS | COND_4_LANES },
	{ 0x3453, 0x00, COND_50_60_FPS | COND_4_LANES },
	{ 0x3454, 0x17, COND_50_60_FPS | COND_4_LANES },
	{ 0x3455, 0x00, COND_50_60_FPS | COND_4_LANES },
};

static const struct imx290_regval imx290_poweron_settings[] = {
	{ 0x3000, 0x01 },
	{ 0x3001, 0x00 },
	{ 0x3002, 0x01 },

	/* physical-lane-num */
	{ 0x3407, 0x01, COND_2_LANES },
	{ 0x3407, 0x03, COND_4_LANES },

	/* csi-lane-num */
	{ 0x3443, 0x01, COND_2_LANES },
	{ 0x3443, 0x03, COND_4_LANES },
};

/* The red "Set to" values in reference manual v0.5.0 (2018-07-22) */
static const struct imx290_regval imx290_model_290_settings[] = {
	{ 0x300f, 0x00 },
	{ 0x3010, 0x21 },
	{ 0x3012, 0x64 },
	{ 0x3016, 0x09 },
	{ 0x3070, 0x02 },
	{ 0x3071, 0x11 },
	{ 0x309b, 0x10 },
	{ 0x309c, 0x22 },
	{ 0x30a2, 0x02 },
	{ 0x30a6, 0x20 },
	{ 0x30a8, 0x20 },
	{ 0x30aa, 0x20 },
	{ 0x30ac, 0x20 },
	{ 0x30b0, 0x43 },
	{ 0x3119, 0x9e },
	{ 0x311c, 0x1e },
	{ 0x311e, 0x08 },
	{ 0x3128, 0x05 },
	{ 0x313d, 0x83 },
	{ 0x3150, 0x03 },
	{ 0x317e, 0x00 },
	{ 0x32b8, 0x50 },
	{ 0x32b9, 0x10 },
	{ 0x32ba, 0x00 },
	{ 0x32bb, 0x04 },
	{ 0x32c8, 0x50 },
	{ 0x32c9, 0x10 },
	{ 0x32ca, 0x00 },
	{ 0x32cb, 0x04 },
	{ 0x332c, 0xd3 },
	{ 0x332d, 0x10 },
	{ 0x332e, 0x0d },
	{ 0x3358, 0x06 },
	{ 0x3359, 0xe1 },
	{ 0x335a, 0x11 },
	{ 0x3360, 0x1e },
	{ 0x3361, 0x61 },
	{ 0x3362, 0x10 },
	{ 0x33b0, 0x50 },
	{ 0x33b2, 0x1a },
	{ 0x33b3, 0x04 },
};

/* The red "Set to" values in reference manual v0.2 (2017-05-25) */
static const struct imx290_regval imx290_model_327_settings[] = {
	{ 0x3011, 0x0a },
	{ 0x309e, 0x4a },
	{ 0x309f, 0x4a },
	{ 0x3128, 0x04 },
	{ 0x313b, 0x41 },
};

static const struct imx290_regval imx290_10bit_settings[] = {
	{ 0x3005, 0x00},
	{ 0x3046, 0x00},
	{ 0x3129, 0x1d},
	{ 0x317c, 0x12},
	{ 0x31ec, 0x37},
	{ 0x3441, 0x0a},
	{ 0x3442, 0x0a},
	{ 0x300a, 0x3c},
	{ 0x300b, 0x00},
};

static const struct imx290_regval imx290_12bit_settings[] = {
	{ 0x3005, 0x01 },
	{ 0x3046, 0x01 },
	{ 0x3129, 0x00 },
	{ 0x317c, 0x00 },
	{ 0x31ec, 0x0e },
	{ 0x3441, 0x0c },
	{ 0x3442, 0x0c },
	{ 0x300a, 0xf0 },
	{ 0x300b, 0x00 },
};

/* supported link frequencies */
#define FREQ_INDEX_1080P	0
#define FREQ_INDEX_720P		1
static const s64 imx290_link_freq_2lanes_37mhz[] = {
	[FREQ_INDEX_1080P] = 445500000,
	[FREQ_INDEX_720P] = 297000000,
};
static const s64 imx290_link_freq_4lanes_37mhz[] = {
	[FREQ_INDEX_1080P] = 222750000,
	[FREQ_INDEX_720P] = 148500000,
};

static const s64 imx290_link_freq_2lanes_74mhz[] = {
	[FREQ_INDEX_1080P] = 891000000,
	[FREQ_INDEX_720P] = 594000000,
};
static const s64 imx290_link_freq_4lanes_74mhz[] = {
	[FREQ_INDEX_1080P] = 445500000,
	[FREQ_INDEX_720P] = 297000000,
};

/*
 * In this function and in the similar ones below We rely on imx290_probe()
 * to ensure that nlanes is either 2 or 4.
 */
static inline const s64 *imx290_link_freqs_ptr(const struct imx290 *imx290)
{
	switch (((imx290->nlanes == 4) ? BIT(4) : 0) |
		((imx290->inck == INCK_74) ? BIT(0) : 0)) {
	case 0x00:	return imx290_link_freq_2lanes_37mhz;
	case 0x01:	return imx290_link_freq_2lanes_74mhz;
	case 0x10:	return imx290_link_freq_4lanes_37mhz;
	case 0x11:	return imx290_link_freq_4lanes_74mhz;
	default:
		BUG();
	}
}

static inline int imx290_link_freqs_num(const struct imx290 *imx290)
{
	switch (((imx290->nlanes == 4) ? BIT(4) : 0) |
		((imx290->inck == INCK_74) ? BIT(0) : 0)) {
	case 0x00:	return ARRAY_SIZE(imx290_link_freq_2lanes_37mhz);
	case 0x01:	return ARRAY_SIZE(imx290_link_freq_2lanes_74mhz);
	case 0x10:	return ARRAY_SIZE(imx290_link_freq_4lanes_37mhz);
	case 0x11:	return ARRAY_SIZE(imx290_link_freq_4lanes_74mhz);
	default:
		BUG();
	}
}

/* Mode configs */
static const struct imx290_mode imx290_modes_2lanes[] = {
	{
		.width = 1920,
		.height = 1080,
		.link_freq_index = FREQ_INDEX_1080P,
		.data = imx290_1080p_settings,
		.data_size = ARRAY_SIZE(imx290_1080p_settings),
	},
	{
		.width = 1280,
		.height = 720,
		.link_freq_index = FREQ_INDEX_720P,
		.data = imx290_720p_settings,
		.data_size = ARRAY_SIZE(imx290_720p_settings),
	},
};

static const struct imx290_mode imx290_modes_4lanes[] = {
	{
		.width = 1920,
		.height = 1080,
		.link_freq_index = FREQ_INDEX_1080P,
		.data = imx290_1080p_settings,
		.data_size = ARRAY_SIZE(imx290_1080p_settings),
	},
	{
		.width = 1280,
		.height = 720,
		.link_freq_index = FREQ_INDEX_720P,
		.data = imx290_720p_settings,
		.data_size = ARRAY_SIZE(imx290_720p_settings),
	},
};

static inline const struct imx290_mode *imx290_modes_ptr(const struct imx290 *imx290)
{
	if (imx290->nlanes == 2)
		return imx290_modes_2lanes;
	else
		return imx290_modes_4lanes;
}

static inline int imx290_modes_num(const struct imx290 *imx290)
{
	if (imx290->nlanes == 2)
		return ARRAY_SIZE(imx290_modes_2lanes);
	else
		return ARRAY_SIZE(imx290_modes_4lanes);
}

static inline struct imx290 *to_imx290(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx290, sd);
}

static inline int imx290_read_reg(struct imx290 *imx290, u16 addr, u8 *value)
{
	unsigned int regval;
	int ret;

	ret = regmap_read(imx290->regmap, addr, &regval);
	if (ret) {
		dev_err(imx290->dev, "I2C read failed for addr: %x\n", addr);
		return ret;
	}

	*value = regval & 0xff;

	return 0;
}

static int imx290_write_reg(struct imx290 *imx290, u16 addr, u8 value)
{
	int ret;

	ret = regmap_write(imx290->regmap, addr, value);
	if (ret) {
		dev_err(imx290->dev, "I2C write failed for addr: %x\n", addr);
		return ret;
	}

	return ret;
}

static bool imx290_settings_match(struct imx290 *imx290, uint8_t cond)
{
	bool	reject = false;

	if (cond & COND_FPS_msk) {
		switch (imx290->fps) {
		case FPS_25:
			reject |= !(cond & COND_25_FPS);
			break;
		case FPS_30:
			reject |= !(cond & COND_30_FPS);
			break;
		case FPS_50:
			reject |= !(cond & COND_50_FPS);
			break;
		case FPS_60:
			reject |= !(cond & COND_60_FPS);
			break;
		default:
			reject |= true;
			break;
		}
	}

	if (cond & COND_LANES_msk) {
		switch (imx290->nlanes) {
		case 2:
			reject |= !(cond & COND_2_LANES);
			break;
		case 4:
			reject |= !(cond & COND_4_LANES);
			break;
		default:
			reject |= true;
			break;
		}
	}

	if (cond & COND_INCK_msk) {
		switch (imx290->inck) {
		case INCK_37:
			reject |= !(cond & COND_INCK_74);
			break;
		case INCK_74:
			reject |= !(cond & COND_INCK_37);
			break;
		default:
			reject |= true;
			break;
		}
	}

	return !reject;
}

static int imx290_set_register_array(struct imx290 *imx290,
				     const struct imx290_regval *settings,
				     unsigned int num_settings)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_settings; ++i, ++settings) {
		if (!imx290_settings_match(imx290, settings->cond))
			continue;

		ret = imx290_write_reg(imx290, settings->reg, settings->val);
		if (ret < 0)
			return ret;
	}

	/* Provide 10ms settle time */
	usleep_range(10000, 11000);

	return 0;
}

static int imx290_write_buffered_reg(struct imx290 *imx290, u16 address_low,
				     u8 nr_regs, u32 value)
{
	unsigned int i;
	int ret;

	ret = imx290_write_reg(imx290, IMX290_REGHOLD, 0x01);
	if (ret) {
		dev_err(imx290->dev, "Error setting hold register\n");
		return ret;
	}

	for (i = 0; i < nr_regs; i++) {
		ret = imx290_write_reg(imx290, address_low + i,
				       (u8)(value >> (i * 8)));
		if (ret) {
			dev_err(imx290->dev, "Error writing buffered registers\n");
			return ret;
		}
	}

	ret = imx290_write_reg(imx290, IMX290_REGHOLD, 0x00);
	if (ret) {
		dev_err(imx290->dev, "Error setting hold register\n");
		return ret;
	}

	return ret;
}

static int imx290_set_gain(struct imx290 *imx290, u32 value)
{
	int ret;

	ret = imx290_write_buffered_reg(imx290, IMX290_GAIN, 1, value);
	if (ret)
		dev_err(imx290->dev, "Unable to write gain\n");

	return ret;
}

/* Stop streaming */
static int imx290_stop_streaming(struct imx290 *imx290)
{
	int ret;

	ret = imx290_write_reg(imx290, IMX290_STANDBY, 0x01);
	if (ret < 0)
		return ret;

	msleep(30);

	return imx290_write_reg(imx290, IMX290_XMSTA, 0x01);
}

static uint8_t imx290_get_winmode(struct imx290 const *imx290)
{
	if (!imx290->current_mode)
		return 0;

	switch (imx290->current_mode->height) {
	case 1080:
		return (0u << 4);
	case 720:
		return (1u << 4);
	default:
		/* TODO: this is unsupported; emit an error? */
		return 0;
	}
}

static int imx290_set_exposure(struct imx290 *imx290, struct v4l2_ctrl *ctrl)
{
	unsigned int val = ctrl->val;

	if (imx290->vmax == 0)
		return 0;

	/*
	 * TODO: this just ensures that we stay within the 1..(h-1) range.
	 * This should be changed so that exposure time can be expressed in ms
	 * or so
	 */

	val *= imx290->vmax - 2;
	val /= ctrl->maximum;

	if (val == 0)
		val = 1;

	val  = imx290->vmax - 2 - val;

	{
		struct imx290_regval const regs[] = {
			{ 0x3001, 0x01 },	/* reghold */
			{ 0x3020, (val >>  0) & 0xff },
			{ 0x3021, (val >>  8) & 0xff },
			{ 0x3022, (val >> 16) & 0x01 },
			{ 0x3001, 0x00 },	/* reghold */
		};

		return imx290_set_register_array(imx290, regs, ARRAY_SIZE(regs));
	}
}

static int imx290_set_flip(struct imx290 *imx290, struct v4l2_ctrl *ctrl)
{
	uint8_t		msk = ctrl->id == V4L2_CID_HFLIP ? BIT(1) : BIT(0);
	uint8_t		r3007;
	int		rc;

	mutex_lock(&imx290->lock);

	r3007 = imx290->reg_3007;
	if (ctrl->val)
		r3007 |=  msk;
	else
		r3007 &= ~msk;

	{
		struct imx290_regval const regs[] = {
			{ 0x3001, 0x01 },	/* reghold */
			{ 0x3007, r3007 },
			{ 0x3001, 0x00 },	/* reghold */
		};

		rc = imx290_set_register_array(imx290,
					       regs, ARRAY_SIZE(regs));
	}

	if (rc < 0)
		goto out;

	imx290->reg_3007 = r3007;

	rc = 0;

out:
	mutex_unlock(&imx290->lock);

	return rc;
}

static int imx290_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx290 *imx290 = container_of(ctrl->handler,
					     struct imx290, ctrls);
	int ret = 0;

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(imx290->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		ret = imx290_set_gain(imx290, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		if (ctrl->val) {
			imx290_write_reg(imx290, IMX290_BLKLEVEL_LOW, 0x00);
			imx290_write_reg(imx290, IMX290_BLKLEVEL_HIGH, 0x00);
			usleep_range(10000, 11000);
			imx290_write_reg(imx290, IMX290_PGCTRL,
					 (u8)(IMX290_PGCTRL_REGEN |
					 IMX290_PGCTRL_THRU |
					 IMX290_PGCTRL_MODE(ctrl->val)));
		} else {
			imx290_write_reg(imx290, IMX290_PGCTRL, 0x00);
			usleep_range(10000, 11000);
			if (imx290->bpp == 10)
				imx290_write_reg(imx290, IMX290_BLKLEVEL_LOW,
						 0x3c);
			else /* 12 bits per pixel */
				imx290_write_reg(imx290, IMX290_BLKLEVEL_LOW,
						 0xf0);
			imx290_write_reg(imx290, IMX290_BLKLEVEL_HIGH, 0x00);
		}
		break;

	case V4L2_CID_EXPOSURE:
		ret = imx290_set_exposure(imx290, ctrl);
		break;

	case V4L2_CID_VFLIP:
	case V4L2_CID_HFLIP:
		ret = imx290_set_flip(imx290, ctrl);
		break;

	case V4L2_CID_IMX290_CG_SWITCH: {
		uint8_t	v = ctrl->val == 0 ? 0 : BIT(4);

		struct imx290_regval const regs[] = {
			{ 0x3001, 0x01 },	/* reghold */
			{ 0x3009, 0x01 | v, COND_50_60_FPS },
			{ 0x3009, 0x02 | v, COND_25_30_FPS },
			{ 0x3001, 0x00 },	/* reghold */
		};

		ret = imx290_set_register_array(imx290, regs, ARRAY_SIZE(regs));
		break;
	}

	case V4L2_CID_LINK_FREQ:
	case V4L2_CID_PIXEL_RATE:
		/* read-only, index always valid */
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx290->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx290_ctrl_ops = {
	.s_ctrl = imx290_set_ctrl,
};

static int imx290_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(imx290_formats))
		return -EINVAL;

	code->code = imx290_formats[code->index].code;

	return 0;
}

static int imx290_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	const struct imx290 *imx290 = to_imx290(sd);
	const struct imx290_mode *imx290_modes = imx290_modes_ptr(imx290);

	if ((fse->code != imx290_formats[0].code) &&
	    (fse->code != imx290_formats[1].code))
		return -EINVAL;

	if (fse->index >= imx290_modes_num(imx290))
		return -EINVAL;

	fse->min_width = imx290_modes[fse->index].width;
	fse->max_width = imx290_modes[fse->index].width;
	fse->min_height = imx290_modes[fse->index].height;
	fse->max_height = imx290_modes[fse->index].height;

	return 0;
}

/*
 * TODO: this should be improved; we can setup non-discrete frame rates by
 * modifying vmax
 *
 * NOTE: this list must be ordered!
 */
static struct v4l2_fract const imx290_intervals[] = {
	[FPS_25] = { 1, 25 },
	[FPS_30] = { 1, 30 },
	[FPS_50] = { 1, 50 },
	[FPS_60] = { 1, 60 },
};

static int imx290_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_pad_config *cfg,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	const struct imx290 *imx290 = to_imx290(sd);
	const struct imx290_mode *imx290_modes = imx290_modes_ptr(imx290);
	size_t i;

	if (fie->index >= ARRAY_SIZE(imx290_intervals))
		return -EINVAL;

	if (fie->code != MEDIA_BUS_FMT_SRGGB10_1X10 &&
	    fie->code != MEDIA_BUS_FMT_SRGGB12_1X12)
		return -EINVAL;

	for (i = imx290_modes_num(imx290); i > 0; --i) {
		const struct imx290_mode *mode = &imx290_modes[i - 1];

		if (mode->width == fie->width &&
		    mode->height == fie->height) {
			fie->interval = imx290_intervals[fie->index];
			return 0;
		}

	}

	return -EINVAL;
}

static int mipi_csis_g_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *interval)
{
	const struct imx290 *imx290 = to_imx290(sd);

	interval->interval = imx290_intervals[imx290->fps];

	return 0;
}

static int mipi_csis_s_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *interval)
{
	struct imx290 *imx290 = to_imx290(sd);
	enum imx290_fps match = 0;
	size_t i;

	unsigned long a = interval->interval.numerator;
	unsigned long b = interval->interval.denominator;

	for (i = 0; i < ARRAY_SIZE(imx290_intervals); ++i) {
		match = i;

		if (a * imx290_intervals[i].denominator >=
		    b * imx290_intervals[i].numerator)
			break;
	}

	interval->interval = imx290_intervals[match];
	imx290->fps = match;

	return 0;
}

static int imx290_g_parm(struct v4l2_subdev *sd,
			 struct v4l2_streamparm *a)
{
	struct v4l2_subdev_frame_interval interval;
	int rc;

	rc = mipi_csis_g_frame_interval(sd, &interval);
	if (rc < 0)
		return rc;

	memset(a->parm.capture.reserved, 0, sizeof(a->parm.capture.reserved));
	a->parm.capture.capability   = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = interval.interval;

	return 0;
}

static int imx290_s_parm(struct v4l2_subdev *sd,
			 struct v4l2_streamparm *a)
{
	struct v4l2_subdev_frame_interval interval = {
		.interval = a->parm.capture.timeperframe,
	};
	int rc;

	memset(a->parm.capture.reserved, 0, sizeof(a->parm.capture.reserved));
	rc = mipi_csis_s_frame_interval(sd, &interval);
	if (rc < 0)
		return rc;

	a->parm.capture.capability   = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = interval.interval;

	return 0;
}

static int imx290_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx290 *imx290 = to_imx290(sd);
	struct v4l2_mbus_framefmt *framefmt;

	mutex_lock(&imx290->lock);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		framefmt = v4l2_subdev_get_try_format(&imx290->sd, cfg,
						      fmt->pad);
	else
		framefmt = &imx290->current_format;

	fmt->format = *framefmt;

	mutex_unlock(&imx290->lock);

	return 0;
}

static inline u8 imx290_get_link_freq_index(struct imx290 *imx290)
{
	return imx290->current_mode->link_freq_index;
}

static s64 imx290_get_link_freq(struct imx290 *imx290)
{
	u8 index = imx290_get_link_freq_index(imx290);

	return *(imx290_link_freqs_ptr(imx290) + index);
}

static u64 imx290_calc_pixel_rate(struct imx290 *imx290)
{
	s64 link_freq = imx290_get_link_freq(imx290);
	u8 nlanes = imx290->nlanes;
	u64 pixel_rate;

	/* pixel rate = link_freq * 2 * nr_of_lanes / bits_per_sample */
	pixel_rate = link_freq * 2 * nlanes;
	do_div(pixel_rate, imx290->bpp);
	return pixel_rate;
}

static int imx290_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx290 *imx290 = to_imx290(sd);
	const struct imx290_mode *mode;
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	mutex_lock(&imx290->lock);

	mode = v4l2_find_nearest_size(imx290_modes_ptr(imx290),
				      imx290_modes_num(imx290), width, height,
				      fmt->format.width, fmt->format.height);

	fmt->format.width = mode->width;
	fmt->format.height = mode->height;

	for (i = 0; i < ARRAY_SIZE(imx290_formats); i++)
		if (imx290_formats[i].code == fmt->format.code)
			break;

	if (i >= ARRAY_SIZE(imx290_formats))
		i = 0;

	fmt->format.code = imx290_formats[i].code;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		format = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
	} else {
		format = &imx290->current_format;
		imx290->current_mode = mode;
		imx290->bpp = imx290_formats[i].bpp;

		if (imx290->link_freq)
			__v4l2_ctrl_s_ctrl(imx290->link_freq,
					   imx290_get_link_freq_index(imx290));
		if (imx290->pixel_rate)
			__v4l2_ctrl_s_ctrl_int64(imx290->pixel_rate,
						 imx290_calc_pixel_rate(imx290));
	}

	*format = fmt->format;

	mutex_unlock(&imx290->lock);

	return 0;
}

static int imx290_entity_init_cfg(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_pad_config *cfg)
{
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = cfg ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.format.width = 1920;
	fmt.format.height = 1080;

	imx290_set_fmt(subdev, cfg, &fmt);

	return 0;
}

static int imx290_write_current_format(struct imx290 *imx290)
{
	int ret;

	switch (imx290->current_format.code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		ret = imx290_set_register_array(imx290, imx290_10bit_settings,
						ARRAY_SIZE(
							imx290_10bit_settings));
		if (ret < 0) {
			dev_err(imx290->dev, "Could not set format registers\n");
			return ret;
		}
		break;
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		ret = imx290_set_register_array(imx290, imx290_12bit_settings,
						ARRAY_SIZE(
							imx290_12bit_settings));
		if (ret < 0) {
			dev_err(imx290->dev, "Could not set format registers\n");
			return ret;
		}
		break;
	default:
		dev_err(imx290->dev, "Unknown pixel format\n");
		return -EINVAL;
	}

	return 0;
}

/* Start streaming */
static int imx290_start_streaming(struct imx290 *imx290)
{
	int ret;

	/* Set init register settings */
	ret = imx290_set_register_array(imx290, imx290_global_init_settings,
					ARRAY_SIZE(
						imx290_global_init_settings));
	if (ret < 0) {
		dev_err(imx290->dev, "Could not set init registers\n");
		return ret;
	}

	/* Apply the register values related to current frame format */
	ret = imx290_write_current_format(imx290);
	if (ret < 0) {
		dev_err(imx290->dev, "Could not set frame format\n");
		return ret;
	}

	/* Apply default values of current mode */
	ret = imx290_set_register_array(imx290, imx290->current_mode->data,
					imx290->current_mode->data_size);
	if (ret < 0) {
		dev_err(imx290->dev, "Could not set current mode\n");
		return ret;
	}

	mutex_lock(&imx290->lock);
	imx290->reg_3007 &= ~(7 << 4);	/* mask out WINMODE */
	imx290->reg_3007 |= imx290_get_winmode(imx290);

	/*
	 * TODO: solve this somehow else... vmax can be changed but modifies
	 * frame rate in turn
	 */
	if (imx290->current_mode->height == 1080)
		imx290->vmax = 1125;
	else
		imx290->vmax =  750;
	mutex_unlock(&imx290->lock);

	/* Apply customized values from user */
	ret = v4l2_ctrl_handler_setup(imx290->sd.ctrl_handler);
	if (ret) {
		dev_err(imx290->dev, "Could not sync v4l2 controls\n");
		return ret;
	}

	ret = imx290_write_reg(imx290, IMX290_STANDBY, 0x00);
	if (ret < 0)
		return ret;

	msleep(30);

	/* Start streaming */
	return imx290_write_reg(imx290, IMX290_XMSTA, 0x00);
}

static int imx290_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx290 *imx290 = to_imx290(sd);
	int ret = 0;

	if (enable) {
		ret = pm_runtime_get_sync(imx290->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(imx290->dev);
			goto unlock_and_return;
		}

		ret = imx290_start_streaming(imx290);
		if (ret) {
			dev_err(imx290->dev, "Start stream failed\n");
			pm_runtime_put(imx290->dev);
			goto unlock_and_return;
		}
	} else {
		imx290_stop_streaming(imx290);
		pm_runtime_put(imx290->dev);
	}

unlock_and_return:

	return ret;
}

static int imx290_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx290 *imx290 = to_imx290(sd);

	if (on)
		pm_runtime_get_sync(imx290->dev);
	else {
		pm_runtime_put_noidle(imx290->dev);
		pm_schedule_suspend(imx290->dev, 2000);
	}

	return 0;
}

static int imx290_get_regulators(struct device *dev, struct imx290 *imx290)
{
	unsigned int i;

	for (i = 0; i < IMX290_NUM_SUPPLIES; i++)
		imx290->supplies[i].supply = imx290_supply_name[i];

	return devm_regulator_bulk_get(dev, IMX290_NUM_SUPPLIES,
				       imx290->supplies);
}

static int imx290_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx290 *imx290 = to_imx290(sd);
	int ret;

	ret = clk_prepare_enable(imx290->xclk);
	if (ret) {
		dev_err(imx290->dev, "Failed to enable clock\n");
		return ret;
	}

	ret = regulator_bulk_enable(IMX290_NUM_SUPPLIES, imx290->supplies);
	if (ret) {
		dev_err(imx290->dev, "Failed to enable regulators\n");
		goto err_clk;
	}

	usleep_range(1, 2);
	gpiod_set_value_cansleep(imx290->rst_gpio, 0);
	usleep_range(30000, 31000);

	ret = imx290_set_register_array(imx290, imx290_poweron_settings,
						ARRAY_SIZE(imx290_poweron_settings));
	if (ret < 0)
		goto err_regulator;

	switch (imx290->type) {
	case IMX290_TYPE_290:
		ret = imx290_set_register_array(imx290, imx290_model_290_settings,
						ARRAY_SIZE(imx290_model_290_settings));
		break;
	case IMX290_TYPE_327:
		ret = imx290_set_register_array(imx290, imx290_model_327_settings,
						ARRAY_SIZE(imx290_model_327_settings));
		break;
	}

	if (ret < 0)
		goto err_regulator;

	return 0;

err_regulator:
	gpiod_set_value_cansleep(imx290->rst_gpio, 1);
	regulator_bulk_disable(IMX290_NUM_SUPPLIES, imx290->supplies);

err_clk:
	clk_disable_unprepare(imx290->xclk);

	return ret;
}

static int imx290_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx290 *imx290 = to_imx290(sd);

	clk_disable_unprepare(imx290->xclk);
	gpiod_set_value_cansleep(imx290->rst_gpio, 1);
	regulator_bulk_disable(IMX290_NUM_SUPPLIES, imx290->supplies);

	return 0;
}

static const struct dev_pm_ops imx290_pm_ops = {
	SET_RUNTIME_PM_OPS(imx290_power_off, imx290_power_on, NULL)
};

static const struct v4l2_subdev_video_ops imx290_video_ops = {
	.g_parm = imx290_g_parm,
	.s_parm = imx290_s_parm,
	.s_stream = imx290_set_stream,
	.s_frame_interval = mipi_csis_s_frame_interval,
	.g_frame_interval = mipi_csis_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx290_pad_ops = {
	.init_cfg = imx290_entity_init_cfg,
	.enum_mbus_code = imx290_enum_mbus_code,
	.enum_frame_size = imx290_enum_frame_size,
	.enum_frame_interval = imx290_enum_frame_interval,
	.get_fmt = imx290_get_fmt,
	.set_fmt = imx290_set_fmt,
};

static struct v4l2_subdev_core_ops const imx290_core_ops = {
	.s_power = imx290_s_power,
};

static const struct v4l2_subdev_ops imx290_subdev_ops = {
	.video = &imx290_video_ops,
	.pad = &imx290_pad_ops,
	.core = &imx290_core_ops,
};

static const struct media_entity_operations imx290_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static char const * const imx290_cg_switching_menu[] = {
	"HCG",
	"LCG",
};

static const struct v4l2_ctrl_config imx290_ctrls[] = {
	{
		.id	= V4L2_CID_IMX290_CG_SWITCH,
		.name	= "Conversion Gain Switching",
		.type	= V4L2_CTRL_TYPE_MENU,
		.ops	= &imx290_ctrl_ops,
		.qmenu	= imx290_cg_switching_menu,
		.max	= ARRAY_SIZE(imx290_cg_switching_menu) - 1,
	}, {
		.name	= "hflip",
		.id	= V4L2_CID_HFLIP,
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.ops	= &imx290_ctrl_ops,
		.step	= 1,
		.max	= 1,
	}, {
		.name	= "vflip",
		.id	= V4L2_CID_VFLIP,
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.ops	= &imx290_ctrl_ops,
		.step	= 1,
		.max	= 1,
	}, {
		.id	= V4L2_CID_TEST_PATTERN,
		.name	= "test pattern",
		.type	= V4L2_CTRL_TYPE_MENU,
		.ops	= &imx290_ctrl_ops,
		.qmenu	= imx290_test_pattern_menu,
		.max	= ARRAY_SIZE(imx290_test_pattern_menu) - 1,
	}, {
		.id	= V4L2_CID_GAIN,
		.name	= "gain",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.ops	= &imx290_ctrl_ops,
		.step	= 1,
	}, {
		.id	= V4L2_CID_EXPOSURE,
		.name	= "exposure",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.ops	= &imx290_ctrl_ops,
		.min	= 0,
		.max	= 10000,
		.def	= 10000,
		.step	= 1,
	}, {
		.id	= V4L2_CID_LINK_FREQ,
		.name	= "link freq",
		.type	= V4L2_CTRL_TYPE_INTEGER_MENU,
		.ops	= &imx290_ctrl_ops,
		.flags	= V4L2_CTRL_FLAG_READ_ONLY,
	}, {
		.id	= V4L2_CID_PIXEL_RATE,
		.name	= "pixel rate",
		.type	= V4L2_CTRL_TYPE_INTEGER64,
		.ops	= &imx290_ctrl_ops,
		.flags	= V4L2_CTRL_FLAG_READ_ONLY,
		.min	= 0,
		.max	= INT_MAX,
		.step	= 1,
	},
};

static int imx290_init_ctrls(struct imx290 *imx290,
			     struct imx290_driver_data const *drv_data)
{
	struct device *dev = imx290->dev;
	int ret;
	struct v4l2_ctrl_handler *hdl;
	size_t i;
	struct v4l2_ctrl_config tmp_config;

	v4l2_ctrl_handler_init(&imx290->ctrls, ARRAY_SIZE(imx290_ctrls));

	hdl = &imx290->ctrls;

	for (i = 0; i < ARRAY_SIZE(imx290_ctrls); ++i) {
		const struct v4l2_ctrl_config *config = &imx290_ctrls[i];
		struct v4l2_ctrl **res_ctrl = NULL;
		struct v4l2_ctrl *ctrl;

		switch (config->id) {
		case V4L2_CID_GAIN:
			tmp_config = *config;
			tmp_config.max = drv_data->max_gain;
			config = &tmp_config;
			break;

		case V4L2_CID_LINK_FREQ:
			tmp_config = *config;
			tmp_config.qmenu_int = imx290_link_freqs_ptr(imx290);
			tmp_config.max = imx290_link_freqs_num(imx290) - 1;
			config = &tmp_config;
			res_ctrl = &imx290->link_freq;
			break;

		case V4L2_CID_PIXEL_RATE:
			tmp_config = *config;
			tmp_config.def = imx290_calc_pixel_rate(imx290);
			config = &tmp_config;
			res_ctrl = &imx290->pixel_rate;
			break;

		default:
			break;
		}

		ctrl = v4l2_ctrl_new_custom(hdl, config, NULL);
		ret = imx290->ctrls.error;
		if (ret) {
			dev_err(dev, "initialization error of control '%s': %d\n",
				config->name, ret);

			v4l2_ctrl_handler_free(&imx290->ctrls);
			goto out;
		}

		if (res_ctrl) {
			WARN_ON(!ctrl);
			*res_ctrl = ctrl;
		}
	}

	imx290->sd.ctrl_handler = hdl;

	ret = 0;

out:
	return ret;
}

/*
 * Returns 0 if all link frequencies used by the driver for the given number
 * of MIPI data lanes are mentioned in the device tree, or the value of the
 * first missing frequency otherwise.
 */
static s64 imx290_check_link_freqs(const struct imx290 *imx290,
				   const struct v4l2_fwnode_endpoint *ep)
{
	int i, j;
	const s64 *freqs = imx290_link_freqs_ptr(imx290);
	int freqs_count = imx290_link_freqs_num(imx290);

	for (i = 0; i < freqs_count; i++) {
		for (j = 0; j < ep->nr_of_link_frequencies; j++)
			if (freqs[i] == ep->link_frequencies[j])
				break;
		if (j == ep->nr_of_link_frequencies)
			return freqs[i];
	}
	return 0;
}

static int imx290_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	/* Only CSI2 is supported for now: */
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct imx290_driver_data const *drv_data = NULL;
	struct imx290 *imx290;
	u32 xclk_freq;
	s64 fq;
	int ret;

	if (dev_fwnode(&client->dev))
		drv_data = device_get_match_data(&client->dev);

	if (!drv_data) {
		dev_err(dev, "missing driver data\n");
		return -EINVAL;
	}

	imx290 = devm_kzalloc(dev, sizeof(*imx290), GFP_KERNEL);
	if (!imx290)
		return -ENOMEM;

	imx290->type = drv_data->type;
	imx290->dev = dev;
	imx290->regmap = devm_regmap_init_i2c(client, &imx290_regmap_config);
	if (IS_ERR(imx290->regmap)) {
		dev_err(dev, "Unable to initialize I2C\n");
		return -ENODEV;
	}

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "Endpoint node not found\n");
		return -EINVAL;
	}

	mutex_init(&imx290->lock);

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret == -ENXIO) {
		dev_err(dev, "Unsupported bus type, should be CSI2\n");
		goto free_err;
	} else if (ret) {
		dev_err(dev, "Parsing endpoint node failed\n");
		goto free_err;
	}

	/* Get number of data lanes */
	imx290->nlanes = ep.bus.mipi_csi2.num_data_lanes;
	if (imx290->nlanes != 2 && imx290->nlanes != 4) {
		dev_err(dev, "Invalid data lanes: %d\n", imx290->nlanes);
		ret = -EINVAL;
		goto free_err;
	}

	dev_dbg(dev, "Using %u data lanes\n", imx290->nlanes);

	/* get system clock (xclk) */
	imx290->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(imx290->xclk)) {
		dev_err(dev, "Could not get xclk");
		ret = PTR_ERR(imx290->xclk);
		goto free_err;
	}

	ret = fwnode_property_read_u32(dev_fwnode(dev), "clock-frequency",
				       &xclk_freq);
	if (ret) {
		dev_err(dev, "Could not get xclk frequency\n");
		goto free_err;
	}

	switch (xclk_freq) {
	case 37125000:
		imx290->inck = INCK_37;
		imx290->fps = FPS_30;
		break;
	case 74250000:
		imx290->inck = INCK_74;
		imx290->fps = FPS_60;
		break;
	default:
		dev_err(dev, "External clock frequency %u is not supported\n",
			xclk_freq);
		ret = -EINVAL;
		goto free_err;
	}

	/*
	 *NOTE: imx290_check_link_freqs() must be called after setting
	 * imx290->inck
	 */
	if (!ep.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property not found in DT\n");
		ret = -EINVAL;
		goto free_err;
	}

	/* Check that link frequences for all the modes are in device tree */
	fq = imx290_check_link_freqs(imx290, &ep);
	if (fq) {
		dev_err(dev, "Link frequency of %lld is not supported\n", fq);
		ret = -EINVAL;
		goto free_err;
	}

	ret = clk_set_rate(imx290->xclk, xclk_freq);
	if (ret) {
		dev_err(dev, "Could not set xclk frequency\n");
		goto free_err;
	}

	ret = imx290_get_regulators(dev, imx290);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Cannot get regulators\n");
		goto free_err;
	}

	imx290->rst_gpio = devm_gpiod_get_optional(dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(imx290->rst_gpio)) {
		ret = PTR_ERR(imx290->rst_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Cannot get reset gpio\n");
		goto free_err;
	}

	/*
	 * Initialize the frame format. In particular, imx290->current_mode
	 * and imx290->bpp are set to defaults: imx290_calc_pixel_rate() call
	 * below relies on these fields.
	 */
	imx290_entity_init_cfg(&imx290->sd, NULL);

	ret = imx290_init_ctrls(imx290, drv_data);
	if (ret <  0)
		goto free_err;

	v4l2_i2c_subdev_init(&imx290->sd, client, &imx290_subdev_ops);
	imx290->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx290->sd.dev = &client->dev;
	imx290->sd.entity.ops = &imx290_subdev_entity_ops;
	imx290->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	imx290->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx290->sd.entity, 1, &imx290->pad);
	if (ret < 0) {
		dev_err(dev, "Could not register media entity\n");
		goto free_ctrl;
	}

	ret = v4l2_async_register_subdev(&imx290->sd);
	if (ret < 0) {
		dev_err(dev, "Could not register v4l2 device\n");
		goto free_entity;
	}

	/* Power on the device to match runtime PM state below */
	ret = imx290_power_on(dev);
	if (ret < 0) {
		dev_err(dev, "Could not power on the device\n");
		goto free_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	v4l2_fwnode_endpoint_free(&ep);

	return 0;

free_entity:
	media_entity_cleanup(&imx290->sd.entity);
free_ctrl:
	v4l2_ctrl_handler_free(&imx290->ctrls);
free_err:
	mutex_destroy(&imx290->lock);
	v4l2_fwnode_endpoint_free(&ep);

	return ret;
}

static int imx290_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx290 *imx290 = to_imx290(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	mutex_destroy(&imx290->lock);

	pm_runtime_disable(imx290->dev);
	if (!pm_runtime_status_suspended(imx290->dev))
		imx290_power_off(imx290->dev);
	pm_runtime_set_suspended(imx290->dev);

	return 0;
}

static const struct imx290_driver_data ixm290_driver_data_imx290 = {
	.type		= IMX290_TYPE_290,
	.max_gain	= 240,
};

static const struct imx290_driver_data ixm290_driver_data_imx327 = {
	.type		= IMX290_TYPE_327,
	.max_gain	= 230,
};

static const struct of_device_id imx290_of_match[] = {
	{ .compatible = "sony,imx290", .data = &ixm290_driver_data_imx290 },
	{ .compatible = "sony,imx327", .data = &ixm290_driver_data_imx327 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx290_of_match);

static struct i2c_driver imx290_i2c_driver = {
	.probe_new  = imx290_probe,
	.remove = imx290_remove,
	.driver = {
		.name  = "imx290",
		.pm = &imx290_pm_ops,
		.of_match_table = of_match_ptr(imx290_of_match),
	},
};

module_i2c_driver(imx290_i2c_driver);

MODULE_DESCRIPTION("Sony IMX290 CMOS Image Sensor Driver");
MODULE_AUTHOR("FRAMOS GmbH");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_LICENSE("GPL v2");
