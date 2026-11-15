// SPDX-License-Identifier: GPL-2.0-only
#include "retry.h"

#include <linux/i2c.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct i2c_bimawen_retry {
	struct list_head list;
	struct delayed_work work;
	struct i2c_adapter *adap;
	void *ctx;
	u8 attempts;
	bool reap; /* cancel owns the free */
	struct i2c_bimawen_retry_spec spec;
};

static LIST_HEAD(retries);
static DEFINE_MUTEX(retries_lock);

static bool retry_hit(struct i2c_bimawen_retry *r, struct i2c_adapter *adap,
		      void *ctx, bool by_adap, bool by_ctx)
{
	if (by_adap && r->adap != adap)
		return false;
	if (by_ctx && r->ctx != ctx)
		return false;
	return true;
}

static void retry_detach_locked(struct list_head *dead, struct i2c_adapter *adap,
				void *ctx, bool by_adap, bool by_ctx)
{
	struct i2c_bimawen_retry *r, *tmp;

	list_for_each_entry_safe(r, tmp, &retries, list) {
		if (!retry_hit(r, adap, ctx, by_adap, by_ctx))
			continue;
		r->reap = true;
		list_move_tail(&r->list, dead);
	}
}

/* cancel_sync waits for retry_fn; under retries_lock that deadlocks. */
static void retry_bury(struct list_head *dead)
{
	struct i2c_bimawen_retry *r, *tmp;

	list_for_each_entry_safe(r, tmp, dead, list) {
		cancel_delayed_work_sync(&r->work);
		list_del(&r->list);
		i2c_put_adapter(r->adap);
		kfree(r);
	}
}

static void retry_kill(struct i2c_adapter *adap, void *ctx, bool by_adap,
		       bool by_ctx)
{
	LIST_HEAD(dead);

	mutex_lock(&retries_lock);
	retry_detach_locked(&dead, adap, ctx, by_adap, by_ctx);
	mutex_unlock(&retries_lock);
	retry_bury(&dead);
}

static void retry_fn(struct work_struct *w)
{
	struct i2c_bimawen_retry *r = container_of(to_delayed_work(w),
					      struct i2c_bimawen_retry, work);
	const struct i2c_bimawen_retry_spec *spec = &r->spec;
	bool stop = false;

	mutex_lock(&retries_lock);
	if (r->reap) {
		mutex_unlock(&retries_lock);
		return;
	}
	mutex_unlock(&retries_lock);

	if (!device_is_registered(&r->adap->dev))
		stop = true;
	else if (spec->run(r->adap, r->ctx))
		stop = true;
	else if (++r->attempts >= I2C_BIMAWEN_RETRY_MAX) {
		if (spec->give_up)
			spec->give_up(r->adap, r->ctx);
		stop = true;
	}

	mutex_lock(&retries_lock);
	if (r->reap) {
		mutex_unlock(&retries_lock);
		return;
	}
	if (!stop) {
		schedule_delayed_work(&r->work, HZ);
		mutex_unlock(&retries_lock);
		return;
	}
	list_del_init(&r->list);
	mutex_unlock(&retries_lock);
	i2c_put_adapter(r->adap);
	kfree(r);
}

void i2c_bimawen_retry_schedule(struct i2c_adapter *adap, void *ctx,
			   const struct i2c_bimawen_retry_spec *spec)
{
	struct i2c_bimawen_retry *r;
	struct i2c_adapter *held;
	LIST_HEAD(dead);

	if (!adap || !spec || !spec->run)
		return;

	held = i2c_get_adapter(adap->nr);
	if (!held)
		return;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		i2c_put_adapter(held);
		return;
	}
	r->adap = held;
	r->ctx = ctx;
	r->spec = *spec;
	INIT_DELAYED_WORK(&r->work, retry_fn);

	mutex_lock(&retries_lock);
	retry_detach_locked(&dead, adap, ctx, true, true);
	list_add(&r->list, &retries);
	schedule_delayed_work(&r->work, HZ);
	mutex_unlock(&retries_lock);
	retry_bury(&dead);
}

void i2c_bimawen_retry_cancel_adap(struct i2c_adapter *adap)
{
	if (adap)
		retry_kill(adap, NULL, true, false);
}

void i2c_bimawen_retry_cancel_ctx(void *ctx)
{
	if (ctx)
		retry_kill(NULL, ctx, false, true);
}

void i2c_bimawen_retry_drop_all(void)
{
	retry_kill(NULL, NULL, false, false);
}
