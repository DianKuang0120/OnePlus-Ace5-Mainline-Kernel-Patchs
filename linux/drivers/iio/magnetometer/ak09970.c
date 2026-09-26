// SPDX-License-Identifier: GPL-2.0-only
/*
 * AK09970 3-axis magnetic sensor / hall-effect slider driver
 *
 * Register layout derived from the Oplus tri_state_key vendor driver.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>

#define AK09970_DRV_NAME	"ak09970"

/* Register map */
#define AK09970_REG_DID		0x00	/* 2 bytes, big-endian: 0x48 0xC1 */
#define AK09970_REG_DATA	0x17	/* burst: ST1, Z[15:0], Y[15:0], X[15:0] */
#define AK09970_REG_CNTL1	0x20
#define AK09970_REG_CNTL2	0x21
#define AK09970_REG_SRST	0x30

#define AK09970_DID		0x48C1

#define AK09970_ST1_DRDY	BIT(0)

#define AK09970_CNTL1_ALL_EN	0x7F
#define AK09970_MODE_POWER_DOWN	0x00
#define AK09970_MODE_CONT_10HZ	0x08	/* MODE4 */
#define AK09970_SRST_RESET	0x01

#define AK09970_MAX_REG		0x40

struct ak09970_data {
	struct regmap *regmap;
	struct device *dev;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	bool powered;
	struct mutex lock;	/* serialize reads */
};

static const struct regmap_config ak09970_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AK09970_MAX_REG,
};

static int ak09970_power_on(struct ak09970_data *data)
{
	int ret;

	if (data->powered)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(data->supplies), data->supplies);
	if (ret)
		return ret;

	if (data->reset_gpio) {
		gpiod_set_value_cansleep(data->reset_gpio, 1);
		msleep(20);
	}

	data->powered = true;
	return 0;
}

static void ak09970_power_off(struct ak09970_data *data)
{
	if (!data->powered)
		return;

	if (data->reset_gpio)
		gpiod_set_value_cansleep(data->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
	data->powered = false;
}

static int ak09970_init(struct ak09970_data *data)
{
	u8 did[2];
	u16 id;
	int ret;

	ret = regmap_bulk_read(data->regmap, AK09970_REG_DID, did, sizeof(did));
	if (ret)
		return ret;
	id = (did[0] << 8) | did[1];
	if (id != AK09970_DID)
		return dev_err_probe(data->dev, -ENODEV,
				     "bad device ID 0x%04x\n", id);

	ret = regmap_write(data->regmap, AK09970_REG_SRST, AK09970_SRST_RESET);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	ret = regmap_write(data->regmap, AK09970_REG_CNTL1, AK09970_CNTL1_ALL_EN);
	if (ret)
		return ret;

	return regmap_write(data->regmap, AK09970_REG_CNTL2, AK09970_MODE_CONT_10HZ);
}

static int ak09970_read_xyz(struct ak09970_data *data, s16 *x, s16 *y, s16 *z)
{
	u8 buf[7];
	int ret;

	ret = regmap_bulk_read(data->regmap, AK09970_REG_DATA, buf, sizeof(buf));
	if (ret)
		return ret;

	if (!(buf[0] & AK09970_ST1_DRDY))
		return -EAGAIN;

	*z = (s16)((buf[1] << 8) | buf[2]);
	*y = (s16)((buf[3] << 8) | buf[4]);
	*x = (s16)((buf[5] << 8) | buf[6]);
	return 0;
}

#define AK09970_CHANNEL(_axis, _addr) {			\
	.type = IIO_MAGN,				\
	.modified = 1,					\
	.channel2 = IIO_MOD_##_axis,			\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
	.address = _addr,				\
}

static const struct iio_chan_spec ak09970_channels[] = {
	AK09970_CHANNEL(X, 0),
	AK09970_CHANNEL(Y, 1),
	AK09970_CHANNEL(Z, 2),
};

static int ak09970_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ak09970_data *data = iio_priv(indio_dev);
	s16 x, y, z;
	int ret;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = pm_runtime_resume_and_get(data->dev);
	if (ret)
		goto out_unlock;

	ret = ak09970_read_xyz(data, &x, &y, &z);
	pm_runtime_put_autosuspend(data->dev);
	if (ret)
		goto out_unlock;

	switch (chan->address) {
	case 0: *val = x; break;
	case 1: *val = y; break;
	case 2: *val = z; break;
	default: ret = -EINVAL; goto out_unlock;
	}

	ret = IIO_VAL_INT;
out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static const struct iio_info ak09970_info = {
	.read_raw = ak09970_read_raw,
};

static int ak09970_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct ak09970_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->dev = &client->dev;
	mutex_init(&data->lock);

	data->regmap = devm_regmap_init_i2c(client, &ak09970_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(data->regmap),
				     "regmap init failed\n");

	data->supplies[0].supply = "vdd";
	data->supplies[1].supply = "vddio";
	ret = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(data->supplies),
				      data->supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret, "regulator get failed\n");

	data->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
						   GPIOD_OUT_LOW);
	if (IS_ERR(data->reset_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(data->reset_gpio),
				     "reset gpio get failed\n");

	ret = ak09970_power_on(data);
	if (ret)
		return dev_err_probe(&client->dev, ret, "power on failed\n");

	ret = ak09970_init(data);
	if (ret) {
		ak09970_power_off(data);
		return dev_err_probe(&client->dev, ret, "chip init failed\n");
	}

	indio_dev->name = AK09970_DRV_NAME;
	indio_dev->info = &ak09970_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ak09970_channels;
	indio_dev->num_channels = ARRAY_SIZE(ak09970_channels);

	pm_runtime_set_active(&client->dev);
	pm_runtime_set_autosuspend_delay(&client->dev, 1000);
	pm_runtime_use_autosuspend(&client->dev);
	pm_runtime_enable(&client->dev);

	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret) {
		pm_runtime_disable(&client->dev);
		ak09970_power_off(data);
		return dev_err_probe(&client->dev, ret, "iio register failed\n");
	}

	i2c_set_clientdata(client, indio_dev);
	return 0;
}

static void ak09970_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ak09970_data *data = iio_priv(indio_dev);

	pm_runtime_disable(&client->dev);
	ak09970_power_off(data);
}

static int ak09970_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ak09970_data *data = iio_priv(i2c_get_clientdata(client));

	regmap_write(data->regmap, AK09970_REG_CNTL2, AK09970_MODE_POWER_DOWN);
	ak09970_power_off(data);
	return 0;
}

static int ak09970_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ak09970_data *data = iio_priv(i2c_get_clientdata(client));
	int ret;

	ret = ak09970_power_on(data);
	if (ret)
		return ret;
	return ak09970_init(data);
}

static DEFINE_RUNTIME_DEV_PM_OPS(ak09970_pm_ops,
				 ak09970_runtime_suspend,
				 ak09970_runtime_resume, NULL);

static const struct of_device_id ak09970_of_match[] = {
	{ .compatible = "asahi-kasei,ak09970" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak09970_of_match);

static const struct i2c_device_id ak09970_i2c_id[] = {
	{ "ak09970" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ak09970_i2c_id);

static struct i2c_driver ak09970_driver = {
	.driver = {
		.name = AK09970_DRV_NAME,
		.of_match_table = ak09970_of_match,
		.pm = pm_ptr(&ak09970_pm_ops),
	},
	.probe = ak09970_probe,
	.remove = ak09970_remove,
	.id_table = ak09970_i2c_id,
};
module_i2c_driver(ak09970_driver);

MODULE_AUTHOR("DianKuang");
MODULE_DESCRIPTION("AK09970 3-axis magnetic sensor driver");
MODULE_LICENSE("GPL");