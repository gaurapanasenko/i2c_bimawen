/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef I2C_BIMAWEN_BACKLIGHT_H
#define I2C_BIMAWEN_BACKLIGHT_H

#include <linux/types.h>

struct i2c_adapter;

bool i2c_bimawen_bl_attach(struct i2c_adapter *adap, void **slot);
void i2c_bimawen_bl_detach(void **slot);
bool i2c_bimawen_bl_skip_sync(void **slot);
void i2c_bimawen_bl_report(void **slot, u16 brightness);

#endif