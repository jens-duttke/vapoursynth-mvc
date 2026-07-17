/*
 * h264poctest - standalone unit test for the POC parser (src/h264poc.h).
 *
 * The parser is what lets a seek point sit on an open-GOP recovery point: it
 * derives the picture order count from the slice headers, and from that the
 * recovery point's display index. Getting it wrong serves a wrong frame, so the
 * pieces are pinned here directly rather than only through a decode.
 *
 * Deliberately needs no bitstream and no decoder - the header compiles standalone
 * (like cache_budget.h), so this test builds from hand-written bits alone.
 *
 * usage: h264poctest
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "h264poc.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void check(int cond, const char *what) {
	if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static void check_eq(int got, int want, const char *what) {
	if (got != want) { printf("FAIL: %s (got %d, want %d)\n", what, got, want); fails++; }
}

/* --- bit reader ----------------------------------------------------------- */

static void test_bits(void) {
	/* ue(v): '1'=0, '010'=1, '011'=2, '00100'=3 ... (9.1) */
	const uint8_t d[] = { 0xA6 }; /* 1 010 011 0 -> ue=0, ue=1, ue=2 */
	H264Bits b;
	h264_bits_init(&b, d, sizeof d);
	check_eq((int)h264_ue(&b), 0, "ue(v) '1' == 0");
	check_eq((int)h264_ue(&b), 1, "ue(v) '010' == 1");
	check_eq((int)h264_ue(&b), 2, "ue(v) '011' == 2");

	/* se(v) maps 0,1,2,3,4 -> 0,1,-1,2,-2 (9.1.1) */
	const uint8_t s[] = { 0xA6 };
	h264_bits_init(&b, s, sizeof s);
	check_eq(h264_se(&b), 0, "se(v) k=0 == 0");
	check_eq(h264_se(&b), 1, "se(v) k=1 == 1");
	check_eq(h264_se(&b), -1, "se(v) k=2 == -1");

	/* reading past the end must set err rather than run on into whatever follows */
	const uint8_t z[] = { 0x00 };
	h264_bits_init(&b, z, sizeof z);
	h264_ue(&b);
	check(b.err, "ue(v) past the buffer sets err");

	/* a run of >31 zeros cannot be a valid ue(v) and must not overflow the shift */
	const uint8_t all0[8] = { 0 };
	h264_bits_init(&b, all0, sizeof all0);
	h264_ue(&b);
	check(b.err, "ue(v) with >31 leading zeros sets err");
}

/* --- emulation prevention ------------------------------------------------- */

static void test_rbsp(void) {
	/* 00 00 03 01 -> the 03 is an emulation_prevention_three_byte and must go;
	 * the leading byte is the NAL header, which h264_rbsp skips. */
	const uint8_t nal[] = { 0x67, 0x00, 0x00, 0x03, 0x01, 0x02 };
	uint8_t buf[8];
	int n = h264_rbsp(nal, nal + sizeof nal, buf, sizeof buf);
	check_eq(n, 4, "rbsp drops the emulation_prevention_three_byte");
	check(n == 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 2,
	      "rbsp keeps the surrounding bytes intact");

	/* 00 00 03 03 -> only the first 03 is stripped: the second is real data */
	const uint8_t twice[] = { 0x67, 0x00, 0x00, 0x03, 0x03 };
	n = h264_rbsp(twice, twice + sizeof twice, buf, sizeof buf);
	check_eq(n, 3, "rbsp strips only the escape, not the escaped byte");

	/* a payload longer than cap yields a short buffer, which the readers turn
	 * into a parse error rather than reading out of bounds */
	const uint8_t big[16] = { 0x67, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	uint8_t small[4];
	n = h264_rbsp(big, big + sizeof big, small, sizeof small);
	check_eq(n, 4, "rbsp never writes past cap");
}

/* --- parameter set ids ---------------------------------------------------- */

static void test_param_set_id(void) {
	/* PPS leads with pic_parameter_set_id: header byte, then ue(v)=0, ue(v)=0 */
	const uint8_t pps[] = { 0x68, 0xA0 }; /* '1' '010...' -> id 0 */
	check_eq(h264_param_set_id(pps, pps + sizeof pps, 8), 0, "PPS id reads the leading ue(v)");

	/* SPS puts the id behind profile_idc / constraints / level_idc (3 bytes) */
	const uint8_t sps[] = { 0x67, 100, 0x00, 40, 0xA0 }; /* ue(v)='1' -> id 0 */
	check_eq(h264_param_set_id(sps, sps + sizeof sps, 7), 0, "SPS id skips the 3 fixed bytes");

	const uint8_t sps1[] = { 0x67, 100, 0x00, 40, 0x40 }; /* '010' -> id 1 */
	check_eq(h264_param_set_id(sps1, sps1 + sizeof sps1, 7), 1, "SPS id 1");

	/* a subset SPS is laid out like an SPS for this purpose */
	const uint8_t sub[] = { 0x6F, 128, 0x00, 40, 0x40 };
	check_eq(h264_param_set_id(sub, sub + sizeof sub, 15), 1, "subset SPS id skips the 3 fixed bytes too");

	/* truncated: must report -1, so refeed_param_sets keeps feeding it rather
	 * than dropping a set a later slice needs */
	const uint8_t trunc[] = { 0x67 };
	check_eq(h264_param_set_id(trunc, trunc + sizeof trunc, 7), -1, "unreadable id reports -1");
}

/* --- POC derivation ------------------------------------------------------- */

/* Build the smallest SPS h264_parse_sps accepts: baseline profile (no high-profile
 * block), the given pic_order_cnt_type, log2_max_pic_order_cnt_lsb_minus4 = 0 (so
 * MaxPicOrderCntLsb is 16), frame_mbs_only = 1. */
static int make_sps(uint8_t *out, int poc_type) {
	/* profile_idc=66, constraints=0, level_idc=30, then the RBSP bits:
	 *   seq_parameter_set_id           ue(v) = 0            -> 1
	 *   log2_max_frame_num_minus4      ue(v) = 0            -> 1
	 *   pic_order_cnt_type             ue(v) = poc_type     -> 1 / 010 / 011
	 *   [poc_type 0] log2_max_poc_lsb_minus4 ue(v) = 0      -> 1
	 *   max_num_ref_frames             ue(v) = 0            -> 1
	 *   gaps_in_frame_num_allowed      u(1)  = 0            -> 0
	 *   pic_width_in_mbs_minus1        ue(v) = 0            -> 1
	 *   pic_height_in_map_units_minus1 ue(v) = 0            -> 1
	 *   frame_mbs_only_flag            u(1)  = 1            -> 1
	 * (direct_8x8 / cropping / VUI follow but are never read) */
	int n = 0;
	out[n++] = 0x67;
	out[n++] = 66;
	out[n++] = 0x00;
	out[n++] = 30;
	/* assemble the bit string by hand */
	const char *bits;
	if (poc_type == 0)      bits = "1" "1" "1"   "1" "1" "0" "1" "1" "1";
	else if (poc_type == 2) bits = "1" "1" "011"     "1" "0" "1" "1" "1";
	else                    return 0; /* poc_type 1 needs a cycle; not built here */
	uint8_t acc = 0;
	int nb = 0;
	for (const char *p = bits; *p; p++) {
		acc = (uint8_t)((acc << 1) | (*p == '1'));
		if (++nb == 8) { out[n++] = acc; acc = 0; nb = 0; }
	}
	if (nb) out[n++] = (uint8_t)(acc << (8 - nb));
	return n;
}

/* PPS: pic_parameter_set_id=0, seq_parameter_set_id=0, entropy_coding_mode=0,
 * bottom_field_pic_order_in_frame_present=0 -> bits '1' '1' '0' '0' */
static int make_pps(uint8_t *out) {
	out[0] = 0x68;
	out[1] = 0xC0; /* 1 1 0 0 .... */
	return 2;
}

/* A base-view slice: first_mb_in_slice=0, slice_type, pic_parameter_set_id=0,
 * frame_num (4 bits), [idr_pic_id], pic_order_cnt_lsb (4 bits). */
static int make_slice(uint8_t *out, int idr, int slice_type, int frame_num, int poc_lsb, int ref) {
	out[0] = (uint8_t)((ref ? 0x60 : 0x00) | (idr ? 5 : 1));
	char bits[64];
	int n = 0;
	bits[n++] = '1';                                   /* first_mb_in_slice = 0  */
	if (slice_type == 2) { bits[n++] = '0'; bits[n++] = '1'; bits[n++] = '1'; } /* ue=2 (I) */
	else                 { bits[n++] = '0'; bits[n++] = '1'; bits[n++] = '0'; } /* ue=1 (B) */
	bits[n++] = '1';                                   /* pic_parameter_set_id=0 */
	for (int i = 3; i >= 0; i--) bits[n++] = (frame_num >> i) & 1 ? '1' : '0';  /* frame_num u(4) */
	if (idr) bits[n++] = '1';                          /* idr_pic_id = 0         */
	for (int i = 3; i >= 0; i--) bits[n++] = (poc_lsb >> i) & 1 ? '1' : '0';    /* poc_lsb u(4) */
	uint8_t acc = 0;
	int nb = 0, o = 1;
	for (int i = 0; i < n; i++) {
		acc = (uint8_t)((acc << 1) | (bits[i] == '1'));
		if (++nb == 8) { out[o++] = acc; acc = 0; nb = 0; }
	}
	if (nb) out[o++] = (uint8_t)(acc << (8 - nb));
	return o;
}

static void test_poc_type0(void) {
	H264PocCtx c;
	h264_poc_ctx_init(&c);
	uint8_t sps[16], pps[4], sl[16];
	int n;
	n = make_sps(sps, 0);
	h264_parse_sps(&c, sps, sps + n);
	check(c.sps[0].valid, "SPS parses");
	check_eq(c.sps[0].poc_type, 0, "poc_type 0 read back");
	check_eq(c.sps[0].log2_max_poc_lsb, 4, "log2_max_poc_lsb == 4 (MaxPicOrderCntLsb 16)");
	n = make_pps(pps);
	h264_parse_pps(&c, pps, pps + n);
	check(c.pps[0].valid, "PPS parses");

	H264PicInfo pi;
	/* an IDR resets the count: POC 0 */
	n = make_slice(sl, 1, 2, 0, 0, 1);
	check(h264_parse_picture(&c, sl, sl + n, &pi), "IDR slice parses");
	check(pi.is_idr && pi.is_intra, "IDR is flagged idr + intra");
	check_eq(pi.poc, 0, "IDR POC == 0");

	/* a following reference picture with poc_lsb 8 */
	n = make_slice(sl, 0, 2, 1, 8, 1);
	check(h264_parse_picture(&c, sl, sl + n, &pi), "non-IDR slice parses");
	check(!pi.is_idr, "non-IDR is not flagged idr");
	check_eq(pi.poc, 8, "POC follows pic_order_cnt_lsb");

	/* the lsb wraps (8 -> 2 with MaxPicOrderCntLsb 16 is a wrap, not a jump back):
	 * prevLsb=8, lsb=2, 8-2=6 < 16/2, so no msb step - still 2. */
	n = make_slice(sl, 0, 1, 2, 2, 1);
	check(h264_parse_picture(&c, sl, sl + n, &pi), "wrap-candidate slice parses");
	check_eq(pi.poc, 2, "lsb 8 -> 2 is a step back within the window, not a wrap");

	/* now a real wrap: prevLsb=2, lsb=15 -> 15-2=13 > 16/2, so msb -= 16 -> -1 */
	n = make_slice(sl, 0, 1, 3, 15, 1);
	check(h264_parse_picture(&c, sl, sl + n, &pi), "wrapping slice parses");
	check_eq(pi.poc, -1, "lsb 2 -> 15 wraps the msb backwards");
}

static void test_poc_type2(void) {
	/* poc_type 2: display order == decode order, POC = 2*frame_num for a
	 * reference picture and one less for a non-reference one (8.2.1.3) */
	H264PocCtx c;
	h264_poc_ctx_init(&c);
	uint8_t sps[16], pps[4], sl[16];
	int n = make_sps(sps, 2);
	h264_parse_sps(&c, sps, sps + n);
	check_eq(c.sps[0].poc_type, 2, "poc_type 2 read back");
	n = make_pps(pps);
	h264_parse_pps(&c, pps, pps + n);

	H264PicInfo pi;
	n = make_slice(sl, 1, 2, 0, 0, 1);
	check(h264_parse_picture(&c, sl, sl + n, &pi), "poc_type 2 IDR parses");
	check_eq(pi.poc, 0, "poc_type 2 IDR POC == 0");

	n = make_slice(sl, 0, 2, 3, 0, 1);
	check(h264_parse_picture(&c, sl, sl + n, &pi), "poc_type 2 ref parses");
	check_eq(pi.poc, 6, "poc_type 2 reference POC == 2*frame_num");

	n = make_slice(sl, 0, 1, 4, 0, 0);
	check(h264_parse_picture(&c, sl, sl + n, &pi), "poc_type 2 non-ref parses");
	check_eq(pi.poc, 7, "poc_type 2 non-reference POC == 2*frame_num - 1");
}

static void test_unknown_param_set(void) {
	/* a slice naming a PPS that was never parsed must fail, not guess: the caller
	 * then falls back to IDR-only seek points rather than trusting a bad POC */
	H264PocCtx c;
	h264_poc_ctx_init(&c);
	uint8_t sl[16];
	int n = make_slice(sl, 1, 2, 0, 0, 1);
	H264PicInfo pi;
	check(!h264_parse_picture(&c, sl, sl + n, &pi), "slice with no PPS fails the parse");
}

/* --- recovery point SEI --------------------------------------------------- */

static void test_recovery_point(void) {
	/* SEI payloadType 6 (recovery_point), payloadSize 2, recovery_frame_cnt=0 as
	 * the leading ue(v) ('1' -> 0x80) */
	const uint8_t rp[] = { 0x06, 6, 2, 0x80, 0x80 };
	check(h264_sei_is_recovery_point(rp, rp + sizeof rp), "recovery_point with rfc 0 detected");

	/* rfc != 0 is not an exact random-access point: '010' -> 1 */
	const uint8_t rp1[] = { 0x06, 6, 2, 0x40, 0x80 };
	check(!h264_sei_is_recovery_point(rp1, rp1 + sizeof rp1), "recovery_point with rfc != 0 rejected");

	/* some other SEI (payloadType 1, pic_timing) must not be mistaken for one */
	const uint8_t pt[] = { 0x06, 1, 2, 0x00, 0x00, 0x80 };
	check(!h264_sei_is_recovery_point(pt, pt + sizeof pt), "a non-recovery SEI is not a recovery point");

	/* recovery_point behind another message: the walk must skip by payloadSize */
	const uint8_t after[] = { 0x06, 1, 2, 0x00, 0x00, 6, 2, 0x80, 0x80 };
	check(h264_sei_is_recovery_point(after, after + sizeof after),
	      "recovery_point found behind an earlier SEI message");
}

int main(void) {
	test_bits();
	test_rbsp();
	test_param_set_id();
	test_poc_type0();
	test_poc_type2();
	test_unknown_param_set();
	test_recovery_point();
	if (fails) { printf("RESULT: FAIL (%d)\n", fails); return 1; }
	printf("RESULT: PASS (POC parser: bit reader, RBSP, ids, poc_type 0/2, recovery point)\n");
	return 0;
}
