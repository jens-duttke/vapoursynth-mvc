/*
 * seektest - regression test for random-access seeking on streams whose access
 * units are NOT each preceded by their own parameter sets.
 *
 * From the committed multi-GOP fixture (SPS/PPS repeated per GOP) it derives, in
 * memory, two harder NAL topologies and drives the decode core through them:
 *
 *   headerless : SPS/PPS once at the front, no non-VCL NAL between access units
 *                (the shape of a default x264 raw elementary stream). If
 *                scan_index freezes the access-unit start at a boundary that has
 *                moved, later seek points carry the wrong byte offset and a
 *                backward seek silently returns the wrong frame. [P4-H-1]
 *
 *   aud_once   : an AUD before every access unit, SPS/PPS only at the front.
 *                The access-unit start is tracked correctly, but a freshly
 *                recreated decoder is re-fed from a seek point that lacks SPS/PPS
 *                and every slice fails ("unexpected end of stream"). [P4-M-1]
 *
 *   cut        : the stream cut mid-GOP so it begins with VCL slices that precede
 *                any SPS/PPS (the Alba.264 case). The decoder returns EBADMSG for
 *                those (no frame), so scan_index must not count them; counting
 *                them overcounts num_frames, shifting every later display index
 *                and leaving the tail unreadable. Checked both ways: seek ==
 *                sequential, and num_frames is an exact frame-suffix of the full
 *                stream.
 *
 * The check is the project's core invariant: reading a frame after a seek must
 * be bit-identical to reading it in sequence. Reading every frame in reverse
 * order forces a backward seek onto each seek point in turn.
 *
 * usage: seektest <base_multigop.264>
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- minimal Annex-B NAL walk (independent of the code under test) --------- */

/* Find the next 00 00 01 start-code prefix at or after p; return end if none. */
static const uint8_t *find_sc(const uint8_t *p, const uint8_t *end) {
	for (const uint8_t *q = p; q + 2 < end; q++)
		if (q[0] == 0 && q[1] == 0 && q[2] == 1) return q;
	return end;
}

static const uint8_t SC4[4] = { 0, 0, 0, 1 };
static const uint8_t AUD[6]  = { 0, 0, 0, 1, 0x09, 0x10 }; /* access unit delimiter */

typedef struct { uint8_t *p; size_t len, cap; } Buf;
static void put(Buf *b, const uint8_t *d, size_t n) {
	if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
	memcpy(b->p + b->len, d, n); b->len += n;
}

/* Rebuild `src` keeping only the first SPS and first PPS at the front, then all
 * VCL NALs in order. If `aud` is set, prefix every coded picture with an AUD. */
static Buf synth(const uint8_t *src, size_t n, int aud) {
	Buf head = {0}, body = {0};
	const uint8_t *end = src + n;
	const uint8_t *p = find_sc(src, end);
	int got_sps = 0, got_pps = 0;
	while (p < end) {
		const uint8_t *pay = p + 3;                 /* NAL header byte */
		const uint8_t *nx = find_sc(pay, end);
		size_t plen = (size_t)(nx - pay);
		if (plen >= 1) {
			int type = pay[0] & 0x1f;
			int vcl = (type >= 1 && type <= 5) || type == 19 || type == 20;
			if (type == 7 && !got_sps) { put(&head, SC4, 4); put(&head, pay, plen); got_sps = 1; }
			else if (type == 8 && !got_pps) { put(&head, SC4, 4); put(&head, pay, plen); got_pps = 1; }
			else if (vcl) {
				int first = plen >= 2 && (pay[1] & 0x80);
				if (aud && (type == 1 || type == 5) && first) put(&body, AUD, sizeof AUD);
				put(&body, SC4, 4); put(&body, pay, plen);
			}
		}
		p = nx;
	}
	put(&head, body.p, body.len); free(body.p);
	return head;
}

/* Byte offset of the start code of the first non-IDR access unit that follows the
 * first IDR. Cutting the stream here makes it begin mid-GOP, with slices that
 * reference the (now removed) first-GOP parameter sets. Returns 0 if none. */
static size_t mid_gop_cut_offset(const uint8_t *src, size_t n) {
	const uint8_t *end = src + n;
	int seen_idr = 0;
	for (const uint8_t *p = find_sc(src, end); p < end; ) {
		const uint8_t *pay = p + 3;                 /* NAL header byte */
		const uint8_t *nx = find_sc(pay, end);
		if (nx - pay >= 2) {
			int type = pay[0] & 0x1f;
			if (type == 5) seen_idr = 1;
			else if (seen_idr && type == 1 && (pay[1] & 0x80)) /* post-IDR AU start */
				return (size_t)(p - src);
		}
		p = nx;
	}
	return 0;
}

/* --- test driver ----------------------------------------------------------- */

static uint64_t hashp(const uint8_t *p, ptrdiff_t st, int w, int h) {
	uint64_t x = 1469598103934665603ULL;
	for (int y = 0; y < h; y++)
		for (int c = 0; c < w; c++) { x ^= p[(ptrdiff_t)y * st + c]; x *= 1099511628211ULL; }
	return x;
}

/* Write a byte buffer to a fresh temp file; returns a malloc'd path or NULL. */
static char *write_temp(const uint8_t *p, size_t n) {
	char tmpl[] = "/tmp/seektestXXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) return NULL;
	ssize_t w = write(fd, p, n);
	close(fd);
	if (w != (ssize_t)n) { unlink(tmpl); return NULL; }
	return strdup(tmpl);
}

/* Reading every frame in reverse must equal reading it in sequence. cachesize_mb
 * exercises the decoded-frame-cache sizing (0 = default; a small value drives a
 * smaller ring, so the reverse pass re-seeks more and the eviction path is hit
 * on any stream with more frames than the ring holds). Returns 0 on success,
 * non-zero on the first mismatch/error. */
static int check_seek_consistency_cache(const char *label, const uint8_t *stream, size_t n, int cachesize_mb) {
	char *path = write_temp(stream, n);
	if (!path) { printf("FAIL[%s]: cannot write temp file\n", label); return 1; }

	char err[256] = "";
	MvcSource *s = mvc_open(path, 0, MVC_BASE, 0, 0, 0, cachesize_mb, err, sizeof err);
	if (!s) { printf("FAIL[%s]: open failed: %s\n", label, err); unlink(path); free(path); return 1; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
	uint64_t *ref = malloc((size_t)N * sizeof *ref);

	int rc = 0;
	for (int i = 0; i < N && !rc; i++) {
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			printf("FAIL[%s]: sequential frame %d: %s\n", label, i, err); rc = 1; break;
		}
		ref[i] = hashp(Y, W, W, H);
	}
	/* reverse-order reads: every one is a backward seek */
	for (int i = N - 1; i >= 0 && !rc; i--) {
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			printf("FAIL[%s]: seek frame %d errored: %s\n", label, i, err); rc = 1; break;
		}
		uint64_t h = hashp(Y, W, W, H);
		if (h != ref[i]) {
			printf("FAIL[%s]: seek frame %d returned WRONG content (%016llx != %016llx)\n",
			       label, i, (unsigned long long)h, (unsigned long long)ref[i]);
			rc = 1; break;
		}
	}
	if (!rc) printf("ok[%s]: %d frames, seek == sequential in reverse (cachesize=%d)\n",
	                label, N, cachesize_mb);

	mvc_close(s); free(Y); free(U); free(V); free(ref);
	unlink(path); free(path);
	return rc;
}

/* Default-cache seek==sequential (the common case). */
static int check_seek_consistency(const char *label, const uint8_t *stream, size_t n) {
	return check_seek_consistency_cache(label, stream, n, 0);
}

/* Decode an entire stream (given as bytes) and store each frame's Y-plane hash.
 * Returns the frame count, or -1 on error / if it exceeds `cap` (message printed). */
static int decode_hashes(const char *label, const uint8_t *stream, size_t n,
                         uint64_t *h, int cap) {
	char *path = write_temp(stream, n);
	if (!path) { printf("FAIL[%s]: cannot write temp file\n", label); return -1; }
	char err[256] = "";
	MvcSource *s = mvc_open(path, 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!s) { printf("FAIL[%s]: open failed: %s\n", label, err); unlink(path); free(path); return -1; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	int rc = N;
	if (N > cap) { printf("FAIL[%s]: %d frames exceed cap %d\n", label, N, cap); rc = -1; }
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
	for (int i = 0; i < N && rc >= 0; i++) {
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			printf("FAIL[%s]: frame %d: %s\n", label, i, err); rc = -1; break;
		}
		h[i] = hashp(Y, W, W, H);
	}
	mvc_close(s); free(Y); free(U); free(V); unlink(path); free(path);
	return rc;
}

/* On a mid-GOP cut the leading slices reference now-removed parameter sets and
 * cannot be decoded, so scan_index must not count them. Verify the cut's frames
 * are an exact contiguous suffix of the full stream's frames and that num_frames
 * reaches the real end - guarding the pre-SPS/PPS overcount regression (num_frames
 * too high -> shifted indices, unreadable tail). Returns 0 on success. */
static int check_cut_count(const uint8_t *base, size_t n, size_t coff) {
	enum { CAP = 256 };
	uint64_t bh[CAP], ch[CAP];
	int bn = decode_hashes("cut-ref", base, n, bh, CAP);
	int cn = decode_hashes("cut-count", base + coff, n - coff, ch, CAP);
	if (bn < 0 || cn < 0) return 1;
	if (cn == 0 || cn > bn) { printf("FAIL[cut]: cut has %d frames, full stream %d\n", cn, bn); return 1; }
	int off = -1;
	for (int i = 0; i <= bn - cn; i++)
		if (ch[0] == bh[i]) { off = i; break; }
	if (off < 0) { printf("FAIL[cut]: cut frame 0 matches no full-stream frame\n"); return 1; }
	for (int i = 0; i < cn; i++)
		if (ch[i] != bh[off + i]) {
			printf("FAIL[cut]: frame %d is not full-stream frame %d "
			       "(num_frames overcounted the pre-SPS/PPS slices?)\n", i, off + i);
			return 1;
		}
	if (off + cn != bn) {
		printf("FAIL[cut]: num_frames=%d ends at full-stream %d, not %d\n", cn, off + cn, bn);
		return 1;
	}
	printf("ok[cut]: num_frames=%d = full-stream suffix [%d..%d], no pre-SPS/PPS overcount\n",
	       cn, off, bn - 1);
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <base_multigop.264>\n", argv[0]); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *base = malloc((size_t)n);
	if (fread(base, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read error\n"); return 2; }
	fclose(f);

	int fail = 0;
	/* the base stream (SPS/PPS per GOP) must already seek correctly */
	fail |= check_seek_consistency("base", base, (size_t)n);
	/* same stream, minimum cache: guards the cachesize plumbing + ring-cap sizing -
	 * seek must stay bit-exact regardless of the cache budget */
	fail |= check_seek_consistency_cache("base_mincache", base, (size_t)n, 16);

	Buf hc = synth(base, (size_t)n, 0);   /* headerless GOP  -> P4-H-1 */
	fail |= check_seek_consistency("headerless", hc.p, hc.len);
	free(hc.p);

	Buf ab = synth(base, (size_t)n, 1);   /* AUD-headed once -> P4-M-1 */
	fail |= check_seek_consistency("aud_once", ab.p, ab.len);
	free(ab.p);

	/* mid-GOP cut: begins with VCL slices before any SPS/PPS (the Alba.264 case) */
	size_t coff = mid_gop_cut_offset(base, (size_t)n);
	if (!coff) { printf("FAIL[cut]: fixture has no post-IDR non-IDR access unit\n"); fail = 1; }
	else {
		fail |= check_cut_count(base, (size_t)n, coff);
		fail |= check_seek_consistency("cut", base + coff, (size_t)n - coff);
	}

	free(base);
	printf(fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
	return fail;
}
