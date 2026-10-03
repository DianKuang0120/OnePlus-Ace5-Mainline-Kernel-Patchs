// SPDX-License-Identifier: GPL-2.0-only
/*
 * SC8547 charge pump driver (mainline port)
 *
 * Copyright (C) 2020-2024 Opus. All rights reserved.
 *
 * Design:
 *   - Enable/disable is event driven: a power_supply notifier watches the
 *     PMIC-GLINK USB supply and kicks a state machine as soon as the PD/PPS
 *     source changes (no polling), so fast charging starts immediately.
 *   - The 1 s watchdog is left ENABLED (REG_09 = 0x13 / 0x93) and fed from a
 *     delayed work every 500 ms; a watchdog reset drops the CP to OFF.
 *   - The CP is only ever enabled with a valid high-voltage source present.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>

#include "sc8547.h"

#define SC8547_DRIVER_NAME		"sc8547"
#define SC8547_DEFAULT_OVP_REG		0x2E
#define SC8547_DEFAULT_OCP_REG		0x08

/* Auto enable/disable based on USB VBUS */
#define SC8547_AUTO_ENABLE_MV		8000
#define SC8547_AUTO_DISABLE_MV		6000
#define SC8547_AUTO_CHECK_INTERVAL_MS	1000

/* sc8547.h 里没有定义 0x0C，这里补上 */
#ifndef SC8547_REG_0C
#define SC8547_REG_0C			0x0C
#endif

/*
 * 1 s watchdog, fed from work context while the CP is enabled.
 * REG_09 bit[2:0] = 0b011.
 */
#define SC8547_WDT_ENABLE		1
#define SC8547_WDT_KICK_INTERVAL_MS	500	/* must be < 1000 */

struct sc8547 {
	struct device		*dev;
	struct i2c_client	*client;
	struct mutex		lock;

	bool			ic_sc8547a;
	bool			is_master;
	u8			ovp_reg;
	u8			ocp_reg;
	u32			pps_ocp_max;
	u32			auto_enable_mv;

	int			irq;
	struct work_struct	irq_work;
	u8			last_int_flag;

	bool			adc_enabled;

	struct delayed_work	kick_dog_work;

	/* event-driven enable/disable driven by the PMIC-GLINK USB supply */
	struct notifier_block	usb_nb;
	struct work_struct	state_work;
	bool			cp_on;

	struct power_supply	*psy;
};

static int sc8547_get_chg_enable(struct sc8547 *chip, bool *enabled);

/* ---------- I2C 基本读写 ---------- */

static int sc8547_read_byte(struct sc8547 *chip, u8 reg, u8 *val)
{
	int ret;

	mutex_lock(&chip->lock);
	ret = i2c_smbus_read_byte_data(chip->client, reg);
	mutex_unlock(&chip->lock);
	if (ret < 0) {
		dev_err(chip->dev, "read reg 0x%02x failed: %d\n", reg, ret);
		return ret;
	}

	*val = (u8)ret;
	return 0;
}

static int sc8547_write_byte(struct sc8547 *chip, u8 reg, u8 val)
{
	int ret;

	mutex_lock(&chip->lock);
	ret = i2c_smbus_write_byte_data(chip->client, reg, val);
	mutex_unlock(&chip->lock);
	if (ret < 0) {
		dev_err(chip->dev, "write reg 0x%02x=0x%02x failed: %d\n",
			reg, val, ret);
		return ret;
	}

	return 0;
}

static int sc8547_update_bits(struct sc8547 *chip, u8 reg, u8 mask, u8 val)
{
	int ret;
	u8 tmp;

	mutex_lock(&chip->lock);
	ret = i2c_smbus_read_byte_data(chip->client, reg);
	if (ret < 0) {
		dev_err(chip->dev, "read reg 0x%02x failed: %d\n", reg, ret);
		goto out;
	}
	tmp = (u8)ret;
	tmp &= ~mask;
	tmp |= val & mask;

	ret = i2c_smbus_write_byte_data(chip->client, reg, tmp);
	if (ret < 0)
		dev_err(chip->dev, "write reg 0x%02x=0x%02x failed: %d\n",
			reg, tmp, ret);
out:
	mutex_unlock(&chip->lock);
	return ret;
}

static int sc8547_read_block(struct sc8547 *chip, u8 reg, u8 *buf, int len)
{
	int ret;

	mutex_lock(&chip->lock);
	ret = i2c_smbus_read_i2c_block_data(chip->client, reg, len, buf);
	mutex_unlock(&chip->lock);
	if (ret < 0) {
		dev_err(chip->dev, "read block reg 0x%02x failed: %d\n",
			reg, ret);
		return ret;
	}
	if (ret != len) {
		dev_err(chip->dev, "short read reg 0x%02x: got %d, want %d\n",
			reg, ret, len);
		return -EIO;
	}

	return 0;
}

/* ---------- 版本与初始化 ---------- */

static int sc8547_hw_version_check(struct sc8547 *chip)
{
	u8 val;
	int ret;

	ret = sc8547_read_byte(chip, SC8547_REG_36, &val);
	if (ret)
		return ret;

	chip->ic_sc8547a = (val == SC8547A_DEVICE_ID);
	dev_info(chip->dev, "chip revision: %s (reg36=0x%02x)\n",
		 chip->ic_sc8547a ? "SC8547A" : "SC8547", val);
	return 0;
}

static int sc8547_reg_reset(struct sc8547 *chip)
{
	return sc8547_update_bits(chip, SC8547_REG_07,
				  SC8547_REG_RESET_MASK,
				  SC8547_RESET_REG << SC8547_REG_RESET_SHIFT);
}

static int sc8547_set_adc_enable(struct sc8547 *chip, bool enable)
{
	int ret;

	ret = sc8547_write_byte(chip, SC8547_REG_11,
				enable ? 0x80 : 0x00);
	if (!ret)
		chip->adc_enabled = enable;

	return ret;
}

static int sc8547_init_device(struct sc8547 *chip)
{
	u8 reg_data;

	sc8547_write_byte(chip, SC8547_REG_11, 0x00);	/* ADC off */
	sc8547_write_byte(chip, SC8547_REG_02, 0x01);	/* VAC_OVP 12V */
	sc8547_write_byte(chip, SC8547_REG_04, 0x78);	/* VBUS_OVP 12V, 兼容 11V PPS */

	reg_data = 0x20 | (chip->ovp_reg & SC8547_BAT_OVP_MASK);
	sc8547_write_byte(chip, SC8547_REG_00, reg_data);

	reg_data = 0x20 | (chip->ocp_reg & SC8547_IBUS_OCP_MASK);
	sc8547_write_byte(chip, SC8547_REG_05, reg_data);

	sc8547_write_byte(chip, SC8547_REG_01, 0xbf);
	sc8547_write_byte(chip, SC8547_REG_2B, 0x00);	/* VOOC off */
	sc8547_write_byte(chip, SC8547_REG_3A, 0x60);
	sc8547_write_byte(chip, SC8547_REG_10, 0x02);	/* mask insert irq */

	sc8547_update_bits(chip, SC8547_REG_09,
			   SC8547_IBUS_UCP_RISE_MASK_MASK,
			   SC8547_IBUS_UCP_RISE_MASK << SC8547_IBUS_UCP_RISE_MASK_SHIFT);
	return 0;
}

/* ---------- 看门狗喂狗 ---------- */

/*
 * 读 REG_36 会刷新看门狗计数器。
 * 只在 CHG_EN 打开时才会周期性调度；CHG_EN 关闭时自动停止。
 */
static void sc8547_kick_dog_work(struct work_struct *work)
{
	struct sc8547 *chip = container_of(to_delayed_work(work),
					   struct sc8547, kick_dog_work);
	bool enabled;
	u8 tmp;

	if (sc8547_get_chg_enable(chip, &enabled) < 0 || !enabled)
		return;

	sc8547_read_byte(chip, SC8547_REG_36, &tmp);
	schedule_delayed_work(&chip->kick_dog_work,
			      msecs_to_jiffies(SC8547_WDT_KICK_INTERVAL_MS));
}

/* ---------- CP 开关 ---------- */

static int sc8547_get_chg_enable(struct sc8547 *chip, bool *enabled)
{
	u8 val;
	int ret;

	ret = sc8547_read_byte(chip, SC8547_REG_07, &val);
	if (ret)
		return ret;

	*enabled = !!(val & SC8547_CHG_EN_MASK);
	return 0;
}

static int sc8547_set_chg_enable(struct sc8547 *chip, bool enable)
{
	u8 val, check = 0;
	int ret;

	/*
	 * 打开 CP 前强制 VAC_OK，避免 VAC 检测抖动导致 CHG_EN 一起被清。
	 * vendor 从 CP 的 sc8547_slave_cp_enable 里也有这一步（REG_0C = 0x02）。
	 * 只在使能时做，关闭时不要碰。
	 */
	if (enable) {
		sc8547_write_byte(chip, SC8547_REG_0C, 0x02);
		msleep(50);
	}

	if (chip->is_master) {
		val = enable ? 0x84 : 0x04;
	} else {
		if (enable)
			val = chip->ic_sc8547a ? 0x81 : 0x85;
		else
			val = chip->ic_sc8547a ? 0x01 : 0x05;
	}

	ret = sc8547_write_byte(chip, SC8547_REG_07, val);
	if (ret)
		return ret;

	/* 修复: readback 的返回值单独存，不覆盖 write 的 ret */
	{
		int rb = sc8547_read_byte(chip, SC8547_REG_07, &check);
		if (!rb)
			dev_info(chip->dev,
				 "%s CP (%s): write 0x%02x, readback 0x%02x\n",
				 enable ? "enable" : "disable",
				 chip->is_master ? "master" : "slave",
				 val, check);
	}

	if (enable) {
#if SC8547_WDT_ENABLE
		schedule_delayed_work(&chip->kick_dog_work,
				      msecs_to_jiffies(SC8547_WDT_KICK_INTERVAL_MS));
#endif
		/* 使能后延时，读一下状态寄存器，打印 CP_SWITCHING_STAT */
		msleep(100);
		sc8547_read_byte(chip, SC8547_REG_06, &check);
		dev_info(chip->dev,
			 "CP status reg06 = 0x%02x (switching=%d)\n",
			 check, !!(check & SC8547_CP_SWITCHING_STAT_MASK));
	} else {
#if SC8547_WDT_ENABLE
		cancel_delayed_work_sync(&chip->kick_dog_work);
#endif
	}

	/* 修复: write 成功就算成功，不再把 readback 的结果当返回值 */
	return 0;
}

/* ---------- 工作模式 ---------- */

static int sc8547_config_sc_mode(struct sc8547 *chip)
{
	u8 reg_data;
	int ret;

	/* 修复: 所有写操作检查返回值，I2C 失败立即返回 */
#define SC8547_WR(r, v) do {                 \
	ret = sc8547_write_byte(chip, r, v); \
	if (ret)                             \
		return ret;                  \
} while (0)

	/* 参考 vendor sc8547_svooc_hw_setting */
	SC8547_WR(SC8547_REG_02, 0x01);	/* VAC_OVP 12V */
	SC8547_WR(SC8547_REG_04, 0x78);	/* VBUS_OVP 12V */

	reg_data = 0x20 | (chip->ocp_reg & SC8547_IBUS_OCP_MASK);
	SC8547_WR(SC8547_REG_05, reg_data);

	/*
	 * WATCHDOG_1S + IBUS_UCP_RISE_MASK.  Fed by kick_dog_work while the
	 * CP is enabled; a missed feed turns CHG_EN off (fail safe).
	 */
	SC8547_WR(SC8547_REG_09, 0x13);

	SC8547_WR(SC8547_REG_11, 0x80);	/* ADC on */
	SC8547_WR(SC8547_REG_0D, 0x70);	/* PMID2OUT */
	SC8547_WR(SC8547_REG_33, 0xd1);	/* LOOSE_DET */
	SC8547_WR(SC8547_REG_3A, 0x60);
	SC8547_WR(SC8547_REG_2B, 0x00);	/* VOOC off */

#undef SC8547_WR

	chip->adc_enabled = true;
	dev_info(chip->dev, "configured SC mode (WDT on), ocp_reg=0x%02x\n",
		 reg_data);
	return 0;
}

static int sc8547_config_bypass_mode(struct sc8547 *chip)
{
	int ret;

	/* 修复: 所有写操作检查返回值，I2C 失败立即返回 */
#define SC8547_WR(r, v) do {                 \
	ret = sc8547_write_byte(chip, r, v); \
	if (ret)                             \
		return ret;                  \
} while (0)

	SC8547_WR(SC8547_REG_02, 0x07);	/* VAC_OVP 6.5V */
	SC8547_WR(SC8547_REG_04, 0x0A);	/* VBUS_OVP 6.5V */
	SC8547_WR(SC8547_REG_05, 0x2a);	/* IBUS OCP */

	/* WATCHDOG_1S | IBUS_UCP_RISE_MASK | CHARGE_MODE_1_1 */
	SC8547_WR(SC8547_REG_09, 0x93);
	SC8547_WR(SC8547_REG_11, 0x80);	/* ADC on */
	SC8547_WR(SC8547_REG_2B, 0x00);
	SC8547_WR(SC8547_REG_3C, 0x40);

#undef SC8547_WR

	chip->adc_enabled = true;
	dev_info(chip->dev, "configured bypass mode (WDT on)\n");
	return 0;
}

/*
 * Event-driven CP enable/disable.
 *
 * The USB power_supply notifier schedules this as soon as the PMIC-GLINK
 * USB supply reports a change, so a PPS/PD source is followed within
 * milliseconds instead of on a 1 Hz tick.
 */
static void sc8547_state_work(struct work_struct *work)
{
	struct sc8547 *chip = container_of(work, struct sc8547, state_work);
	union power_supply_propval val = { 0 };
	struct power_supply *psy;
	bool online = false, cp_en, want;
	int usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	int vbus_uv = 0;

	psy = power_supply_get_by_name("qcom-battmgr-usb");
	if (psy) {
		if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &val))
			online = val.intval;
		if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_USB_TYPE, &val))
			usb_type = val.intval;
		if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &val))
			vbus_uv = val.intval;
		power_supply_put(psy);
	}

	/*
	 * SC (charge-pump) mode needs a ~2*Vbat PPS source; a fixed
	 * high-voltage source (VOOC/SVOOC, HVDCP) uses bypass.  Both show up
	 * as VBUS >= 6.5 V, so only ever enable with a valid high-V source.
	 */
	want = online &&
	       (usb_type == POWER_SUPPLY_USB_TYPE_PD_PPS ||
		vbus_uv >= chip->auto_enable_mv * 1000);

	if (sc8547_get_chg_enable(chip, &cp_en))
		return;

	if (want && !cp_en) {
		dev_info(chip->dev, "VBUS %dmV type %d -> enabling CP\n",
			 vbus_uv / 1000, usb_type);
		if (sc8547_config_sc_mode(chip) == 0)
			sc8547_set_chg_enable(chip, true);
	} else if (!want && cp_en) {
		dev_info(chip->dev, "source gone -> disabling CP\n");
		sc8547_set_chg_enable(chip, false);
	}
}

static int sc8547_usb_notify(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	struct sc8547 *chip = container_of(nb, struct sc8547, usb_nb);
	struct power_supply *psy = data;

	if (psy && psy->desc && !strcmp(psy->desc->name, "qcom-battmgr-usb"))
		schedule_work(&chip->state_work);

	return NOTIFY_OK;
}

/* ---------- ADC 读取 ---------- */

static int sc8547_get_cp_vbus(struct sc8547 *chip, int *mv)
{
	u8 buf[2];
	int ret;

	ret = sc8547_read_block(chip, SC8547_REG_15, buf, 2);
	if (ret)
		return ret;

	*mv = (((buf[0] & SC8547_VBUS_POL_H_MASK) << 8) | buf[1]) *
	      SC8547_VBUS_ADC_LSB;
	return 0;
}

static int sc8547_get_cp_ichg(struct sc8547 *chip, int *ma)
{
	u8 buf[2];
	int ret;
	bool enabled;

	ret = sc8547_get_chg_enable(chip, &enabled);
	if (ret)
		return ret;
	if (!enabled) {
		*ma = 0;
		return 0;
	}

	ret = sc8547_read_block(chip, SC8547_REG_13, buf, 2);
	if (ret)
		return ret;

	*ma = (((buf[0] & SC8547_IBUS_POL_H_MASK) << 8) | buf[1]) * 1875 / 1000;
	return 0;
}

static int sc8547_get_cp_vac(struct sc8547 *chip, int *mv)
{
	u8 buf[2];
	int ret;

	ret = sc8547_read_block(chip, SC8547_REG_17, buf, 2);
	if (ret)
		return ret;

	*mv = (((buf[0] & SC8547_VAC_POL_H_MASK) << 8) | buf[1]) *
	      SC8547_VAC_ADC_LSB;
	return 0;
}

static int sc8547_get_cp_vout(struct sc8547 *chip, int *mv)
{
	u8 buf[2];
	int ret;

	ret = sc8547_read_block(chip, SC8547_REG_19, buf, 2);
	if (ret)
		return ret;

	*mv = (((buf[0] & SC8547_VOUT_POL_H_MASK) << 8) | buf[1]) *
	      SC8547_VOUT_ADC_LSB;
	return 0;
}

static int sc8547_get_cp_vbat(struct sc8547 *chip, int *mv)
{
	u8 buf[2];
	int ret;

	ret = sc8547_read_block(chip, SC8547_REG_1B, buf, 2);
	if (ret)
		return ret;

	*mv = (((buf[0] & SC8547_VBAT_POL_H_MASK) << 8) | buf[1]) *
	      SC8547_VBAT_ADC_LSB;
	return 0;
}

static int sc8547_get_cp_tdie(struct sc8547 *chip, int *deci_c)
{
	u8 buf[2];
	int ret;

	ret = sc8547_read_block(chip, SC8547_REG_1F, buf, 2);
	if (ret)
		return ret;

	*deci_c = (((buf[0] & SC8547_TDIE_POL_H_MASK) << 8) |
		   (buf[1] & SC8547_TDIE_POL_L_MASK)) * SC8547_TDIE_ADC_LSB;
	if (*deci_c < SC8547_TDIE_MIN || *deci_c > SC8547_TDIE_MAX)
		*deci_c = SC8547_TDIE_MAX;
	return 0;
}

/* ---------- 中断 ---------- */

static void sc8547_irq_work(struct work_struct *work)
{
	struct sc8547 *chip = container_of(work, struct sc8547, irq_work);
	u8 flag = 0;
	int ret;

	ret = sc8547_read_byte(chip, SC8547_REG_0F, &flag);
	if (ret) {
		dev_err(chip->dev, "read int flag failed: %d\n", ret);
		return;
	}

	chip->last_int_flag = flag;
	if (flag)
		dev_info(chip->dev, "interrupt: reg0F=0x%02x\n", flag);

	/* 顺便喂一次狗（如果看门狗开启过） */
	sc8547_read_byte(chip, SC8547_REG_36, &flag);
}

static irqreturn_t sc8547_irq_handler(int irq, void *data)
{
	struct sc8547 *chip = data;

	schedule_work(&chip->irq_work);
	return IRQ_HANDLED;
}

/* ---------- sysfs ---------- */

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct sc8547 *chip = dev_get_drvdata(dev);
	bool enabled;
	int ret;

	ret = sc8547_get_chg_enable(chip, &enabled);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", enabled);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct sc8547 *chip = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	ret = sc8547_set_chg_enable(chip, enable);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(enable);

#define SC8547_ADC_ATTR(_name, _func)					\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct sc8547 *chip = dev_get_drvdata(dev);			\
	int val;							\
	int ret;							\
									\
	ret = _func(chip, &val);					\
	if (ret)							\
		return ret;						\
									\
	return sysfs_emit(buf, "%d\n", val);				\
}									\
static DEVICE_ATTR_RO(_name)

SC8547_ADC_ATTR(vbus, sc8547_get_cp_vbus);
SC8547_ADC_ATTR(ibus, sc8547_get_cp_ichg);
SC8547_ADC_ATTR(vac,  sc8547_get_cp_vac);
SC8547_ADC_ATTR(vout, sc8547_get_cp_vout);
SC8547_ADC_ATTR(vbat, sc8547_get_cp_vbat);
SC8547_ADC_ATTR(tdie, sc8547_get_cp_tdie);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct sc8547 *chip = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = sc8547_read_byte(chip, SC8547_REG_09, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n",
			  (val & SC8547_CHARGE_MODE_MASK) ? "bypass" : "sc");
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct sc8547 *chip = dev_get_drvdata(dev);
	int ret;

	if (sysfs_streq(buf, "sc"))
		ret = sc8547_config_sc_mode(chip);
	else if (sysfs_streq(buf, "bypass"))
		ret = sc8547_config_bypass_mode(chip);
	else
		return -EINVAL;

	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(mode);

static ssize_t int_flag_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct sc8547 *chip = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%02x\n", chip->last_int_flag);
}
static DEVICE_ATTR_RO(int_flag);

static ssize_t reg_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct sc8547 *chip = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = sc8547_read_byte(chip, SC8547_REG_36, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "chip_id=0x%02x\n", val);
}
static DEVICE_ATTR_RO(reg);

static ssize_t dump_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct sc8547 *chip = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i <= 0x3F; i++) {
		u8 val;

		if (sc8547_read_byte(chip, i, &val))
			continue;
		len += sysfs_emit_at(buf, len, "reg%02x=0x%02x\n", i, val);
	}
	return len;
}
static DEVICE_ATTR_RO(dump);

static ssize_t role_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct sc8547 *chip = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", chip->is_master ? "master" : "slave");
}
static DEVICE_ATTR_RO(role);

static struct attribute *sc8547_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_vbus.attr,
	&dev_attr_ibus.attr,
	&dev_attr_vac.attr,
	&dev_attr_vout.attr,
	&dev_attr_vbat.attr,
	&dev_attr_tdie.attr,
	&dev_attr_mode.attr,
	&dev_attr_int_flag.attr,
	&dev_attr_reg.attr,
	&dev_attr_dump.attr,
	&dev_attr_role.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sc8547);

/* ---------- power_supply ---------- */

static int sc8547_psy_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct sc8547 *chip = power_supply_get_drvdata(psy);
	bool en = false;
	int tmp;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if (sc8547_get_chg_enable(chip, &en))
			en = false;
		val->intval = en;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (sc8547_get_chg_enable(chip, &en))
			en = false;
		val->intval = en ? POWER_SUPPLY_STATUS_CHARGING :
				   POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (sc8547_get_cp_vout(chip, &tmp))
			return -EAGAIN;
		val->intval = tmp * 1000;	/* mV -> uV */
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (sc8547_get_cp_ichg(chip, &tmp))
			return -EAGAIN;
		val->intval = tmp * 1000;	/* mA -> uA */
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = chip->pps_ocp_max * 1000;	/* mA -> uA */
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sc8547_psy_set_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	struct sc8547 *chip = power_supply_get_drvdata(psy);
	u8 ocp;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		/* uA -> mA, clamped to the IBUS OCP field range */
		ocp = clamp_val(val->intval / 1000, 0, SC8547_IBUS_OCP_MASK);
		chip->ocp_reg = ocp;
		return sc8547_write_byte(chip, SC8547_REG_05,
					 0x20 | (ocp & SC8547_IBUS_OCP_MASK));
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property sc8547_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
};

static const struct power_supply_desc sc8547_psy_desc = {
	.name		= "sc8547",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= sc8547_psy_props,
	.num_properties	= ARRAY_SIZE(sc8547_psy_props),
	.get_property	= sc8547_psy_get_property,
	.set_property	= sc8547_psy_set_property,
};

/* ---------- DT 解析 ---------- */

static int sc8547_parse_dt(struct sc8547 *chip)
{
	struct device_node *np = chip->dev->of_node;
	u32 val;
	int ret;

	ret = of_property_read_u32(np, "ovp_reg", &val);
	if (ret)
		chip->ovp_reg = SC8547_DEFAULT_OVP_REG;
	else
		chip->ovp_reg = (u8)val;

	ret = of_property_read_u32(np, "ocp_reg", &val);
	if (ret)
		chip->ocp_reg = SC8547_DEFAULT_OCP_REG;
	else
		chip->ocp_reg = (u8)val;

	ret = of_property_read_u32(np, "oplus,pps_ocp_max", &chip->pps_ocp_max);
	if (ret)
		chip->pps_ocp_max = 3600;

	ret = of_property_read_u32(np, "southchip,auto-enable-mv",
				   &chip->auto_enable_mv);
	if (ret)
		chip->auto_enable_mv = SC8547_AUTO_ENABLE_MV;

	chip->is_master = of_property_read_bool(np, "southchip,master");

	dev_info(chip->dev, "role: %s, ovp_reg=0x%02x ocp_reg=0x%02x pps_ocp_max=%u\n",
		 chip->is_master ? "master" : "slave",
		 chip->ovp_reg, chip->ocp_reg, chip->pps_ocp_max);
	return 0;
}

/* ---------- probe / remove ---------- */

static int sc8547_probe(struct i2c_client *client)
{
	struct sc8547 *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;
	chip->client = client;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	ret = i2c_smbus_read_byte_data(client, SC8547_REG_07);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "I2C communication failed\n");

	ret = sc8547_parse_dt(chip);
	if (ret)
		return ret;

	ret = sc8547_hw_version_check(chip);
	if (ret)
		return ret;

	ret = sc8547_reg_reset(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret, "reg reset failed\n");

	ret = sc8547_init_device(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret, "init device failed\n");

	INIT_WORK(&chip->irq_work, sc8547_irq_work);
	INIT_DELAYED_WORK(&chip->kick_dog_work, sc8547_kick_dog_work);
	INIT_WORK(&chip->state_work, sc8547_state_work);

	chip->usb_nb.notifier_call = sc8547_usb_notify;
	power_supply_reg_notifier(&chip->usb_nb);
	/* evaluate the current source immediately */
	schedule_work(&chip->state_work);

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, sc8547_irq_handler,
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						SC8547_DRIVER_NAME, chip);
		if (ret)
			dev_warn(&client->dev, "request irq %d failed: %d\n",
				 client->irq, ret);
		else
			chip->irq = client->irq;
	}

	{
		struct power_supply_config psy_cfg = { .drv_data = chip };

		chip->psy = devm_power_supply_register(&client->dev,
						       &sc8547_psy_desc,
						       &psy_cfg);
		if (IS_ERR(chip->psy))
			return dev_err_probe(&client->dev, PTR_ERR(chip->psy),
					     "failed to register power supply\n");
	}

	dev_info(&client->dev, "probe done (%s, %s)\n",
		 chip->is_master ? "master" : "slave",
		 chip->ic_sc8547a ? "SC8547A" : "SC8547");
	return 0;
}

static void sc8547_remove(struct i2c_client *client)
{
	struct sc8547 *chip = i2c_get_clientdata(client);

	power_supply_unreg_notifier(&chip->usb_nb);
	cancel_work_sync(&chip->irq_work);
	cancel_work_sync(&chip->state_work);
	cancel_delayed_work_sync(&chip->kick_dog_work);
	sc8547_set_chg_enable(chip, false);
	sc8547_set_adc_enable(chip, false);
	dev_info(chip->dev, "removed\n");
}

static void sc8547_shutdown(struct i2c_client *client)
{
	struct sc8547 *chip = i2c_get_clientdata(client);

	power_supply_unreg_notifier(&chip->usb_nb);
	cancel_work_sync(&chip->state_work);
	cancel_delayed_work_sync(&chip->kick_dog_work);
	sc8547_set_chg_enable(chip, false);
	sc8547_set_adc_enable(chip, false);
}

static const struct of_device_id sc8547_of_match[] = {
	{ .compatible = "southchip,sc8547" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sc8547_of_match);

static const struct i2c_device_id sc8547_id_table[] = {
	{ "sc8547", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, sc8547_id_table);

static struct i2c_driver sc8547_driver = {
	.driver = {
		.name		= SC8547_DRIVER_NAME,
		.of_match_table	= sc8547_of_match,
		.dev_groups	= sc8547_groups,
	},
	.probe		= sc8547_probe,
	.remove		= sc8547_remove,
	.shutdown	= sc8547_shutdown,
	.id_table	= sc8547_id_table,
};
module_i2c_driver(sc8547_driver);

MODULE_DESCRIPTION("SC8547 charge pump driver");
MODULE_LICENSE("GPL v2");