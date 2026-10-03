// SPDX-License-Identifier: GPL-2.0-only
/*
 * AK09970 alert slider driver (OnePlus Ace 5)
 *
 * 从 ak09970 IIO 驱动读取 X/Y/Z，判断滑块位置并以 input 上报。
 *
 * 采用轮询而非 IRQ：AK09970 的 INT 引脚需要额外配置芯片内部
 * 阈值寄存器才工作，成本高、收益低。滑块是低频事件，200ms 轮询足够。
 */

#include <linux/device.h>
#include <linux/input.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#include <linux/iio/consumer.h>

#define AK09970_SLIDER_POLL_MS		200
#define AK09970_SLIDER_INIT_MS		500
#define AK09970_SLIDER_CONFIRM		2

enum ak09970_pos {
	POS_UNKNOWN = -1,
	POS_UP = 0,
	POS_MID = 1,
	POS_DOWN = 2,
};

struct ak09970_slider {
	struct device *dev;
	struct iio_channel *chans[3];
	struct input_dev *input;
	struct delayed_work work;

	enum ak09970_pos last;
	enum ak09970_pos pending;
	int confirm;

	int up_thr;
	int mid_thr;
};

static enum ak09970_pos ak09970_classify(struct ak09970_slider *s,
					  int x, int y, int z)
{
	if (y > s->mid_thr)
		return POS_MID;
	if (x > s->up_thr)
		return POS_UP;
	if (x < -s->up_thr)
		return POS_DOWN;
	return s->last;
}

static int ak09970_read_xyz(struct ak09970_slider *s,
			    int *x, int *y, int *z)
{
	int ret;

	ret = iio_read_channel_raw(s->chans[0], x);
	if (ret < 0)
		return ret;
	ret = iio_read_channel_raw(s->chans[1], y);
	if (ret < 0)
		return ret;
	ret = iio_read_channel_raw(s->chans[2], z);
	if (ret < 0)
		return ret;
	return 0;
}

static void ak09970_report(struct ak09970_slider *s, enum ak09970_pos pos)
{
	input_report_abs(s->input, ABS_X, pos);
	input_sync(s->input);
}

static void ak09970_slider_work(struct work_struct *work)
{
	struct ak09970_slider *s =
		container_of(work, struct ak09970_slider, work.work);
	int x, y, z, ret;
	enum ak09970_pos pos;

	ret = ak09970_read_xyz(s, &x, &y, &z);
	if (ret) {
		dev_warn_ratelimited(s->dev, "read failed: %d\n", ret);
		goto resched;
	}

	pos = ak09970_classify(s, x, y, z);

	if (pos == s->last)
		goto resched;

	/* 连续 N 次读到同一位置才认，防止滑动中途误报 */
	if (pos != s->pending) {
		s->pending = pos;
		s->confirm = 1;
		goto resched;
	}
	if (++s->confirm < AK09970_SLIDER_CONFIRM)
		goto resched;

	s->last = pos;
	s->confirm = 0;
	dev_info(s->dev, "slider: %s (x=%d y=%d z=%d)\n",
		 pos == POS_UP ? "up" :
		 pos == POS_MID ? "mid" : "down", x, y, z);
	ak09970_report(s, pos);

resched:
	queue_delayed_work(system_dfl_wq, &s->work,
			   msecs_to_jiffies(AK09970_SLIDER_POLL_MS));
}

static int ak09970_slider_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "x", "y", "z" };
	struct ak09970_slider *s;
	u32 tmp;
	int ret, i;

	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->dev = &pdev->dev;
	s->last = POS_UNKNOWN;
	s->pending = POS_UNKNOWN;
	s->confirm = 0;
	INIT_DELAYED_WORK(&s->work, ak09970_slider_work);

	s->up_thr = 5000;
	s->mid_thr = 22000;
	if (!device_property_read_u32(&pdev->dev, "oneplus,up-threshold", &tmp))
		s->up_thr = (int)tmp;
	if (!device_property_read_u32(&pdev->dev, "oneplus,mid-threshold", &tmp))
		s->mid_thr = (int)tmp;

	for (i = 0; i < 3; i++) {
		s->chans[i] = devm_iio_channel_get(&pdev->dev, names[i]);
		if (IS_ERR(s->chans[i]))
			return dev_err_probe(&pdev->dev,
					     PTR_ERR(s->chans[i]),
					     "failed to get %s channel\n",
					     names[i]);
	}

	s->input = devm_input_allocate_device(&pdev->dev);
	if (!s->input)
		return -ENOMEM;

	s->input->name = "ak09970-slider";
	s->input->phys = "ak09970-slider/input0";
	s->input->id.bustype = BUS_I2C;
	s->input->dev.parent = &pdev->dev;

	input_set_abs_params(s->input, ABS_X, 0, 2, 0, 0);

	ret = input_register_device(s->input);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "input register failed\n");

	platform_set_drvdata(pdev, s);

	queue_delayed_work(system_dfl_wq, &s->work,
			   msecs_to_jiffies(AK09970_SLIDER_INIT_MS));

	return 0;
}

static void ak09970_slider_remove(struct platform_device *pdev)
{
	struct ak09970_slider *s = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&s->work);
}

static const struct of_device_id ak09970_slider_of_match[] = {
	{ .compatible = "oneplus,ak09970-slider" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak09970_slider_of_match);

static struct platform_driver ak09970_slider_driver = {
	.probe = ak09970_slider_probe,
	.remove = ak09970_slider_remove,
	.driver = {
		.name = "ak09970-slider",
		.of_match_table = ak09970_slider_of_match,
	},
};
module_platform_driver(ak09970_slider_driver);

MODULE_AUTHOR("DianKuang");
MODULE_DESCRIPTION("AK09970 alert slider driver (OnePlus Ace 5)");
MODULE_LICENSE("GPL");