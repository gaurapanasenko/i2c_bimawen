// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "backlight.h"
#include "ddc.h"
#include "retry.h"

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

static bool backlight = true;
module_param(backlight, bool, 0644);
MODULE_PARM_DESC(backlight,
		 "Register sysfs backlight (0 = DDC VCP remap only)");

struct i2c_bimawen_bl_ctx {
	struct i2c_adapter *adap;
	struct backlight_device *bd;
	struct mutex lock;
	struct task_struct *self; /* this task's own DDC read is not an external change */
	unsigned long last_get_jiffies;
	u32 seq; /* bumps on every level recorded from the bus */
	u16 cached_brightness;
};

static struct device *bl_drm_parent(struct i2c_adapter *adap)
{
	struct device *dev = adap->dev.parent;

	while (dev) {
		const char *name = dev_name(dev);

		if (name && !strncmp(name, "card", 4) && strchr(name, '-'))
			return dev;
		dev = dev->parent;
	}
	return &adap->dev;
}

static int ddc_send(struct i2c_adapter *adap, const u8 *pay, int paylen,
		    bool pflag)
{
	u8 pkt[20];
	struct i2c_msg wmsg;
	int i, len;

	if (paylen > 12)
		return -EINVAL;

	len = 2 + paylen + 1;
	pkt[0] = 0x51;
	pkt[1] = (u8)((pflag ? 0x80 : 0) | paylen);
	for (i = 0; i < paylen; i++)
		pkt[2 + i] = pay[i];
	pkt[2 + paylen] = 0;
	for (i = 0; i < len - 1; i++)
		pkt[len - 1] ^= pkt[i];
	pkt[len - 1] ^= DDC_CI_ADDR << 1;

	wmsg.addr = DDC_CI_ADDR;
	wmsg.flags = 0;
	wmsg.len = len;
	wmsg.buf = pkt;
	if (i2c_transfer(adap, &wmsg, 1) == 1)
		return 0;
	return -EIO;
}

static int ddc_recv(struct i2c_adapter *adap, u8 *r, int rlen)
{
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = rlen, .buf = r,
	};
	int ret = i2c_transfer(adap, &msg, 1);

	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;
	return rlen;
}

static int ddc_get_vcp(struct i2c_adapter *adap, u8 vcp, u16 *val, u16 *max)
{
	static const bool pf[] = { true, false };
	u8 pay[2] = { 0x01, vcp };
	u8 r[32];
	int i, ret, err = -EIO;

	for (i = 0; i < 2; i++) {
		ret = ddc_send(adap, pay, sizeof(pay), pf[i]);
		if (ret)
			continue;
		msleep(50); /* DDC/CI min gap; shortcut: panel timing, raise if reads fail */
		memset(r, 0, sizeof(r));
		ret = ddc_recv(adap, r, sizeof(r));
		if (ret < 0)
			continue;
		err = i2c_bimawen_ddc_parse_get_vcp(r, sizeof(r), vcp, val, max);
		if (!err)
			return 0;
	}
	return err;
}

static int ddc_set_vcp(struct i2c_adapter *adap, u8 vcp, u16 val)
{
	u8 pay[4] = {
		0x03, vcp, (u8)(val >> 8), (u8)(val & 0xff),
	};

	return ddc_send(adap, pay, sizeof(pay), true);
}

static int bl_get(struct backlight_device *bd)
{
	struct i2c_bimawen_bl_ctx *ctx = bl_get_data(bd);
	unsigned long seen;
	u16 val;
	int ret;

	if (!ctx)
		return -ENODEV;
	if (backlight_is_blank(bd))
		return 0;
	mutex_lock(&ctx->lock);
	seen = ctx->last_get_jiffies;
	if (time_before(jiffies, seen + 2 * HZ)) {
		ret = ctx->cached_brightness;
		mutex_unlock(&ctx->lock);
		return ret;
	}
	mutex_unlock(&ctx->lock);
	WRITE_ONCE(ctx->self, current);
	ret = ddc_get_vcp(ctx->adap, VCP_LUMINANCE, &val, NULL);
	WRITE_ONCE(ctx->self, NULL);
	if (ret) {
		mutex_lock(&ctx->lock);
		ret = ctx->cached_brightness;
		mutex_unlock(&ctx->lock);
		return ret;
	}
	mutex_lock(&ctx->lock);
	if (ctx->last_get_jiffies == seen) {
		ctx->last_get_jiffies = jiffies;
		ctx->cached_brightness = val;
	} else {
		val = ctx->cached_brightness;
	}
	mutex_unlock(&ctx->lock);
	return val;
}

static int bl_update(struct backlight_device *bd)
{
	struct i2c_bimawen_bl_ctx *ctx = bl_get_data(bd);
	u32 seq;
	int bright, ret;

	if (!ctx)
		return -ENODEV;
	bright = backlight_get_brightness(bd);
	mutex_lock(&ctx->lock);
	seq = ctx->seq;
	mutex_unlock(&ctx->lock);
	WRITE_ONCE(ctx->self, current);
	ret = ddc_set_vcp(ctx->adap, VCP_LUMINANCE, bright);
	WRITE_ONCE(ctx->self, NULL);
	if (backlight_is_blank(bd))
		return ret;
	/*
	 * The core keeps the requested level even when the write fails.
	 * The hook records what landed while it still held the bus; a later
	 * DDC write wins. Wait for that bus before publishing.
	 */
	i2c_lock_bus(ctx->adap, I2C_LOCK_SEGMENT);
	mutex_lock(&ctx->lock);
	if (!ret && ctx->seq == seq) {
		ctx->cached_brightness = bright;
		ctx->last_get_jiffies = jiffies;
	}
	bd->props.brightness = ctx->cached_brightness;
	mutex_unlock(&ctx->lock);
	i2c_unlock_bus(ctx->adap, I2C_LOCK_SEGMENT);
	return ret;
}

static const struct backlight_ops i2c_bimawen_bl_ops = {
	.get_brightness	= bl_get,
	.update_status	= bl_update,
};

static bool bl_register(struct i2c_adapter *adap, void *vslot)
{
	struct i2c_bimawen_bl_ctx *ctx;
	struct backlight_properties props;
	void **slot = vslot;
	u16 val, max;
	char name[24];

	if (!slot || *slot)
		return true;
	if (ddc_get_vcp(adap, VCP_LUMINANCE, &val, &max))
		return false;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return false;
	ctx->adap = adap;
	ctx->cached_brightness = val;
	ctx->last_get_jiffies = jiffies;
	mutex_init(&ctx->lock);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = max;
	props.brightness = val;

	snprintf(name, sizeof(name), "i2c_bimawen-%d", adap->nr);
	ctx->bd = backlight_device_register(name, bl_drm_parent(adap), ctx,
					    &i2c_bimawen_bl_ops, &props);
	if (IS_ERR(ctx->bd)) {
		mutex_destroy(&ctx->lock);
		kfree(ctx);
		return false;
	}
	*slot = ctx;
	pr_info("backlight %s (max %u)\n", name, max);
	return true;
}

static void bl_retry_give_up(struct i2c_adapter *adap, void *ctx)
{
	pr_warn("i2c-%d luminance read failed, no backlight\n", adap->nr);
}

static const struct i2c_bimawen_retry_spec bl_retry_spec = {
	.run = bl_register,
	.give_up = bl_retry_give_up,
};

bool i2c_bimawen_bl_attach(struct i2c_adapter *adap, void **slot)
{
	if (!backlight)
		return true;
	i2c_bimawen_retry_cancel_ctx(slot);
	if (bl_register(adap, slot))
		return true;
	i2c_bimawen_retry_schedule(adap, slot, &bl_retry_spec);
	return false;
}

void i2c_bimawen_bl_detach(void **slot)
{
	struct i2c_bimawen_bl_ctx *ctx;

	i2c_bimawen_retry_cancel_ctx(slot);
	if (!slot || !*slot)
		return;
	ctx = *slot;
	backlight_device_unregister(ctx->bd);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	*slot = NULL;
}

bool i2c_bimawen_bl_skip_sync(void **slot)
{
	struct i2c_bimawen_bl_ctx *ctx;

	if (!slot || !*slot)
		return true;
	ctx = *slot;
	return READ_ONCE(ctx->self) == current;
}

void i2c_bimawen_bl_report(void **slot, u16 brightness)
{
	struct i2c_bimawen_bl_ctx *ctx;
	struct backlight_device *bd;
	bool mine, blank;

	if (!slot || !*slot)
		return;
	ctx = *slot;
	bd = ctx->bd;
	if (!bd || brightness > bd->props.max_brightness)
		return;
	mine = READ_ONCE(ctx->self) == current;
	blank = backlight_is_blank(bd);
	/* Blanking writes 0. That must not replace the level the user asked for. */
	if (mine && blank)
		return;

	mutex_lock(&ctx->lock);
	ctx->cached_brightness = brightness;
	ctx->last_get_jiffies = jiffies;
	ctx->seq++;
	mutex_unlock(&ctx->lock);
	if (mine || blank)
		return;
	/* update_status already holds this across the bus transfer */
	if (mutex_trylock(&bd->update_lock)) {
		bd->props.brightness = brightness;
		mutex_unlock(&bd->update_lock);
	}
	sysfs_notify(&bd->dev.kobj, NULL, "brightness");
	sysfs_notify(&bd->dev.kobj, NULL, "actual_brightness");
}
