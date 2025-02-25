/*
 * rtc-isl12058 - Driver for Intersil ISL12058 I2C Real Time Clock
 *
 * Copyright (C) 2013, Arnaud EBALARD <arno@natisbad.org>
 *
 * This work is largely based on Intersil ISL1208 driver developed by
 * Hebert Valerio Riedel <hvr@gnu.org>.
 *
 * Detailed datasheet on which this development is based is available here:
 *
 *  http://natisbad.org/NAS2/refs/ISL12058.pdf
 *
 * Adapted to ISL12058 by Remi Peuvergne.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rtc.h>
#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>

#define DRV_NAME "rtc-isl12058"

/* RTC section */
#define ISL12058_REG_RTC_SC	0x00	/* Seconds */
#define ISL12058_REG_RTC_MN	0x01	/* Minutes */
#define ISL12058_REG_RTC_HR	0x02	/* Hours */
#define ISL12058_REG_RTC_HR_PM	BIT(5)	/* AM/PM bit in 12h format */
#define ISL12058_REG_RTC_HR_MIL BIT(7)	/* 24h/12h format */
#define ISL12058_REG_RTC_DW	0x06	/* Day of the Week */
#define ISL12058_REG_RTC_DT	0x03	/* Date */
#define ISL12058_REG_RTC_MO	0x04	/* Month */
#define ISL12058_REG_RTC_YR	0x05	/* Year */
#define ISL12058_RTC_SEC_LEN	7

/* Alarm 1 section */
#define ISL12058_REG_A1_SC	0x0C	/* Alarm 1 Seconds */
#define ISL12058_REG_A1_MN	0x0D	/* Alarm 1 Minutes */
#define ISL12058_REG_A1_HR	0x0E	/* Alarm 1 Hours */
#define ISL12058_REG_A1_HR_MIL	BIT(6)	/* 24h/12h format */
#define ISL12058_REG_A1_DWDT	0x11	/* Alarm 1 Date / Day of the week */
#define ISL12058_REG_A1_DWDT_B	BIT(6)	/* DW / DT selection bit */
#define ISL12058_A1_SEC_LEN	6

/* Alarm 2 section */
#define ISL12058_REG_A2_MN	0x12	/* Alarm 2 Minutes */
#define ISL12058_REG_A2_HR	0x13	/* Alarm 2 Hours */
#define ISL12058_REG_A2_DWDT	0x14	/* Alarm 2 Date / Day of the week */
#define ISL12058_A2_SEC_LEN	3

/* Control/Status registers */
#define ISL12058_REG_INT	0x08
#define ISL12058_REG_INT_A1IE	BIT(6)	/* Alarm 1 interrupt enable bit */
#define ISL12058_REG_INT_A2IE	BIT(5)	/* Alarm 2 interrupt enable bit */
#define ISL12058_REG_INT_EOSC	BIT(7)	/* Oscillator enable bit */

#define ISL12058_REG_SR	0x07
#define ISL12058_REG_SR_A1F	BIT(2)	/* Alarm 1 interrupt bit */
#define ISL12058_REG_SR_A2F	BIT(1)	/* Alarm 2 interrupt bit */
#define ISL12058_REG_SR_OSF	BIT(3)	/* Oscillator failure bit */
#define ISL12058_REG_SR_WRTC	BIT(4)  /* Write Enable */

/* Register memory map length */
#define ISL12058_MEM_MAP_LEN	0x15

struct isl12058_rtc_data {
	struct rtc_device *rtc;
	struct regmap *regmap;
	struct mutex lock;
	int irq;
};


static ssize_t
isl12058_sysfs_show_osf(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev->parent);
	unsigned int sr;
	int ret;
	//dev_err(dev, "%s: data=%x\n", __func__, data);
	//dev_err(dev, "%s: regmap=%x\n", __func__, data->regmap);

	ret = regmap_read(data->regmap, ISL12058_REG_SR, &sr);
	if (ret) {
		dev_err(dev, "%s: unable to read oscillator status flag (%d)\n",
			__func__, ret);
		return ret;
	} else {
		int osf = (sr & ISL12058_REG_SR_OSF) ? 1 : 0;
		return sprintf(buf, "%d\n", osf);
	}
}

static ssize_t
isl12058_sysfs_store_osf(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev->parent);
	int ret;

	/* Writing (just anything) to the node resets flags */
	ret = regmap_update_bits(data->regmap, ISL12058_REG_SR,
				 ISL12058_REG_SR_OSF, 0);
	if (ret < 0) {
		dev_err(dev, "%s: unable to clear osc. failure bit (%d)\n",
			__func__, ret);
		return ret;
	}

	return count;
}

static DEVICE_ATTR(osf, S_IRUGO | S_IWUSR,
		   isl12058_sysfs_show_osf,
		   isl12058_sysfs_store_osf);

static struct attribute *isl12058_rtc_attrs[] = {
	&dev_attr_osf.attr,
	NULL
};

static const struct attribute_group isl12058_rtc_sysfs_files = {
	.attrs	= isl12058_rtc_attrs,
};

static void isl12058_rtc_regs_to_tm(struct rtc_time *tm, u8 *regs)
{
	tm->tm_sec = bcd2bin(regs[ISL12058_REG_RTC_SC]);
	tm->tm_min = bcd2bin(regs[ISL12058_REG_RTC_MN]);

	if (regs[ISL12058_REG_RTC_HR] & (!ISL12058_REG_RTC_HR_MIL)) { /* AM/PM */
		tm->tm_hour = bcd2bin(regs[ISL12058_REG_RTC_HR] & 0x1f);
		if (regs[ISL12058_REG_RTC_HR] & ISL12058_REG_RTC_HR_PM)
			tm->tm_hour += 12;
	} else {					    /* 24 hour mode */
		tm->tm_hour = bcd2bin(regs[ISL12058_REG_RTC_HR] & 0x3f);
	}

	tm->tm_mday = bcd2bin(regs[ISL12058_REG_RTC_DT]);
	tm->tm_wday = bcd2bin(regs[ISL12058_REG_RTC_DW]) - 1; /* starts at 1 */
	tm->tm_mon  = bcd2bin(regs[ISL12058_REG_RTC_MO] & 0x1f) - 1; /* ditto */
	tm->tm_year = bcd2bin(regs[ISL12058_REG_RTC_YR]) + 100;
}

static int isl12058_rtc_tm_to_regs(u8 *regs, struct rtc_time *tm)
{
	/*
	 * The clock has an 8 bit wide bcd-coded register for the year.
	 * It also has a century bit encoded in MO flag which provides
	 * information about overflow of year register from 99 to 00.
	 * tm_year is an offset from 1900 and we are interested in the
	 * 2000-2199 range, so any value less than 100 or larger than
	 * 299 is invalid.
	 */
	if (tm->tm_year < 100 || tm->tm_year > 299)
		return -EINVAL;

	regs[ISL12058_REG_RTC_SC] = bin2bcd(tm->tm_sec);
	regs[ISL12058_REG_RTC_MN] = bin2bcd(tm->tm_min);
	regs[ISL12058_REG_RTC_HR] = bin2bcd(tm->tm_hour) | ISL12058_REG_RTC_HR_MIL; /* 24-hour format */
	regs[ISL12058_REG_RTC_DT] = bin2bcd(tm->tm_mday);
	regs[ISL12058_REG_RTC_MO] = bin2bcd(tm->tm_mon + 1);
	regs[ISL12058_REG_RTC_YR] = bin2bcd(tm->tm_year % 100);
	regs[ISL12058_REG_RTC_DW] = bin2bcd(tm->tm_wday + 1);

	return 0;
}

/*
 * Try and match register bits w/ fixed null values to see whether we
 * are dealing with an ISL12058. Note: this function is called early
 * during init and hence does need mutex protection.
 */
static int isl12058_i2c_validate_chip(struct regmap *regmap)
{
	u8 regs[ISL12058_MEM_MAP_LEN];
	static const u8 mask[ISL12058_MEM_MAP_LEN] = { 0x80, 0x80, 0x40, 0xc0,
						       0xe0, 0x00, 0xf8, 0x20,
						       0x82, 0xff, 0xff, 0xff,
						       0x00, 0x00, 0x00, 0x40,
						       0x60, 0x78, 0x00, 0x00,
						       0x00 };
	int ret, i;

	ret = regmap_bulk_read(regmap, 0, regs, ISL12058_MEM_MAP_LEN);
	if (ret)
		return ret;

	for (i = 0; i < ISL12058_MEM_MAP_LEN; ++i) {
		if (regs[i] & mask[i])	/* check if bits are cleared */
			return -ENODEV;
	}

	return 0;
}

static int _isl12058_rtc_clear_alarm(struct device *dev)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);
	int ret;

	ret = regmap_update_bits(data->regmap, ISL12058_REG_SR,
				 ISL12058_REG_SR_A1F, 0);
	if (ret)
		dev_err(dev, "%s: clearing alarm failed (%d)\n", __func__, ret);

	return ret;
}

static int _isl12058_rtc_update_alarm(struct device *dev, int enable)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);
	int ret;

	ret = regmap_update_bits(data->regmap, ISL12058_REG_INT,
				 ISL12058_REG_INT_A1IE,
				 enable ? ISL12058_REG_INT_A1IE : 0);
	if (ret)
		dev_err(dev, "%s: changing alarm interrupt flag failed (%d)\n",
			__func__, ret);

	return ret;
}

/*
 * Note: as we only read from device and do not perform any update, there is
 * no need for an equivalent function which would try and get driver's main
 * lock. Here, it is safe for everyone if we just use regmap internal lock
 * on the device when reading.
 */
static int _isl12058_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);
	u8 regs[ISL12058_RTC_SEC_LEN];
	unsigned int sr;
	int ret;

	ret = regmap_read(data->regmap, ISL12058_REG_SR, &sr);
	if (ret) {
		dev_err(dev, "%s: unable to read oscillator status flag (%d)\n",
			__func__, ret);
		goto out;
	} else {
		if (sr & ISL12058_REG_SR_OSF) {
			ret = -ENODATA;
			goto out;
		}
	}

	ret = regmap_bulk_read(data->regmap, ISL12058_REG_RTC_SC, regs,
			       ISL12058_RTC_SEC_LEN);
	if (ret)
		dev_err(dev, "%s: unable to read RTC time section (%d)\n",
			__func__, ret);

out:
	if (ret)
		return ret;

	isl12058_rtc_regs_to_tm(tm, regs);

	return rtc_valid_tm(tm);
}

static int isl12058_rtc_update_alarm(struct device *dev, int enable)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	ret = _isl12058_rtc_update_alarm(dev, enable);
	mutex_unlock(&data->lock);

	return ret;
}

static int isl12058_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);
	struct rtc_time rtc_tm, *alarm_tm = &alarm->time;
	unsigned long rtc_secs, alarm_secs;
	u8 regs[ISL12058_A1_SEC_LEN];
	unsigned int ir;
	int ret;

	mutex_lock(&data->lock);
	ret = regmap_bulk_read(data->regmap, ISL12058_REG_A1_SC, regs,
			       ISL12058_A1_SEC_LEN);
	if (ret) {
		dev_err(dev, "%s: reading alarm section failed (%d)\n",
			__func__, ret);
		goto err_unlock;
	}

	alarm_tm->tm_sec  = bcd2bin(regs[0] & 0x7f);
	alarm_tm->tm_min  = bcd2bin(regs[1] & 0x7f);
	alarm_tm->tm_hour = bcd2bin(regs[2] & 0x3f);
	alarm_tm->tm_mday = bcd2bin(regs[3] & 0x3f);
	alarm_tm->tm_wday = -1;

	/*
	 * The alarm section does not store year/month. We use the ones in rtc
	 * section as a basis and increment month and then year if needed to get
	 * alarm after current time.
	 */
	ret = _isl12058_rtc_read_time(dev, &rtc_tm);
	if (ret)
		goto err_unlock;

	alarm_tm->tm_year = rtc_tm.tm_year;
	alarm_tm->tm_mon = rtc_tm.tm_mon;

	ret = rtc_tm_to_time(&rtc_tm, &rtc_secs);
	if (ret)
		goto err_unlock;

	ret = rtc_tm_to_time(alarm_tm, &alarm_secs);
	if (ret)
		goto err_unlock;

	if (alarm_secs < rtc_secs) {
		if (alarm_tm->tm_mon == 11) {
			alarm_tm->tm_mon = 0;
			alarm_tm->tm_year += 1;
		} else {
			alarm_tm->tm_mon += 1;
		}
	}

	ret = regmap_read(data->regmap, ISL12058_REG_INT, &ir);
	if (ret) {
		dev_err(dev, "%s: reading alarm interrupt flag failed (%d)\n",
			__func__, ret);
		goto err_unlock;
	}

	alarm->enabled = !!(ir & ISL12058_REG_INT_A1IE);

err_unlock:
	mutex_unlock(&data->lock);

	return ret;
}

static int isl12058_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);
	struct rtc_time *alarm_tm = &alarm->time;
	unsigned long rtc_secs, alarm_secs;
	u8 regs[ISL12058_A1_SEC_LEN];
	struct rtc_time rtc_tm;
	int ret, enable = 1;

	mutex_lock(&data->lock);
	ret = _isl12058_rtc_read_time(dev, &rtc_tm);
	if (ret)
		goto err_unlock;

	ret = rtc_tm_to_time(&rtc_tm, &rtc_secs);
	if (ret)
		goto err_unlock;

	ret = rtc_tm_to_time(alarm_tm, &alarm_secs);
	if (ret)
		goto err_unlock;

	/* If alarm time is before current time, disable the alarm */
	if (!alarm->enabled || alarm_secs <= rtc_secs) {
		enable = 0;
	} else {
		/*
		 * Chip only support alarms up to one month in the future. Let's
		 * return an error if we get something after that limit.
		 * Comparison is done by incrementing rtc_tm month field by one
		 * and checking alarm value is still below.
		 */
		if (rtc_tm.tm_mon == 11) { /* handle year wrapping */
			rtc_tm.tm_mon = 0;
			rtc_tm.tm_year += 1;
		} else {
			rtc_tm.tm_mon += 1;
		}

		ret = rtc_tm_to_time(&rtc_tm, &rtc_secs);
		if (ret)
			goto err_unlock;

		if (alarm_secs > rtc_secs) {
			dev_err(dev, "%s: max for alarm is one month (%d)\n",
				__func__, ret);
			ret = -EINVAL;
			goto err_unlock;
		}
	}

	/* Disable the alarm before modifying it */
	ret = _isl12058_rtc_update_alarm(dev, 0);
	if (ret < 0) {
		dev_err(dev, "%s: unable to disable the alarm (%d)\n",
			__func__, ret);
		goto err_unlock;
	}

	/* Program alarm registers */
	regs[0] = bin2bcd(alarm_tm->tm_sec) & 0x7f;
	regs[1] = bin2bcd(alarm_tm->tm_min) & 0x7f;
	regs[2] = bin2bcd(alarm_tm->tm_hour) & 0x3f;
	regs[3] = bin2bcd(alarm_tm->tm_mday) & 0x3f;

	ret = regmap_bulk_write(data->regmap, ISL12058_REG_A1_SC, regs,
				ISL12058_A1_SEC_LEN);
	if (ret < 0) {
		dev_err(dev, "%s: writing alarm section failed (%d)\n",
			__func__, ret);
		goto err_unlock;
	}

	/* Enable or disable alarm */
	ret = _isl12058_rtc_update_alarm(dev, enable);

err_unlock:
	mutex_unlock(&data->lock);

	return ret;
}

static int isl12058_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);
	u8 regs[ISL12058_RTC_SEC_LEN];
	int ret;

	ret = isl12058_rtc_tm_to_regs(regs, tm);
	if (ret)
		return ret;

	mutex_lock(&data->lock);
	ret = regmap_bulk_write(data->regmap, ISL12058_REG_RTC_SC, regs,
				ISL12058_RTC_SEC_LEN);
	if (ret) {
		dev_err(dev, "%s: unable to write RTC time section (%d)\n",
			__func__, ret);
		goto out;
	}

out:
	mutex_unlock(&data->lock);

	return ret;
}

/*
 * Check current RTC status and enable/disable what needs to be. Return 0 if
 * everything went ok and a negative value upon error. Note: this function
 * is called early during init and hence does need mutex protection.
 */
static int isl12058_check_rtc_status(struct device *dev, struct regmap *regmap)
{
	int ret;

	/* Enable RTC write. Do it with a whole SR write (otherwise it's ignored) */
	ret = regmap_write(regmap, ISL12058_REG_SR, ISL12058_REG_SR_WRTC | ISL12058_REG_SR_OSF);
	if (ret < 0) {
		dev_err(dev, "%s: unable to enable oscillator (%d)\n",
			__func__, ret);
		return ret;
	}

	/* Clear alarm bit if needed */
	ret = regmap_update_bits(regmap, ISL12058_REG_SR,
				 ISL12058_REG_SR_A1F, 0);
	if (ret < 0) {
		dev_err(dev, "%s: unable to clear alarm bit (%d)\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

#ifdef CONFIG_OF
/*
 * One would expect the device to be marked as a wakeup source only
 * when an IRQ pin of the RTC is routed to an interrupt line of the
 * CPU. In practice, such an IRQ pin can be connected to a PMIC and
 * this allows the device to be powered up when RTC alarm rings. This
 * is for instance the case on ReadyNAS 102, 104 and 2120. On those
 * devices with no IRQ driectly connected to the SoC, the RTC chip
 * can be forced as a wakeup source by stating that explicitly in
 * the device's .dts file using the "wakeup-source" boolean property.
 * This will guarantee 'wakealarm' sysfs entry is available on the device.
 *
 * The function below returns 1, i.e. the capability of the chip to
 * wakeup the device, based on IRQ availability or if the boolean
 * property has been set in the .dts file. Otherwise, it returns 0.
 */

static bool isl12058_can_wakeup_machine(struct device *dev)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);

	return data->irq || of_property_read_bool(dev->of_node, "wakeup-source")
		|| of_property_read_bool(dev->of_node, /* legacy */
					 "isil,irq2-can-wakeup-machine");
}
#else
static bool isl12058_can_wakeup_machine(struct device *dev)
{
	struct isl12058_rtc_data *data = dev_get_drvdata(dev);

	return !!data->irq;
}
#endif

static int isl12058_rtc_alarm_irq_enable(struct device *dev,
					 unsigned int enable)
{
	struct isl12058_rtc_data *rtc_data = dev_get_drvdata(dev);
	int ret = -ENOTTY;

	if (rtc_data->irq)
		ret = isl12058_rtc_update_alarm(dev, enable);

	return ret;
}

static irqreturn_t isl12058_rtc_interrupt(int irq, void *data)
{
	struct i2c_client *client = data;
	struct isl12058_rtc_data *rtc_data = dev_get_drvdata(&client->dev);
	struct rtc_device *rtc = rtc_data->rtc;
	int ret, handled = IRQ_NONE;
	unsigned int sr;

	ret = regmap_read(rtc_data->regmap, ISL12058_REG_SR, &sr);
	if (!ret && (sr & ISL12058_REG_SR_A1F)) {
		dev_dbg(&client->dev, "RTC alarm!\n");

		rtc_update_irq(rtc, 1, RTC_IRQF | RTC_AF);

		/* Acknowledge and disable the alarm */
		_isl12058_rtc_clear_alarm(&client->dev);
		_isl12058_rtc_update_alarm(&client->dev, 0);

		handled = IRQ_HANDLED;
	}

	return handled;
}

static const struct rtc_class_ops rtc_ops = {
	.read_time = _isl12058_rtc_read_time,
	.set_time = isl12058_rtc_set_time,
	.read_alarm = isl12058_rtc_read_alarm,
	.set_alarm = isl12058_rtc_set_alarm,
	.alarm_irq_enable = isl12058_rtc_alarm_irq_enable,
};

static const struct regmap_config isl12058_rtc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int isl12058_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct isl12058_rtc_data *data;
	struct regmap *regmap;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_I2C_BLOCK))
		return -ENODEV;

	regmap = devm_regmap_init_i2c(client, &isl12058_rtc_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(dev, "%s: regmap allocation failed (%d)\n",
			__func__, ret);
		return ret;
	}

	ret = isl12058_i2c_validate_chip(regmap);
	if (ret)
		return ret;

	ret = isl12058_check_rtc_status(dev, regmap);
	if (ret)
		return ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);
	data->regmap = regmap;
	dev_set_drvdata(dev, data);

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						isl12058_rtc_interrupt,
						IRQF_SHARED|IRQF_ONESHOT,
						DRV_NAME, client);
		if (!ret)
			data->irq = client->irq;
		else
			dev_err(dev, "%s: irq %d unavailable (%d)\n", __func__,
				client->irq, ret);
	}

	if (isl12058_can_wakeup_machine(dev))
		device_init_wakeup(dev, true);

	data->rtc = devm_rtc_allocate_device(dev);
	ret = PTR_ERR_OR_ZERO(data->rtc);
	if (ret) {
		dev_err(dev, "%s: unable to register RTC device (%d)\n",
			__func__, ret);
		goto err;
	}

	ret = rtc_add_group(data->rtc, &isl12058_rtc_sysfs_files);
	if (ret) {
		dev_err(dev, "%s: unable to setup sysfs nodes (%d)\n",
			__func__, ret);
		goto err;
	}

	data->rtc->ops = &rtc_ops;
	ret = rtc_register_device(data->rtc);
	if (ret) {
		dev_err(dev, "%s: unable to register RTC device (%d)\n",
			__func__, ret);
		goto err;
	}

	/* We cannot support UIE mode if we do not have an IRQ line */
	if (!data->irq)
		data->rtc->uie_unsupported = 1;

err:
	return ret;
}

static int isl12058_remove(struct i2c_client *client)
{
	if (isl12058_can_wakeup_machine(&client->dev))
		device_init_wakeup(&client->dev, false);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int isl12058_rtc_suspend(struct device *dev)
{
	struct isl12058_rtc_data *rtc_data = dev_get_drvdata(dev);

	if (rtc_data->irq && device_may_wakeup(dev))
		return enable_irq_wake(rtc_data->irq);

	return 0;
}

static int isl12058_rtc_resume(struct device *dev)
{
	struct isl12058_rtc_data *rtc_data = dev_get_drvdata(dev);

	if (rtc_data->irq && device_may_wakeup(dev))
		return disable_irq_wake(rtc_data->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(isl12058_rtc_pm_ops, isl12058_rtc_suspend,
			 isl12058_rtc_resume);

#ifdef CONFIG_OF
static const struct of_device_id isl12058_dt_match[] = {
	{ .compatible = "isl,isl12058" }, /* for backward compat., don't use */
	{ .compatible = "isil,isl12058" },
	{ },
};
MODULE_DEVICE_TABLE(of, isl12058_dt_match);
#endif

static const struct i2c_device_id isl12058_id[] = {
	{ "isl12058", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, isl12058_id);

static struct i2c_driver isl12058_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = &isl12058_rtc_pm_ops,
		.of_match_table = of_match_ptr(isl12058_dt_match),
	},
	.probe	  = isl12058_probe,
	.remove	 = isl12058_remove,
	.id_table = isl12058_id,
};
module_i2c_driver(isl12058_driver);

MODULE_AUTHOR("Arnaud EBALARD <arno@natisbad.org>");
MODULE_DESCRIPTION("Intersil ISL12058 RTC driver");
MODULE_LICENSE("GPL");
