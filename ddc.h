/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef I2C_BIMAWEN_DDC_H
#define I2C_BIMAWEN_DDC_H

#ifdef I2C_BIMAWEN_USERSPACE_TEST
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EIO 5
typedef uint8_t u8;
typedef uint16_t u16;
struct i2c_msg {
	unsigned short addr;
	unsigned short flags;
	unsigned short len;
	u8 *buf;
};
#define I2C_M_RD 0x0001
#else
#include <linux/i2c.h>
#endif

#define DDC_CI_ADDR 0x37
#define VCP_LUMINANCE 0x10
/* shortcut: 32 swapped byte-frames per master_xfer, raise if batched tool appears */
#define I2C_BIMAWEN_TX_UNDO_MAX 32

struct i2c_bimawen_tx_undo {
	u8 *vcp_byte;
	u8 *chk_byte;
};

struct i2c_bimawen_tx {
	u8 st;
	u8 pay_rem;
	u8 pay_i;
	u8 cmd;
	bool vcp_swapped;
	u8 vcp_old;
	u8 set_hi;
	u8 set_lo;
	bool lum_set;
	u8 *vcp_byte;
	u8 *chk_byte;
	u8 undo_n;
	struct i2c_bimawen_tx_undo undo[I2C_BIMAWEN_TX_UNDO_MAX];
};

u8 i2c_bimawen_vcp_remap(u8 code);
void i2c_bimawen_tx_reset(struct i2c_bimawen_tx *tx);
void i2c_bimawen_tx_out_byte(struct i2c_bimawen_tx *tx, u8 *byte);
void i2c_bimawen_ddc_munge_msgs(struct i2c_msg *msgs, int num, bool post_xfer);
int i2c_bimawen_ddc_parse_get_vcp(const u8 *raw, int rawlen, u8 vcp, u16 *val,
			     u16 *max);
bool i2c_bimawen_tx_take_luminance(struct i2c_bimawen_tx *tx, u16 *val);
bool i2c_bimawen_ddc_sniff_luminance(struct i2c_msg *msgs, int num, bool post,
				struct i2c_bimawen_tx *tx, u16 *val);
void i2c_bimawen_tx_undo_outbound(struct i2c_bimawen_tx *tx);
void i2c_bimawen_ddc_restore_outbound(struct i2c_msg *msgs, int num,
				 struct i2c_bimawen_tx *tx);
bool i2c_bimawen_edid_header_ok(const u8 *edid, size_t len);
bool i2c_bimawen_edid_block_ok(const u8 *edid, size_t len);
bool i2c_bimawen_edid_match_bimawen(const u8 *edid, size_t len, bool relaxed);

#endif