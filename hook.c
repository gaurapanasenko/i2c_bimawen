// SPDX-License-Identifier: GPL-2.0-only
/*
 * Swap DDC/CI VCP 0x10 <-> 0x12 on HSJ Bimawen panels (i915 DP AUX).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "backlight.h"
#include "ddc.h"
#include "retry.h"

#include <linux/i2c.h>
#include <linux/string.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>

struct i2c_bimawen_hook {
	struct list_head list;
	struct i2c_adapter *adap;
	const struct i2c_algorithm *orig_algo;
	struct i2c_algorithm wrap_algo;
	struct i2c_bimawen_tx tx;
	struct mutex lock;
	void *bl;
	struct kref ref;
};

static void hook_free(struct kref *ref)
{
	struct i2c_bimawen_hook *h = container_of(ref, struct i2c_bimawen_hook, ref);

	mutex_destroy(&h->lock);
	kfree(h);
}

static LIST_HEAD(hooks);
static DEFINE_MUTEX(hooks_lock);

static int adapter_nr = -1;
module_param(adapter_nr, int, 0444);
MODULE_PARM_DESC(adapter_nr, "Limit to i2c adapter number (-1 = all matches)");

static void scan_adapters(void);
static void drop_all_hooks(void);

static bool edid_relaxed;

static int edid_relaxed_set(const char *val, const struct kernel_param *kp)
{
	bool prev = edid_relaxed;
	int err = param_set_bool(val, kp);

	if (err || edid_relaxed == prev)
		return err;
	if (!edid_relaxed)
		drop_all_hooks();
	scan_adapters();
	return 0;
}

static const struct kernel_param_ops edid_relaxed_param_ops = {
	.set = edid_relaxed_set,
	.get = param_get_bool,
};

module_param_cb(edid_relaxed, &edid_relaxed_param_ops, &edid_relaxed, 0644);
MODULE_PARM_DESC(edid_relaxed,
		 "Match HSJ 0x0133 without Bimawen name in EDID");

static bool adapter_aux_dp(const struct i2c_adapter *adap)
{
	return adap && strnstr(adap->name, "AUX", I2C_NAME_SIZE);
}

static struct i2c_bimawen_hook *find_hook_locked(struct i2c_adapter *adap)
{
	struct i2c_bimawen_hook *h;

	list_for_each_entry(h, &hooks, list) {
		if (h->adap == adap)
			return h;
	}
	return NULL;
}

static bool adapter_hooked(struct i2c_adapter *adap)
{
	bool hooked;

	mutex_lock(&hooks_lock);
	hooked = find_hook_locked(adap) != NULL;
	mutex_unlock(&hooks_lock);
	return hooked;
}

static int i2c_bimawen_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			      int num)
{
	struct i2c_bimawen_hook *h;
	int ret, i;
	bool ddc = false;

	mutex_lock(&hooks_lock);
	h = find_hook_locked(adap);
	if (!h || !h->orig_algo->master_xfer) {
		mutex_unlock(&hooks_lock);
		return -ENODEV;
	}

	if (!try_module_get(THIS_MODULE)) {
		mutex_unlock(&hooks_lock);
		return -ENODEV;
	}

	kref_get(&h->ref);
	mutex_unlock(&hooks_lock);

	for (i = 0; i < num; i++) {
		if (msgs[i].addr == DDC_CI_ADDR) {
			ddc = true;
			break;
		}
	}
	if (ddc) {
		mutex_lock(&h->lock);
		for (i = 0; i < num; i++) {
			if (msgs[i].addr != DDC_CI_ADDR)
				continue;
			if ((msgs[i].flags & I2C_M_RD) || msgs[i].len != 1) {
				i2c_bimawen_tx_reset(&h->tx);
				break;
			}
		}
	}
	for (i = 0; i < num; i++) {
		if (msgs[i].addr != DDC_CI_ADDR)
			continue;
		if (msgs[i].flags & I2C_M_RD)
			i2c_bimawen_tx_reset(&h->tx);
		else if (msgs[i].len == 1 && msgs[i].buf)
			i2c_bimawen_tx_out_byte(&h->tx, msgs[i].buf);
	}

	if (ddc)
		i2c_bimawen_ddc_munge_msgs(msgs, num, false);
	ret = h->orig_algo->master_xfer(adap, msgs, num);
	if (ddc) {
		if (ret == num) {
			u16 lum;

			i2c_bimawen_ddc_munge_msgs(msgs, num, true);
			if (h->bl &&
			    i2c_bimawen_ddc_sniff_luminance(msgs, num, true, &h->tx,
						      &lum)) {
				/* Our own read is published by bl_get, which drops a stale reply. */
				if (!i2c_bimawen_bl_skip_sync(&h->bl))
					i2c_bimawen_bl_report(&h->bl, lum);
			} else if (h->bl &&
				   i2c_bimawen_ddc_sniff_luminance(msgs, num, false, &h->tx,
							      &lum)) {
				i2c_bimawen_bl_report(&h->bl, lum);
			}
			/* shortcut: byte state ends with this xfer, one byte per ioctl is not swapped */
			i2c_bimawen_tx_reset(&h->tx);
		} else {
			i2c_bimawen_ddc_restore_outbound(msgs, num, &h->tx);
		}
		mutex_unlock(&h->lock);
	}
	kref_put(&h->ref, hook_free);
	module_put(THIS_MODULE);
	return ret;
}

static int read_edid(struct i2c_adapter *adap, u8 *edid)
{
	u8 off = 0;
	struct i2c_msg msgs[2] = {
		{ .addr = 0x50, .flags = 0, .len = 1, .buf = &off },
		{ .addr = 0x50, .flags = I2C_M_RD, .len = 128, .buf = edid },
	};

	if (!adap->algo || !adap->algo->master_xfer)
		return -EIO;
	return i2c_transfer(adap, msgs, 2) == 2 ? 0 : -EIO;
}

static void uninstall_hook(struct i2c_adapter *adap)
{
	struct i2c_bimawen_hook *h;

	/* i2c_transfer holds this lock across the algo->master_xfer load. */
	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	mutex_lock(&hooks_lock);
	h = find_hook_locked(adap);
	if (!h) {
		mutex_unlock(&hooks_lock);
		i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
		return;
	}
	adap->algo = h->orig_algo;
	list_del(&h->list);
	mutex_unlock(&hooks_lock);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);

	i2c_bimawen_bl_detach(&h->bl);
	kref_put(&h->ref, hook_free);
}

static bool try_hook_adapter(struct i2c_adapter *adap, bool arm_retry);

static bool edid_retry_run(struct i2c_adapter *adap, void *ctx)
{
	/* false: retry_fn reschedules; schedule() here would cancel_sync this work */
	return try_hook_adapter(adap, false);
}

static const struct i2c_bimawen_retry_spec edid_retry_spec = {
	.run = edid_retry_run,
};

static int install_hook(struct i2c_adapter *adap)
{
	struct i2c_bimawen_hook *h;

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h) {
		pr_err("i2c-%d: hook alloc failed\n", adap->nr);
		return -ENOMEM;
	}

	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	mutex_lock(&hooks_lock);
	if (find_hook_locked(adap)) {
		mutex_unlock(&hooks_lock);
		i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
		kfree(h);
		return 0;
	}

	if (!adap->algo || !adap->algo->master_xfer) {
		mutex_unlock(&hooks_lock);
		i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
		kfree(h);
		return -ENODEV;
	}

	h->adap = adap;
	h->orig_algo = adap->algo;
	h->wrap_algo = *adap->algo;
	h->wrap_algo.master_xfer = i2c_bimawen_master_xfer;

	mutex_init(&h->lock);
	kref_init(&h->ref);
	adap->algo = &h->wrap_algo;
	list_add_tail(&h->list, &hooks);
	mutex_unlock(&hooks_lock);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);

	pr_info("hooked i2c-%d (%s)\n", adap->nr, adap->name);
	if (!i2c_bimawen_bl_attach(adap, &h->bl))
		pr_warn("i2c-%d: no sysfs backlight yet (DDC read failed)\n",
			adap->nr);
	return 0;
}

/* true: stop retrying. Keep going only when the EDID is not readable yet. */
static bool try_hook_adapter(struct i2c_adapter *adap, bool arm_retry)
{
	u8 edid[128];

	if (!adapter_aux_dp(adap))
		return true;

	if (adapter_nr >= 0 && adap->nr != adapter_nr)
		return true;

	if (adapter_hooked(adap))
		return true;

	/* Training junk can look like a header but fail the block checksum. */
	if (read_edid(adap, edid) != 0 ||
	    !i2c_bimawen_edid_header_ok(edid, sizeof(edid)) ||
	    !i2c_bimawen_edid_block_ok(edid, sizeof(edid))) {
		if (arm_retry)
			i2c_bimawen_retry_schedule(adap, NULL, &edid_retry_spec);
		return false;
	}

	if (!i2c_bimawen_edid_match_bimawen(edid, sizeof(edid), edid_relaxed))
		return true;

	return install_hook(adap) == 0;
}

static void scan_adapters(void)
{
	struct i2c_adapter *adap;
	int i;

	for (i = 0; i < 256; i++) {
		adap = i2c_get_adapter(i);
		if (!adap)
			continue;
		try_hook_adapter(adap, true);
		i2c_put_adapter(adap);
	}
}

static int i2c_bimawen_notify(struct notifier_block *nb, unsigned long action,
			 void *devptr)
{
	struct device *dev = devptr;
	struct i2c_adapter *adap;

	if (dev->type != &i2c_adapter_type)
		return NOTIFY_DONE;

	adap = to_i2c_adapter(dev);
	if (action == BUS_NOTIFY_ADD_DEVICE) {
		try_hook_adapter(adap, true);
	} else if (action == BUS_NOTIFY_DEL_DEVICE) {
		i2c_bimawen_retry_cancel_adap(adap);
		uninstall_hook(adap);
	}
	return NOTIFY_OK;
}

static struct notifier_block i2c_bimawen_nb = {
	.notifier_call = i2c_bimawen_notify,
};

static void drop_all_hooks(void)
{
	struct i2c_bimawen_hook *h;
	struct i2c_adapter *adap;

	for (;;) {
		mutex_lock(&hooks_lock);
		if (list_empty(&hooks)) {
			mutex_unlock(&hooks_lock);
			break;
		}
		h = list_first_entry(&hooks, struct i2c_bimawen_hook, list);
		adap = h->adap;
		mutex_unlock(&hooks_lock);
		uninstall_hook(adap);
	}
}

static int __init i2c_bimawen_init(void)
{
	int err;

	err = bus_register_notifier(&i2c_bus_type, &i2c_bimawen_nb);
	if (err)
		return err;
	scan_adapters();
	return 0;
}

static void __exit i2c_bimawen_exit(void)
{
	bus_unregister_notifier(&i2c_bus_type, &i2c_bimawen_nb);
	i2c_bimawen_retry_drop_all();
	drop_all_hooks();
}

module_init(i2c_bimawen_init);
module_exit(i2c_bimawen_exit);

MODULE_AUTHOR("Yehor Panasenko <yehor.panasenko@gmail.com>");
MODULE_DESCRIPTION("DDC/CI VCP 0x10/0x12 swap for HSJ Bimawen (i2c_bimawen)");
MODULE_LICENSE("GPL");