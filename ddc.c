// SPDX-License-Identifier: GPL-2.0-only
#include "ddc.h"

#include <linux/string.h>
#ifdef I2C_BIMAWEN_USERSPACE_TEST
#include <stddef.h>
#include <string.h>

static const char *strnstr(const char *s, const char *find, size_t len)
{
	size_t flen = strlen(find);

	if (flen > len)
		return NULL;
	for (; len >= flen; len--, s++) {
		if (!memcmp(s, find, flen))
			return s;
	}
	return NULL;
}
#endif

u8 i2c_bimawen_vcp_remap(u8 code)
{
	switch (code) {
	case 0x10:
		return 0x12;
	case 0x12:
		return 0x10;
	default:
		return code;
	}
}

void i2c_bimawen_tx_reset(struct i2c_bimawen_tx *tx)
{
	memset(tx, 0, sizeof(*tx));
}

static void tx_abort_frame(struct i2c_bimawen_tx *tx);

void i2c_bimawen_tx_undo_outbound(struct i2c_bimawen_tx *tx)
{
	struct i2c_bimawen_tx_undo u;
	u8 neu;

	if (!tx)
		return;
	tx_abort_frame(tx);
	while (tx->undo_n > 0) {
		u = tx->undo[--tx->undo_n];
		if (!u.vcp_byte)
			continue;
		neu = i2c_bimawen_vcp_remap(*u.vcp_byte);
		if (u.chk_byte)
			*u.chk_byte ^= *u.vcp_byte ^ neu;
		*u.vcp_byte = neu;
	}
	i2c_bimawen_tx_reset(tx);
}

static void tx_end_frame(struct i2c_bimawen_tx *tx)
{
	u8 hi = tx->set_hi;
	u8 lo = tx->set_lo;
	bool lum = tx->lum_set;
	u8 undo_n = tx->undo_n;
	struct i2c_bimawen_tx_undo undo[I2C_BIMAWEN_TX_UNDO_MAX];

	memcpy(undo, tx->undo, sizeof(undo));
	memset(tx, 0, sizeof(*tx));
	tx->undo_n = undo_n;
	memcpy(tx->undo, undo, sizeof(undo));
	if (lum) {
		tx->lum_set = true;
		tx->set_hi = hi;
		tx->set_lo = lo;
	}
}

static void tx_abort_frame(struct i2c_bimawen_tx *tx)
{
	if (tx->vcp_swapped && tx->vcp_byte && tx->undo_n < I2C_BIMAWEN_TX_UNDO_MAX) {
		tx->undo[tx->undo_n].vcp_byte = tx->vcp_byte;
		tx->undo[tx->undo_n].chk_byte = tx->chk_byte;
		tx->undo_n++;
	}
	tx_end_frame(tx);
}

static int ddc_frame_len(const u8 *buf, int buf_len)
{
	int pay;

	if (!buf || buf_len < 3)
		return 0;
	pay = buf[1] & 0x7f;
	/* Declared frame does not fit: the last byte is not the checksum. */
	if (pay > buf_len - 3)
		return 0;
	return 3 + pay;
}

static void swap_vcp_and_checksum(u8 *buf, int len, int vcp_pos)
{
	u8 old, neu;
	int flen;

	if (vcp_pos < 0 || vcp_pos >= len)
		return;

	flen = ddc_frame_len(buf, len);
	if (flen < 4 || vcp_pos >= flen)
		return;

	old = buf[vcp_pos];
	neu = i2c_bimawen_vcp_remap(old);
	if (old == neu)
		return;

	buf[vcp_pos] = neu;
	if (flen > 1)
		buf[flen - 1] ^= old ^ neu;
}

#define DDC_REPLY 0x02

static int ddc_reply_off(const u8 *buf, int len, int start)
{
	int i;

	for (i = start; i + 4 < len; i++) {
		if (buf[i] != 0x6e && buf[i] != 0x51)
			continue;
		if (buf[i + 2] != DDC_REPLY || buf[i + 3] != 0x00)
			continue;
		return i;
	}
	return -1;
}

int i2c_bimawen_ddc_parse_get_vcp(const u8 *raw, int rawlen, u8 vcp, u16 *val,
			     u16 *max)
{
	int off, flen, i;
	u8 x;

	for (off = ddc_reply_off(raw, rawlen, 0); off >= 0;
	     off = ddc_reply_off(raw, rawlen, off + 1)) {
		/* Buffer is already swapped to the code userspace asked for. */
		if (raw[off + 4] != vcp)
			continue;
		flen = ddc_frame_len(raw + off, rawlen - off);
		if (flen < 11)
			continue;
		x = 0x50; /* virtual host address, not stored in the read */
		for (i = 0; i < flen; i++)
			x ^= raw[off + i];
		if (x)
			continue;
		if (max)
			*max = (raw[off + 6] << 8) | raw[off + 7];
		if (val)
			*val = (raw[off + 8] << 8) | raw[off + 9];
		return 0;
	}
	return -EIO;
}

static void munge_buf(u8 *buf, int len, bool inbound)
{
	int off, start;

	if (!buf || len < 4)
		return;

	/* ddcutil: [0x51][len][cmd][vcp]… */
	if (!inbound && buf[0] == 0x51 &&
	    (buf[2] == 0x01 || buf[2] == 0x03))
		swap_vcp_and_checksum(buf, len, 3);

	if (!inbound)
		return;

	start = 0;
	while ((off = ddc_reply_off(buf, len, start)) >= 0) {
		swap_vcp_and_checksum(buf + off, len - off, 4);
		start = off + 1;
	}
}

/* Userspace SMBus byte writes; block DDC uses munge_buf above. */
void i2c_bimawen_tx_out_byte(struct i2c_bimawen_tx *tx, u8 *byte)
{
	u8 neu;

	if (!tx || !byte)
		return;

	switch (tx->st) {
	case 0:
		if (*byte == 0x51)
			tx->st = 1;
		return;
	case 1:
		tx->pay_rem = *byte & 0x7f;
		tx->pay_i = 0;
		tx->cmd = 0;
		tx->vcp_swapped = false;
		tx->st = 2;
		return;
	case 2:
		if (tx->pay_i == 0)
			tx->cmd = *byte;
		else if (tx->pay_i == 1 && (tx->cmd == 0x01 || tx->cmd == 0x03)) {
			neu = i2c_bimawen_vcp_remap(*byte);
			if (neu != *byte) {
				tx->vcp_swapped = true;
				tx->vcp_old = *byte;
				tx->vcp_byte = byte;
				*byte = neu;
			}
		} else if (tx->cmd == 0x03 && tx->vcp_swapped &&
			   tx->vcp_old == VCP_LUMINANCE) {
			if (tx->pay_i == 2)
				tx->set_hi = *byte;
			else if (tx->pay_i == 3)
				tx->set_lo = *byte;
		}
		tx->pay_i++;
		if (tx->pay_rem > 0)
			tx->pay_rem--;
		if (tx->pay_rem == 0) {
			if (tx->cmd == 0x03 && tx->vcp_swapped &&
			    tx->vcp_old == VCP_LUMINANCE)
				tx->lum_set = true;
			tx->st = 3;
		}
		return;
	case 3:
		if (tx->vcp_swapped) {
			tx->chk_byte = byte;
			*byte ^= tx->vcp_old ^ i2c_bimawen_vcp_remap(tx->vcp_old);
			if (tx->undo_n < I2C_BIMAWEN_TX_UNDO_MAX) {
				tx->undo[tx->undo_n].vcp_byte = tx->vcp_byte;
				tx->undo[tx->undo_n].chk_byte = byte;
				tx->undo_n++;
			}
		}
		tx_end_frame(tx);
		return;
	default:
		i2c_bimawen_tx_reset(tx);
		return;
	}
}

bool i2c_bimawen_tx_take_luminance(struct i2c_bimawen_tx *tx, u16 *val)
{
	if (!tx || !tx->lum_set || !val)
		return false;

	*val = (tx->set_hi << 8) | tx->set_lo;
	tx->lum_set = false;
	return true;
}

static bool sniff_set_block(const u8 *buf, int len, u16 *val)
{
	int flen;

	if (!buf || len < 4 || buf[0] != 0x51 || buf[2] != 0x03)
		return false;
	flen = ddc_frame_len(buf, len);
	/* Outbound munge maps luminance 0x10 -> wire 0x12 */
	if (flen < 7 || buf[3] != 0x12)
		return false;
	*val = (buf[4] << 8) | buf[5];
	return true;
}

bool i2c_bimawen_ddc_sniff_luminance(struct i2c_msg *msgs, int num, bool post,
				struct i2c_bimawen_tx *tx, u16 *val)
{
	int i;
	u16 v;

	if (!msgs || !val)
		return false;
	for (i = 0; i < num; i++) {
		if (msgs[i].addr != DDC_CI_ADDR || !msgs[i].buf)
			continue;
		if (post && (msgs[i].flags & I2C_M_RD)) {
			if (!i2c_bimawen_ddc_parse_get_vcp(msgs[i].buf, msgs[i].len,
						      VCP_LUMINANCE, &v, NULL))
				goto found;
			continue;
		}
		if (!post && !(msgs[i].flags & I2C_M_RD) &&
		    msgs[i].len > 1 &&
		    sniff_set_block(msgs[i].buf, msgs[i].len, &v))
			goto found;
	}
	if (post && tx && i2c_bimawen_tx_take_luminance(tx, &v))
		goto found;
	return false;
found:
	*val = v;
	return true;
}

void i2c_bimawen_ddc_munge_msgs(struct i2c_msg *msgs, int num, bool post_xfer)
{
	int i;

	for (i = 0; i < num; i++) {
		if (msgs[i].addr != DDC_CI_ADDR || !msgs[i].buf || msgs[i].len <= 1)
			continue;
		if (post_xfer && !(msgs[i].flags & I2C_M_RD))
			continue;
		if (!post_xfer && (msgs[i].flags & I2C_M_RD))
			continue;

		munge_buf(msgs[i].buf, msgs[i].len, post_xfer);
	}
}

void i2c_bimawen_ddc_restore_outbound(struct i2c_msg *msgs, int num,
				 struct i2c_bimawen_tx *tx)
{
	i2c_bimawen_ddc_munge_msgs(msgs, num, false);
	i2c_bimawen_tx_undo_outbound(tx);
}

#define HSJ_VENDOR_ID ((('H' - 'A' + 1) << 10) | (('S' - 'A' + 1) << 5) | \
		       ('J' - 'A' + 1))

bool i2c_bimawen_edid_header_ok(const u8 *edid, size_t len)
{
	static const u8 hdr[8] = { 0x00, 0xff, 0xff, 0xff,
				   0xff, 0xff, 0xff, 0x00 };

	return edid && len >= sizeof(hdr) && !memcmp(edid, hdr, sizeof(hdr));
}

bool i2c_bimawen_edid_block_ok(const u8 *edid, size_t len)
{
	u8 sum = 0;
	size_t i;

	if (!edid || len < 128)
		return false;
	for (i = 0; i < 128; i++)
		sum += edid[i];
	return sum == 0;
}

bool i2c_bimawen_edid_match_bimawen(const u8 *edid, size_t len, bool relaxed)
{
	u16 id, product;
	int i;

	if (len < 128 || !i2c_bimawen_edid_header_ok(edid, len))
		return false;
	id = (edid[8] << 8) | edid[9];
	product = edid[10] | (edid[11] << 8);
	if (product != 0x0133)
		return false;
	if (id != HSJ_VENDOR_ID)
		return false;
	if (relaxed)
		return true;
	for (i = 0; i < 4; i++) {
		const u8 *desc = edid + 0x36 + i * 18;

		if (desc[3] != 0xfc)
			continue;
		if (strnstr((const char *)desc + 5, "Bimawen", 13))
			return true;
	}
	return false;
}
