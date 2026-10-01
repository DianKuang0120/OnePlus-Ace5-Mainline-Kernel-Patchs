// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PM8550B High-Voltage Haptics (HAP525_HV) driver.
 *
 * Drives the LRA through the PMIC's HV haptics block and exposes it as an
 * input FF_RUMBLE device so that userspace (fftest, feedbackd, Android ...)
 * can request vibration with a magnitude and duration.
 *
 * Register sequences are derived from the Qualcomm downstream
 * "qcom-hv-haptics" driver, trimmed down to the DIRECT_PLAY path.  LRA
 * closed-loop calibration, FIFO/pattern playback and the SWR path are not
 * implemented (the open-loop period from DT plus auto-resonance is used).
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

/* CFG block registers */
#define HAP_CFG_HW_CONFIG	0x0D
#define HAP_HV_DRIVER_BIT	BIT(1)
#define HAP_CFG_EN_CTL		0x46
#define HAP_EN_BIT		BIT(7)
#define HAP_CFG_VMAX		0x48
#define HAP_CFG_DRV_WF_SEL	0x49
#define HAP_DRV_WF_FMT_BIT	BIT(4)
#define HAP_DRV_WF_SEL_MASK	GENMASK(1, 0)
#define HAP_CFG_BRAKE_MODE_CFG	0x50
#define HAP_BRAKE_MODE_MASK	GENMASK(7, 6)
#define HAP_BRAKE_WF_SEL_MASK	GENMASK(1, 0)
#define HAP_CFG_SPMI_PLAY	0x4C
#define HAP_PLAY_EN_BIT		BIT(7)
#define HAP_CFG_TLRA_OL_HIGH	0x5C
#define HAP_TLRA_OL_MSB_MASK	GENMASK(3, 0)
#define HAP_TLRA_OL_LSB_MASK	GENMASK(7, 0)
#define HAP_CFG_AUTORES		0x63
#define HAP_AUTORES_EN_BIT	BIT(7)
#define HAP_CFG_FAULT_CLR	0x66
#define HAP_FAULT_CLR_VAL	(BIT(2) | BIT(1) | BIT(0))
#define HAP_CFG_VSET		0x68
#define HAP_FORCE_VREG_RDY_BIT	BIT(0)
#define HAP_CFG_CAL_EN		0x72
#define HAP_CAL_RC_CLK_MASK	GENMASK(3, 2)
#define HAP_CAL_RC_CLK_AUTO	1

/* PTN block register */
#define HAP_PTN_DIRECT_PLAY	0x26

/* DIRECT_PLAY is pattern source index 1 */
#define HAP_SRC_DIRECT_PLAY	1

#define HAP_TLRA_STEP_US	5
#define HAP_VMAX_HV_STEP_MV	50
#define HAP_VMAX_MV_STEP_MV	32
#define HAP_MAX_HV_VMAX_MV	10000

struct hap_mini {
	struct device		*dev;
	struct regmap		*regmap;
	struct input_dev	*input;
	u32			cfg_base;
	u32			ptn_base;
	u32			boost_base;
	u32			vmax_mv;
	u32			lra_period_us;
	u32			drv_wf;
	u32			brake_mode;
	bool			is_hv;
	u8			cur_amp;
	struct mutex		lock;
};

static int hap_wr(struct hap_mini *h, u32 base, u8 off, u8 val)
{
	return regmap_write(h->regmap, base + off, val);
}

static int hap_upd(struct hap_mini *h, u32 base, u8 off, u8 mask, u8 val)
{
	return regmap_update_bits(h->regmap, base + off, mask, val);
}

static int hap_init_hw(struct hap_mini *h)
{
	unsigned int v;
	u8 val[2];
	u32 tmp;
	int rc;

	rc = regmap_read(h->regmap, h->cfg_base + HAP_CFG_HW_CONFIG, &v);
	if (rc)
		return rc;
	h->is_hv = v & HAP_HV_DRIVER_BIT;
	dev_info(h->dev, "hw_config=%#x hv=%d\n", v, h->is_hv);

	/* Limiter on the configured VMAX */
	if (h->vmax_mv > HAP_MAX_HV_VMAX_MV)
		h->vmax_mv = HAP_MAX_HV_VMAX_MV;

	/* Driver waveform shape + 2's complement data format */
	rc = hap_upd(h, h->cfg_base, HAP_CFG_DRV_WF_SEL,
		     HAP_DRV_WF_SEL_MASK | HAP_DRV_WF_FMT_BIT,
		     (h->drv_wf & HAP_DRV_WF_SEL_MASK) | HAP_DRV_WF_FMT_BIT);
	if (rc)
		return rc;

	/* Brake mode / waveform */
	rc = hap_upd(h, h->cfg_base, HAP_CFG_BRAKE_MODE_CFG,
		     HAP_BRAKE_MODE_MASK | HAP_BRAKE_WF_SEL_MASK,
		     FIELD_PREP(HAP_BRAKE_MODE_MASK, h->brake_mode) |
		     FIELD_PREP(HAP_BRAKE_WF_SEL_MASK, h->drv_wf));
	if (rc)
		return rc;

	/* Automatic RC clock calibration */
	rc = hap_upd(h, h->cfg_base, HAP_CFG_CAL_EN, HAP_CAL_RC_CLK_MASK,
		     FIELD_PREP(HAP_CAL_RC_CLK_MASK, HAP_CAL_RC_CLK_AUTO));
	if (rc)
		return rc;

	/* Open-loop LRA period */
	tmp = h->lra_period_us / HAP_TLRA_STEP_US;
	val[0] = (tmp >> 8) & HAP_TLRA_OL_MSB_MASK;
	val[1] = tmp & HAP_TLRA_OL_LSB_MASK;
	rc = regmap_bulk_write(h->regmap, h->cfg_base + HAP_CFG_TLRA_OL_HIGH,
			       val, 2);
	if (rc)
		return rc;

	/* VMAX */
	val[0] = h->vmax_mv /
		 (h->is_hv ? HAP_VMAX_HV_STEP_MV : HAP_VMAX_MV_STEP_MV);
	rc = hap_wr(h, h->cfg_base, HAP_CFG_VMAX, val[0]);
	if (rc)
		return rc;

	/* Enable the module; for HAP525_HV this also votes hBoost */
	return hap_wr(h, h->cfg_base, HAP_CFG_EN_CTL, HAP_EN_BIT);
}

/* amplitude 0 stops the LRA, 1..255 sets the DIRECT_PLAY amplitude */
static int hap_vibrate(struct hap_mini *h, u8 amplitude)
{
	int rc = 0;

	mutex_lock(&h->lock);

	if (amplitude == h->cur_amp) {
		mutex_unlock(&h->lock);
		return 0;
	}

	if (amplitude) {
		hap_wr(h, h->cfg_base, HAP_CFG_FAULT_CLR, HAP_FAULT_CLR_VAL);
		hap_upd(h, h->cfg_base, HAP_CFG_VSET,
			HAP_FORCE_VREG_RDY_BIT, HAP_FORCE_VREG_RDY_BIT);
		hap_upd(h, h->cfg_base, HAP_CFG_AUTORES,
			HAP_AUTORES_EN_BIT, HAP_AUTORES_EN_BIT);
		hap_wr(h, h->ptn_base, HAP_PTN_DIRECT_PLAY, amplitude);
	}

	rc = hap_wr(h, h->cfg_base, HAP_CFG_SPMI_PLAY,
		    amplitude ? (HAP_SRC_DIRECT_PLAY | HAP_PLAY_EN_BIT)
			      : HAP_SRC_DIRECT_PLAY);
	if (!amplitude)
		hap_upd(h, h->cfg_base, HAP_CFG_VSET,
			HAP_FORCE_VREG_RDY_BIT, 0);

	if (!rc)
		h->cur_amp = amplitude;

	mutex_unlock(&h->lock);
	return rc;
}

static int hap_ff_play(struct input_dev *dev, void *data,
		       struct ff_effect *effect)
{
	struct hap_mini *h = input_get_drvdata(dev);
	u16 mag;

	/* FF magnitude is 0..0xffff; DIRECT_PLAY takes 0..0xff */
	mag = max(effect->u.rumble.strong_magnitude,
		  effect->u.rumble.weak_magnitude);

	return hap_vibrate(h, (u8)(mag >> 8));
}

static void hap_ff_close(struct input_dev *dev)
{
	struct hap_mini *h = input_get_drvdata(dev);

	hap_vibrate(h, 0);
}

static int hap_probe(struct platform_device *pdev)
{
	struct hap_mini *h;
	struct input_dev *input;
	u32 regs[3];
	int n, rc;

	h = devm_kzalloc(&pdev->dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = &pdev->dev;

	h->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!h->regmap) {
		dev_err(&pdev->dev, "failed to get parent PMIC regmap\n");
		return -ENXIO;
	}

	n = of_property_read_u32_array(pdev->dev.of_node, "reg", regs, 3);
	h->cfg_base = (n == 0) ? regs[0] : 0xf000;
	h->ptn_base = (n == 0) ? regs[1] : 0xf100;
	h->boost_base = (n == 0) ? regs[2] : 0xf200;

	if (of_property_read_u32(pdev->dev.of_node, "qcom,vmax-mv", &h->vmax_mv))
		h->vmax_mv = 1260;
	if (of_property_read_u32(pdev->dev.of_node, "qcom,lra-period-us",
				 &h->lra_period_us))
		h->lra_period_us = 5882;
	if (of_property_read_u32(pdev->dev.of_node, "qcom,drv-sig-shape",
				 &h->drv_wf))
		h->drv_wf = 1;
	if (of_property_read_u32(pdev->dev.of_node, "qcom,brake-mode",
				 &h->brake_mode))
		h->brake_mode = 0;

	mutex_init(&h->lock);
	platform_set_drvdata(pdev, h);

	rc = hap_init_hw(h);
	if (rc) {
		dev_err(&pdev->dev, "hardware init failed: %d\n", rc);
		return rc;
	}

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;
	input->name = "pm8550b-hv-haptics";
	input->id.bustype = BUS_HOST;
	h->input = input;
	input_set_drvdata(input, h);
	input_set_capability(input, EV_FF, FF_RUMBLE);

	rc = input_ff_create_memless(input, NULL, hap_ff_play);
	if (rc)
		return rc;

	input->close = hap_ff_close;

	rc = input_register_device(input);
	if (rc)
		return rc;

	dev_info(&pdev->dev, "PM8550B HV haptics ready (vmax=%umV, T_LRA=%uus)\n",
		 h->vmax_mv, h->lra_period_us);
	return 0;
}

static void hap_remove(struct platform_device *pdev)
{
	struct hap_mini *h = platform_get_drvdata(pdev);

	hap_vibrate(h, 0);
	hap_wr(h, h->cfg_base, HAP_CFG_EN_CTL, 0);
}

static const struct of_device_id hap_of_match[] = {
	{ .compatible = "qcom,hv-haptics" },
	{}
};
MODULE_DEVICE_TABLE(of, hap_of_match);

static struct platform_driver hap_driver = {
	.probe = hap_probe,
	.remove = hap_remove,
	.driver = {
		.name = "qcom-hv-haptics",
		.of_match_table = hap_of_match,
	},
};
module_platform_driver(hap_driver);

MODULE_DESCRIPTION("Qualcomm PM8550B HV haptics driver");
MODULE_LICENSE("GPL");
