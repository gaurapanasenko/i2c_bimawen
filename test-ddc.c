/* Userspace checks for ddc.c (make check) */
#include "ddc.h"

#include <stdio.h>
#include <string.h>

static u8 xor_buf(const u8 *buf, int len)
{
	u8 x = 0;
	int i;

	for (i = 0; i < len; i++)
		x ^= buf[i];
	return x;
}

static int test_block_out(void)
{
	u8 buf[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x50, 0x00 };
	struct i2c_msg msg = { .addr = DDC_CI_ADDR, .len = 7, .buf = buf };

	buf[6] = xor_buf(buf, 6);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, false);
	if (buf[3] != 0x12)
		return 1;
	if (buf[6] != xor_buf(buf, 6))
		return 1;
	return 0;
}

static int test_block_in(void)
{
	u8 buf[] = { 0x6e, 0x06, 0x02, 0x00, 0x12, 0x64, 0x00, 0x32, 0x00 };
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 9, .buf = buf,
	};

	buf[8] = xor_buf(buf, 8);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, true);
	if (buf[4] != 0x10)
		return 1;
	return buf[8] == xor_buf(buf, 8) ? 0 : 1;
}

static int test_byte_stream(void)
{
	struct i2c_bimawen_tx tx;
	u8 bytes[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x50, 0x00 };
	u8 chk;
	int i;

	i2c_bimawen_tx_reset(&tx);
	chk = xor_buf(bytes, 6);
	bytes[6] = chk;
	for (i = 0; i < 7; i++)
		i2c_bimawen_tx_out_byte(&tx, &bytes[i]);
	if (bytes[3] != 0x12)
		return 1;
	return bytes[6] == xor_buf(bytes, 6) ? 0 : 1;
}

static int test_byte_stream_contrast_no_lum(void)
{
	struct i2c_bimawen_tx tx;
	u8 bytes[] = { 0x51, 0x04, 0x03, 0x12, 0x00, 0x50, 0x00 };
	u16 val;
	int i;

	i2c_bimawen_tx_reset(&tx);
	bytes[6] = xor_buf(bytes, 6);
	for (i = 0; i < 7; i++)
		i2c_bimawen_tx_out_byte(&tx, &bytes[i]);
	if (bytes[3] != 0x10)
		return 1;
	if (i2c_bimawen_tx_take_luminance(&tx, &val))
		return 1;
	return 0;
}

static int test_byte_stream_lum_take(void)
{
	struct i2c_bimawen_tx tx;
	u8 bytes[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x50, 0x00 };
	u16 val;
	int i;

	i2c_bimawen_tx_reset(&tx);
	bytes[6] = xor_buf(bytes, 6);
	for (i = 0; i < 7; i++)
		i2c_bimawen_tx_out_byte(&tx, &bytes[i]);
	if (!i2c_bimawen_tx_take_luminance(&tx, &val) || val != 0x50)
		return 1;
	return 0;
}

static int test_sniff_set_block(void)
{
	u8 buf[] = { 0x51, 0x84, 0x03, 0x12, 0x00, 0x32, 0x00 };
	struct i2c_msg msg = { .addr = DDC_CI_ADDR, .len = 7, .buf = buf };
	u16 val;

	if (!i2c_bimawen_ddc_sniff_luminance(&msg, 1, false, NULL, &val) ||
	    val != 0x32)
		return 1;
	return 0;
}

static int test_sniff_get_block(void)
{
	u8 buf[16];
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 16, .buf = buf,
	};
	u16 val;

	memcpy(buf, (u8[]){ 0x6e, 0x08, 0x02, 0x00, 0x12, 0x64, 0x00, 0x00,
			    0x00, 0x32, 0x00 },
	       11);
	buf[10] = xor_buf(buf, 10) ^ 0x50;
	i2c_bimawen_ddc_munge_msgs(&msg, 1, true);
	if (!i2c_bimawen_ddc_sniff_luminance(&msg, 1, true, NULL, &val) ||
	    val != 0x32)
		return 1;
	return 0;
}

static u8 bl_chk(const u8 *m, int n)
{
	u8 x = DDC_CI_ADDR << 1;
	int i;

	for (i = 0; i < n; i++)
		x ^= m[i];
	return x;
}

static int test_bl_set_frame(void)
{
	u8 w[7] = { 0x51, 0x84, 0x03, 0x10, 0x00, 0x50, 0x00 };
	struct i2c_msg msg = { .addr = DDC_CI_ADDR, .len = 7, .buf = w };

	w[6] = bl_chk(w, 6);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, false);
	if (w[3] != 0x12 || w[6] != bl_chk(w, 6))
		return 1;
	return 0;
}

static int test_block_in_echo10(void)
{
	u8 buf[] = { 0x6e, 0x06, 0x02, 0x00, 0x10, 0x64, 0x00, 0x32, 0x00 };
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 9, .buf = buf,
	};

	buf[8] = xor_buf(buf, 8);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, true);
	return (buf[4] == 0x12 && i2c_bimawen_vcp_remap(buf[4]) == 0x10) ? 0 : 1;
}

static int test_block_in_padded(void)
{
	u8 buf[32];
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 32, .buf = buf,
	};

	memcpy(buf, (u8[]){ 0x6e, 0x06, 0x02, 0x00, 0x12, 0x64, 0x00,
			    0x32, 0x00 },
	       9);
	memset(buf + 9, 0x55, sizeof(buf) - 9);
	buf[8] = xor_buf(buf, 8);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, true);
	if (buf[4] != 0x10 || buf[8] != xor_buf(buf, 8))
		return 1;
	return buf[31] == 0x55 ? 0 : 1;
}

static int test_wr_rd_xfer(void)
{
	u8 wbuf[] = { 0x51, 0x82, 0x01, 0x10, 0x00 };
	u8 rbuf[32];
	struct i2c_msg msgs[2] = {
		{ .addr = DDC_CI_ADDR, .len = 5, .buf = wbuf },
		{ .addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 32, .buf = rbuf },
	};

	wbuf[4] = xor_buf(wbuf, 4);
	memcpy(rbuf, (u8[]){ 0x6e, 0x06, 0x02, 0x00, 0x12, 0x64, 0x00, 0x32, 0x00 },
	       9);
	memset(rbuf + 9, 0, sizeof(rbuf) - 9);
	rbuf[8] = xor_buf(rbuf, 8);
	i2c_bimawen_ddc_munge_msgs(msgs, 2, false);
	if (wbuf[3] != 0x12 || wbuf[4] != xor_buf(wbuf, 4))
		return 1;
	i2c_bimawen_ddc_munge_msgs(msgs, 2, true);
	return (rbuf[4] == 0x10 && rbuf[8] == xor_buf(rbuf, 8)) ? 0 : 1;
}

static int test_block_in_offset(void)
{
	u8 buf[32];
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 32, .buf = buf,
	};

	memset(buf, 0, 32);
	memcpy(buf + 3, (u8[]){ 0x6e, 0x06, 0x02, 0x00, 0x12, 0x64, 0x00,
				 0x32, 0x00 },
	       9);
	buf[11] = xor_buf(buf + 3, 8);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, true);
	return (buf[7] == 0x10 && buf[11] == xor_buf(buf + 3, 8)) ? 0 : 1;
}

static int test_block_in_two_frames(void)
{
	u8 buf[24];
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 24, .buf = buf,
	};
	u8 f1[] = { 0x6e, 0x06, 0x02, 0x00, 0x12, 0x64, 0x00, 0x32, 0x00 };
	u8 f2[] = { 0x6e, 0x06, 0x02, 0x00, 0x12, 0x64, 0x00, 0x28, 0x00 };

	memcpy(buf, f1, 9);
	buf[8] = xor_buf(buf, 8);
	memcpy(buf + 9, f2, 9);
	buf[17] = xor_buf(buf + 9, 8);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, true);
	return (buf[4] == 0x10 && buf[8] == xor_buf(buf, 8) &&
		buf[13] == 0x10 && buf[17] == xor_buf(buf + 9, 8)) ? 0 : 1;
}

static int test_outbound_restore(void)
{
	u8 buf[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x50, 0x00 };
	struct i2c_msg msg = { .addr = DDC_CI_ADDR, .len = 7, .buf = buf };
	u8 orig_vcp = buf[3];

	buf[6] = xor_buf(buf, 6);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, false);
	if (buf[3] == orig_vcp)
		return 1;
	i2c_bimawen_ddc_restore_outbound(&msg, 1, NULL);
	return (buf[3] == orig_vcp && buf[6] == xor_buf(buf, 6)) ? 0 : 1;
}

static int stream_frame(struct i2c_bimawen_tx *tx, u8 *bytes, int n)
{
	int i;

	for (i = 0; i < n; i++)
		i2c_bimawen_tx_out_byte(tx, &bytes[i]);
	return 0;
}

static int test_byte_stream_abort_preserve(void)
{
	struct i2c_bimawen_tx tx;
	u8 f1[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x40, 0x00 };
	u8 b;
	u8 sync = 0x51;
	int i;

	i2c_bimawen_tx_reset(&tx);
	f1[6] = xor_buf(f1, 6);
	stream_frame(&tx, f1, 7);
	for (i = 0; i < 4; i++) {
		b = (i == 0) ? 0x51 : (i == 1) ? 0x04 : (i == 2) ? 0x03 : 0x10;
		i2c_bimawen_tx_out_byte(&tx, &b);
	}
	i2c_bimawen_tx_out_byte(&tx, &sync);
	if (f1[3] != 0x12)
		return 1;
	i2c_bimawen_tx_undo_outbound(&tx);
	return f1[3] == 0x10 ? 0 : 1;
}

static int test_byte_stream_multi_restore(void)
{
	struct i2c_bimawen_tx tx;
	u8 f1[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x40, 0x00 };
	u8 f2[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x50, 0x00 };

	i2c_bimawen_tx_reset(&tx);
	f1[6] = xor_buf(f1, 6);
	f2[6] = xor_buf(f2, 6);
	stream_frame(&tx, f1, 7);
	stream_frame(&tx, f2, 7);
	if (f1[3] != 0x12 || f2[3] != 0x12)
		return 1;
	i2c_bimawen_tx_undo_outbound(&tx);
	return (f1[3] == 0x10 && f2[3] == 0x10 &&
		f1[6] == xor_buf(f1, 6) && f2[6] == xor_buf(f2, 6)) ? 0 : 1;
}

static int test_byte_value_51(void)
{
	struct i2c_bimawen_tx tx;
	u8 bytes[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x51, 0x51 };
	int i;

	i2c_bimawen_tx_reset(&tx);
	for (i = 0; i < 7; i++)
		i2c_bimawen_tx_out_byte(&tx, &bytes[i]);
	if (bytes[3] != 0x12 || bytes[5] != 0x51)
		return 1;
	return bytes[6] == (0x51 ^ 0x10 ^ 0x12) ? 0 : 1;
}

static int test_partial_undo(void)
{
	struct i2c_bimawen_tx tx;
	u8 bytes[] = { 0x51, 0x04, 0x03, 0x10, 0x00 };
	u16 val = 0;
	int i;

	i2c_bimawen_tx_reset(&tx);
	for (i = 0; i < 5; i++)
		i2c_bimawen_tx_out_byte(&tx, &bytes[i]);
	if (bytes[3] != 0x12)
		return 1;
	i2c_bimawen_tx_undo_outbound(&tx);
	if (bytes[3] != 0x10 || i2c_bimawen_tx_take_luminance(&tx, &val))
		return 1;
	return 0;
}

static int test_byte_stream_restore(void)
{
	struct i2c_bimawen_tx tx;
	u8 bytes[] = { 0x51, 0x04, 0x03, 0x10, 0x00, 0x50, 0x00 };
	u16 val;
	int i;

	i2c_bimawen_tx_reset(&tx);
	bytes[6] = xor_buf(bytes, 6);
	for (i = 0; i < 7; i++)
		i2c_bimawen_tx_out_byte(&tx, &bytes[i]);
	if (bytes[3] != 0x12)
		return 1;
	i2c_bimawen_tx_undo_outbound(&tx);
	if (i2c_bimawen_tx_take_luminance(&tx, &val))
		return 1;
	return (bytes[3] == 0x10 && bytes[6] == xor_buf(bytes, 6)) ? 0 : 1;
}

static u8 fake_edid[128];

static const u8 edid_hdr[8] = { 0x00, 0xff, 0xff, 0xff,
				0xff, 0xff, 0xff, 0x00 };

static void edid_fix_checksum(u8 *edid)
{
	unsigned sum = 0;
	int i;

	for (i = 0; i < 127; i++)
		sum += edid[i];
	edid[127] = (256U - (sum & 0xffU)) & 0xffU;
}

static void edid_hsj_base(void)
{
	memset(fake_edid, 0, 128);
	memcpy(fake_edid, edid_hdr, sizeof(edid_hdr));
	fake_edid[8] = 0x22;
	fake_edid[9] = 0x6a;
	fake_edid[10] = 0x33;
	fake_edid[11] = 0x01;
}

static int test_edid_block_ok(void)
{
	memset(fake_edid, 0, 128);
	memcpy(fake_edid, edid_hdr, sizeof(edid_hdr));
	if (i2c_bimawen_edid_header_ok(fake_edid, 128) &&
	    i2c_bimawen_edid_block_ok(fake_edid, 128))
		return 1;
	edid_hsj_base();
	edid_fix_checksum(fake_edid);
	if (!i2c_bimawen_edid_block_ok(fake_edid, 128))
		return 1;
	if (i2c_bimawen_edid_match_bimawen(fake_edid, 128, false))
		return 1;
	return 0;
}

static int test_parse_checksum(void)
{
	u8 buf[] = { 0x6e, 0x08, 0x02, 0x00, 0x10, 0x64, 0x00, 0x00, 0x00, 0x32, 0x00 };
	u16 val = 0;

	buf[10] = xor_buf(buf, 10);
	if (!i2c_bimawen_ddc_parse_get_vcp(buf, 11, VCP_LUMINANCE, &val, NULL))
		return 1;
	buf[10] ^= 0x50;
	if (i2c_bimawen_ddc_parse_get_vcp(buf, 11, VCP_LUMINANCE, &val, NULL) ||
	    val != 0x32)
		return 1;
	buf[4] = 0x12;
	buf[10] ^= 0x10 ^ 0x12;
	if (!i2c_bimawen_ddc_parse_get_vcp(buf, 11, VCP_LUMINANCE, &val, NULL))
		return 1;
	buf[4] = 0x10;
	buf[10] ^= 0x10 ^ 0x12;
	buf[9] ^= 1;
	return i2c_bimawen_ddc_parse_get_vcp(buf, 11, VCP_LUMINANCE, &val, NULL) ? 0 : 1;
}

static int test_truncated_no_corrupt(void)
{
	u8 w[] = { 0x51, 0x84, 0x03, 0x10, 0x00, 0x20 };
	u8 r[] = { 0x6e, 0x08, 0x02, 0x00, 0x12, 0x64, 0x00, 0x11 };
	struct i2c_msg mw = { .addr = DDC_CI_ADDR, .len = 6, .buf = w };
	struct i2c_msg mr = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 8, .buf = r,
	};

	i2c_bimawen_ddc_munge_msgs(&mw, 1, false);
	i2c_bimawen_ddc_munge_msgs(&mr, 1, true);
	if (w[3] != 0x10 || w[5] != 0x20)
		return 1;
	return (r[4] == 0x12 && r[7] == 0x11) ? 0 : 1;
}

static int test_munge_then_sniff(void)
{
	u8 bright[7] = { 0x51, 0x84, 0x03, 0x10, 0x00, 0x28, 0x00 };
	u8 contrast[7] = { 0x51, 0x84, 0x03, 0x12, 0x00, 0x28, 0x00 };
	struct i2c_msg mb = { .addr = DDC_CI_ADDR, .len = 7, .buf = bright };
	struct i2c_msg mc = { .addr = DDC_CI_ADDR, .len = 7, .buf = contrast };
	u16 val = 0;

	bright[6] = bl_chk(bright, 6);
	contrast[6] = bl_chk(contrast, 6);
	i2c_bimawen_ddc_munge_msgs(&mb, 1, false);
	if (!i2c_bimawen_ddc_sniff_luminance(&mb, 1, false, NULL, &val) || val != 0x28)
		return 1;
	i2c_bimawen_ddc_munge_msgs(&mc, 1, false);
	if (contrast[3] != 0x10)
		return 1;
	return i2c_bimawen_ddc_sniff_luminance(&mc, 1, false, NULL, &val) ? 1 : 0;
}

static int test_edid_bimawen(void)
{
	edid_hsj_base();
	fake_edid[0x39] = 0xfc;
	memcpy(fake_edid + 0x3b, "Bimawen", 8);
	edid_fix_checksum(fake_edid);
	if (!i2c_bimawen_edid_match_bimawen(fake_edid, 128, false))
		return 1;
	if (i2c_bimawen_edid_match_bimawen(fake_edid, 64, false))
		return 1;
	edid_hsj_base();
	if (i2c_bimawen_edid_match_bimawen(fake_edid, 128, false))
		return 1;
	if (!i2c_bimawen_edid_match_bimawen(fake_edid, 128, true))
		return 1;
	fake_edid[2] = 0x00;
	if (i2c_bimawen_edid_header_ok(fake_edid, 128))
		return 1;
	if (i2c_bimawen_edid_match_bimawen(fake_edid, 128, true))
		return 1;
	return 0;
}

static int test_inbound_not_munged(void)
{
	u8 buf[] = { 0xaa, 0x06, 0x02, 0x00, 0x12, 0x64, 0x00, 0x32, 0x00 };
	struct i2c_msg msg = {
		.addr = DDC_CI_ADDR, .flags = I2C_M_RD, .len = 9, .buf = buf,
	};

	buf[8] = xor_buf(buf, 8);
	i2c_bimawen_ddc_munge_msgs(&msg, 1, true);
	if (buf[4] != 0x12)
		return 1;
	return 0;
}

int main(void)
{
	if (i2c_bimawen_vcp_remap(0x10) != 0x12 || i2c_bimawen_vcp_remap(0x12) != 0x10)
		return 1;
	if (test_block_out())
		return 1;
	if (test_block_in())
		return 1;
	if (test_block_in_echo10())
		return 1;
	if (test_block_in_padded())
		return 1;
	if (test_wr_rd_xfer())
		return 1;
	if (test_block_in_offset())
		return 1;
	if (test_block_in_two_frames())
		return 1;
	if (test_byte_stream())
		return 1;
	if (test_byte_stream_contrast_no_lum())
		return 1;
	if (test_byte_stream_lum_take())
		return 1;
	if (test_sniff_set_block())
		return 1;
	if (test_sniff_get_block())
		return 1;
	if (test_bl_set_frame())
		return 1;
	if (test_inbound_not_munged())
		return 1;
	if (test_outbound_restore())
		return 1;
	if (test_byte_stream_restore())
		return 1;
	if (test_byte_stream_multi_restore())
		return 1;
	if (test_byte_stream_abort_preserve())
		return 1;
	if (test_byte_value_51())
		return 1;
	if (test_partial_undo())
		return 1;
	if (test_edid_bimawen())
		return 1;
	if (test_edid_block_ok())
		return 1;
	if (test_parse_checksum())
		return 1;
	if (test_truncated_no_corrupt())
		return 1;
	if (test_munge_then_sniff())
		return 1;
	puts("ok");
	return 0;
}