/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef I2C_BIMAWEN_RETRY_H
#define I2C_BIMAWEN_RETRY_H

#include <linux/types.h>

#define I2C_BIMAWEN_RETRY_MAX 5

struct i2c_adapter;

struct i2c_bimawen_retry_spec {
	bool (*run)(struct i2c_adapter *adap, void *ctx);
	void (*give_up)(struct i2c_adapter *adap, void *ctx);
};

void i2c_bimawen_retry_schedule(struct i2c_adapter *adap, void *ctx,
			   const struct i2c_bimawen_retry_spec *spec);
void i2c_bimawen_retry_cancel_adap(struct i2c_adapter *adap);
void i2c_bimawen_retry_cancel_ctx(void *ctx);
void i2c_bimawen_retry_drop_all(void);

#endif