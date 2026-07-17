/*
 * h264poc - picture order count from a NAL scan, without pixel decoding.
 *
 * Why this exists: a seek must map a display index to a byte offset, and the
 * only random-access points frequent enough to make seeking fast on a 3D Blu-ray
 * are open-GOP recovery points, not IDRs (measured on a real disc: 103 IDRs vs
 * 341 recovery points over 7514 pictures). At an IDR, decode order and display
 * order agree, so counting pictures during the scan is enough. At an open-GOP
 * recovery point they do NOT: the recovery point's leading pictures follow it in
 * decode order but precede it in display order, so its display index is its
 * decode index plus the number of those leading pictures - a count only the
 * picture order count reveals. Deriving it is what lets a seek point sit on every
 * recovery point instead of only on IDRs.
 *
 * Scope is deliberately the minimum for that: the slice header is parsed only as
 * far as the POC fields, and only for base-view primary pictures. Anything this
 * cannot model (field coding, separate colour planes, an unknown parameter set)
 * makes the parse fail, and the caller falls back to IDR-only seek points - the
 * previously shipped, proven behaviour. Split into its own header so a test can
 * compile it standalone (see tests/h264poctest.c), matching cache_budget.h.
 *
 * Conformance: this models H.264 8.2.1 for pic_order_cnt_type 0/1/2, but NOT the
 * MMCO5 POC reset (reaching dec_ref_pic_marking would mean parsing the whole
 * ref_pic_list_modification and pred_weight_table). An MMCO5 stream therefore
 * gets wrong POC values here; the caller must reject a POC set that is not unique
 * within a coded video sequence, which is how MMCO5 shows up (the reset collides
 * with an earlier POC). Verified: that guard is what catches MR4_TANDBERG_C in
 * the JVT conformance corpus, and it is the only stream of 71 that needs it.
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#ifndef H264POC_H
#define H264POC_H

#include <stdint.h>
#include <string.h>

/* --- bit reader over a de-emulated RBSP ----------------------------------- */

typedef struct { const uint8_t *d; int nbytes, pos, err; } H264Bits;

static inline void h264_bits_init(H264Bits *b, const uint8_t *d, int nbytes) {
	b->d = d; b->nbytes = nbytes; b->pos = 0; b->err = 0;
}

/* Every read past the end sets err and yields 0, so a truncated/corrupt header
 * degrades to a failed parse (and an IDR-only fallback) rather than reading on. */
static inline int h264_u1(H264Bits *b) {
	if (b->pos >= b->nbytes * 8) { b->err = 1; return 0; }
	int v = (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
	b->pos++;
	return v;
}

static inline uint32_t h264_un(H264Bits *b, int bits) {
	uint32_t v = 0;
	for (int i = 0; i < bits; i++) v = (v << 1) | (uint32_t)h264_u1(b);
	return v;
}

static inline uint32_t h264_ue(H264Bits *b) {
	int lz = 0;
	while (!h264_u1(b)) {
		/* >31 leading zeros cannot encode a valid ue(v) in a conforming stream and
		 * would overflow the shift below; treat it as a corrupt header. */
		if (b->err || ++lz > 31) { b->err = 1; return 0; }
	}
	if (lz == 0) return 0;
	return ((1u << lz) - 1) + h264_un(b, lz);
}

static inline int32_t h264_se(H264Bits *b) {
	uint32_t k = h264_ue(b);
	return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

/* Copy a NAL's payload (past the one-byte header) into buf, dropping the
 * emulation_prevention_three_byte. Returns the byte count written; a header
 * longer than cap simply yields a short buffer, which the readers above turn
 * into a parse error. */
static inline int h264_rbsp(const uint8_t *nal, const uint8_t *end, uint8_t *buf, int cap) {
	int n = 0, zeros = 0;
	for (const uint8_t *q = nal + 1; q < end && n < cap; q++) {
		if (zeros >= 2 && *q == 3) { zeros = 0; continue; }
		buf[n++] = *q;
		zeros = (*q == 0) ? zeros + 1 : 0;
	}
	return n;
}

/* --- parameter sets ------------------------------------------------------- */

#define H264_MAX_SPS 32   /* seq_parameter_set_id is ue(v) in [0,31]  */
#define H264_MAX_PPS 256  /* pic_parameter_set_id is ue(v) in [0,255] */
#define H264_MAX_POC_CYCLE 256

typedef struct {
	int valid;
	int log2_max_frame_num;
	int poc_type;
	int log2_max_poc_lsb;          /* poc_type 0 */
	int delta_poc_always_zero;     /* poc_type 1 */
	int32_t offset_for_non_ref_pic, offset_for_top_to_bottom_field;
	int num_ref_frames_in_poc_cycle;
	int32_t offset_for_ref_frame[H264_MAX_POC_CYCLE];
	int frame_mbs_only;
	int separate_colour_plane;
} H264Sps;

typedef struct { int valid, sps_id, bottom_field_poc_present; } H264Pps;

/*
 * Parser state for one stream. Holds the active parameter sets plus the POC
 * derivation state, so one context tracks one linear pass over the stream. Big
 * enough (~35 KB, mostly offset_for_ref_frame) that callers should heap-allocate
 * it rather than put it on a frameserver's worker stack.
 */
typedef struct {
	H264Sps sps[H264_MAX_SPS];
	H264Pps pps[H264_MAX_PPS];
	/* POC derivation state (H.264 8.2.1), carried across pictures */
	int32_t prev_poc_msb, prev_poc_lsb;  /* poc_type 0 */
	int prev_frame_num, frame_num_offset; /* poc_type 1/2 */
} H264PocCtx;

static inline void h264_poc_ctx_init(H264PocCtx *c) { memset(c, 0, sizeof *c); }

/* Reset the POC derivation state. The caller invokes this at every IDR, which
 * starts a new coded video sequence (POC restarts at 0). */
static inline void h264_poc_reset(H264PocCtx *c) {
	c->prev_poc_msb = c->prev_poc_lsb = 0;
	c->prev_frame_num = c->frame_num_offset = 0;
}

/* Scaling lists are skipped, not stored - they sit between the profile block and
 * the POC fields, so the bit position must still advance exactly. */
static inline void h264_skip_scaling_list(H264Bits *b, int size) {
	int last = 8, next = 8;
	for (int j = 0; j < size; j++) {
		if (next) next = (last + h264_se(b) + 256) % 256;
		last = next ? next : last;
	}
}

/*
 * Parse an SPS (NAL type 7) into the context. Only type 7 must be fed here, never
 * a subset SPS (type 15): the two share the seq_parameter_set_id numbering but
 * are distinct sets in the spec, so letting an MVC subset SPS land in the same
 * slot would overwrite the base view's SPS and mis-derive every later POC.
 */
static inline void h264_parse_sps(H264PocCtx *c, const uint8_t *nal, const uint8_t *end) {
	uint8_t buf[2048];
	int n = h264_rbsp(nal, end, buf, sizeof buf);
	H264Bits b;
	h264_bits_init(&b, buf, n);
	int profile = (int)h264_un(&b, 8);
	h264_un(&b, 8);   /* constraint_set flags + reserved_zero_2bits */
	h264_un(&b, 8);   /* level_idc */
	uint32_t id = h264_ue(&b);
	if (b.err || id >= H264_MAX_SPS) return;
	H264Sps s;
	memset(&s, 0, sizeof s);
	/* The high-profile block is present only for these profile_idc values (7.3.2.1.1) */
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
	    profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
	    profile == 139 || profile == 134 || profile == 135) {
		uint32_t cfi = h264_ue(&b);   /* chroma_format_idc */
		if (cfi == 3) s.separate_colour_plane = h264_u1(&b);
		h264_ue(&b);   /* bit_depth_luma_minus8 */
		h264_ue(&b);   /* bit_depth_chroma_minus8 */
		h264_u1(&b);   /* qpprime_y_zero_transform_bypass_flag */
		if (h264_u1(&b))   /* seq_scaling_matrix_present_flag */
			for (int i = 0; i < ((cfi != 3) ? 8 : 12); i++)
				if (h264_u1(&b)) h264_skip_scaling_list(&b, i < 6 ? 16 : 64);
	}
	s.log2_max_frame_num = (int)h264_ue(&b) + 4;
	s.poc_type = (int)h264_ue(&b);
	if (s.poc_type == 0) {
		s.log2_max_poc_lsb = (int)h264_ue(&b) + 4;
		/* log2_max_pic_order_cnt_lsb_minus4 is [0,12] (7.4.2.1.1); a larger value
		 * would make h264_un read a nonsense width, so reject the SPS instead. */
		if (s.log2_max_poc_lsb > 16) return;
	} else if (s.poc_type == 1) {
		s.delta_poc_always_zero = h264_u1(&b);
		s.offset_for_non_ref_pic = h264_se(&b);
		s.offset_for_top_to_bottom_field = h264_se(&b);
		s.num_ref_frames_in_poc_cycle = (int)h264_ue(&b);
		if (s.num_ref_frames_in_poc_cycle > H264_MAX_POC_CYCLE) return;
		for (int i = 0; i < s.num_ref_frames_in_poc_cycle; i++)
			s.offset_for_ref_frame[i] = h264_se(&b);
	} else if (s.poc_type != 2) {
		return;   /* only 0/1/2 exist; anything else is corrupt */
	}
	h264_ue(&b);   /* max_num_ref_frames */
	h264_u1(&b);   /* gaps_in_frame_num_value_allowed_flag */
	h264_ue(&b);   /* pic_width_in_mbs_minus1 */
	h264_ue(&b);   /* pic_height_in_map_units_minus1 */
	s.frame_mbs_only = h264_u1(&b);
	if (b.err || s.log2_max_frame_num > 16) return;
	s.valid = 1;
	c->sps[id] = s;
}

/* Parse a PPS (NAL type 8). Only the first four fields matter here: the id, the
 * SPS it selects, and bottom_field_pic_order_in_frame_present_flag, which decides
 * whether delta_pic_order_cnt_bottom sits in the slice header. */
static inline void h264_parse_pps(H264PocCtx *c, const uint8_t *nal, const uint8_t *end) {
	uint8_t buf[256];
	int n = h264_rbsp(nal, end, buf, sizeof buf);
	H264Bits b;
	h264_bits_init(&b, buf, n);
	uint32_t id = h264_ue(&b);
	uint32_t sid = h264_ue(&b);
	if (b.err || id >= H264_MAX_PPS || sid >= H264_MAX_SPS) return;
	h264_u1(&b);   /* entropy_coding_mode_flag */
	int bfp = h264_u1(&b);
	if (b.err) return;
	c->pps[id].valid = 1;
	c->pps[id].sps_id = (int)sid;
	c->pps[id].bottom_field_poc_present = bfp;
}

/* --- slice header + POC --------------------------------------------------- */

typedef struct {
	int is_idr;     /* NAL type 5 */
	int is_intra;   /* slice_type is I: the picture can start an open GOP */
	int32_t poc;    /* picture order count (TopFieldOrderCnt for a frame) */
} H264PicInfo;

/*
 * Parse the first slice of a base-view primary picture (NAL type 1 or 5) as far
 * as the POC fields and derive the picture's POC per H.264 8.2.1. `nal` points at
 * the NAL header byte. Returns 1 with *out filled, or 0 if this is not a first
 * slice or the stream uses something outside this parser's scope (field coding,
 * separate colour planes, an unknown/invalid parameter set) - the caller then
 * falls back to IDR-only seek points.
 *
 * Must be called for every base primary picture in decode order and nowhere else:
 * the POC derivation is a state machine over that exact sequence.
 */
static inline int h264_parse_picture(H264PocCtx *c, const uint8_t *nal, const uint8_t *end,
	H264PicInfo *out) {
	uint8_t buf[64];   /* the POC fields sit well inside the first bytes of the header */
	int n = h264_rbsp(nal, end, buf, sizeof buf);
	H264Bits b;
	h264_bits_init(&b, buf, n);
	memset(out, 0, sizeof *out);
	int nal_ref_idc = (nal[0] >> 5) & 3;
	out->is_idr = (nal[0] & 0x1f) == 5;

	if (h264_ue(&b) != 0) return 0;   /* first_mb_in_slice != 0: not a picture's first slice */
	uint32_t slice_type = h264_ue(&b);
	/* slice_type 0..4 = P/B/I/SP/SI, and 5..9 repeat those for an all-same-type
	 * picture, so %5==2 is I either way. */
	out->is_intra = (slice_type % 5 == 2);
	uint32_t pps_id = h264_ue(&b);
	if (b.err || pps_id >= H264_MAX_PPS || !c->pps[pps_id].valid) return 0;
	const H264Sps *s = &c->sps[c->pps[pps_id].sps_id];
	if (!s->valid || s->separate_colour_plane) return 0;

	int frame_num = (int)h264_un(&b, s->log2_max_frame_num);
	if (!s->frame_mbs_only) {
		/* field_pic_flag: a field-coded stream has two pictures per frame, which
		 * breaks the caller's one-picture-per-frame count outright. Bail. */
		if (h264_u1(&b)) return 0;
	}
	if (out->is_idr) h264_ue(&b);   /* idr_pic_id */

	if (s->poc_type == 0) {
		int poc_lsb = (int)h264_un(&b, s->log2_max_poc_lsb);
		if (b.err) return 0;
		int max = 1 << s->log2_max_poc_lsb;
		if (out->is_idr) { c->prev_poc_msb = 0; c->prev_poc_lsb = 0; }
		int32_t msb;
		if (poc_lsb < c->prev_poc_lsb && (c->prev_poc_lsb - poc_lsb) >= max / 2)
			msb = c->prev_poc_msb + max;
		else if (poc_lsb > c->prev_poc_lsb && (poc_lsb - c->prev_poc_lsb) > max / 2)
			msb = c->prev_poc_msb - max;
		else
			msb = c->prev_poc_msb;
		out->poc = msb + poc_lsb;
		/* only a reference picture advances the prev* pair (8.2.1.1) */
		if (nal_ref_idc) { c->prev_poc_msb = msb; c->prev_poc_lsb = poc_lsb; }
		return 1;
	}

	int max_fn = 1 << s->log2_max_frame_num;
	if (out->is_idr) { c->frame_num_offset = 0; c->prev_frame_num = 0; }
	else if (c->prev_frame_num > frame_num) c->frame_num_offset += max_fn;   /* frame_num wrapped */

	if (s->poc_type == 2) {
		/* 8.2.1.3: display order == decode order; a non-reference picture sits just
		 * before its reference counterpart. */
		out->poc = 2 * (c->frame_num_offset + frame_num) - (nal_ref_idc ? 0 : 1);
		c->prev_frame_num = frame_num;
		return b.err ? 0 : 1;
	}

	/* poc_type 1 (8.2.1.2) */
	int32_t delta_poc0 = 0;
	if (!s->delta_poc_always_zero) {
		delta_poc0 = h264_se(&b);
		if (c->pps[pps_id].bottom_field_poc_present) h264_se(&b);   /* delta_pic_order_cnt[1] */
	}
	if (b.err) return 0;
	int32_t abs_fn = s->num_ref_frames_in_poc_cycle ? c->frame_num_offset + frame_num : 0;
	if (!nal_ref_idc && abs_fn > 0) abs_fn--;
	int32_t expected = 0;
	if (abs_fn > 0 && s->num_ref_frames_in_poc_cycle > 0) {
		int32_t cycle = (abs_fn - 1) / s->num_ref_frames_in_poc_cycle;
		int32_t idx = (abs_fn - 1) % s->num_ref_frames_in_poc_cycle;
		int32_t sum = 0;
		for (int i = 0; i < s->num_ref_frames_in_poc_cycle; i++) sum += s->offset_for_ref_frame[i];
		expected = cycle * sum;
		for (int i = 0; i <= idx; i++) expected += s->offset_for_ref_frame[i];
	}
	if (!nal_ref_idc) expected += s->offset_for_non_ref_pic;
	out->poc = expected + delta_poc0;
	c->prev_frame_num = frame_num;
	return 1;
}

/* --- recovery point SEI --------------------------------------------------- */

/*
 * True if this SEI NAL (type 6) carries a recovery_point message (payloadType 6)
 * with recovery_frame_cnt == 0: the access unit it precedes is an exact open-GOP
 * random-access point, which is what Blu-ray uses for entry points between IDRs.
 * Pictures from it onward *in display order* are guaranteed correct on a cold
 * decode; its leading pictures are not, which is precisely what the POC-derived
 * display index above accounts for.
 */
static inline int h264_sei_is_recovery_point(const uint8_t *nal, const uint8_t *end) {
	uint8_t buf[512];
	int n = h264_rbsp(nal, end, buf, sizeof buf);
	int i = 0;
	while (i < n) {
		int type = 0;
		while (i < n && buf[i] == 0xff) { type += 255; i++; }
		if (i >= n) break;
		type += buf[i++];
		int size = 0;
		while (i < n && buf[i] == 0xff) { size += 255; i++; }
		if (i >= n) break;
		size += buf[i++];
		if (type == 6) {   /* recovery_point: recovery_frame_cnt is the leading ue(v) */
			H264Bits b;
			h264_bits_init(&b, buf + i, n - i);
			return h264_ue(&b) == 0 && !b.err;
		}
		i += size;
	}
	return 0;
}

#endif
