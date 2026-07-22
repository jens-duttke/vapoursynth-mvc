/*
 * Two-file (separate base + dependent view) decode test for the mvcsource core.
 *
 * A 3D Blu-ray demuxed with tsMuxeR / BD3D2MK3D yields two separate elementary
 * streams - a base-view .264 and a dependent-view .mvc - rather than the single
 * combined MVC stream mvc_open expects. mvc_open2 reads both and interleaves them
 * per access unit in memory. This test proves that path bit-exact against the
 * already-trusted single-file (combined) decode of the very same stream:
 *
 *   1. for every layout, open the combined stream (mvc_open) and the separate
 *      base+dependent pair (mvc_open2) and require identical stream info and
 *      byte-identical frames;
 *   2. re-read frame 0 after touching the last frame, so the two-file path's
 *      backward seek (IDR re-decode) is exercised and must still match;
 *   3. the base stream alone must decode as a plain 2D AVC clip - what makes a
 *      demuxer's base output 2D-playable.
 *
 * The base+dependent pair is a REAL demuxer split (committed fixtures produced by
 * tsMuxeR), NOT a split synthesised here. An earlier version split the combined
 * stream by NAL type, which cannot reproduce a real demux: a Blu-ray access
 * unit's dependent section carries its own parameter sets and SEI, sharing NAL
 * types (7/8/6) with the base view, so a by-type split misrouted them to the base
 * file and made mvc_open2's dependent slices decode before their PPS. That flaw
 * lived only in the test (mvc_open2 is bit-exact to FRIMSource on real demuxes);
 * using a real committed demux removes the guesswork. See tests/fixtures/README.md.
 *
 * usage: twofiletest <combined.264> <base.264> <dependent.mvc>
 */
#include "mvcsource.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read frame `n` in layout `lay` from `s` into caller planes; return 0 on ok. */
static int get(MvcSource *s, int n, uint8_t *Y, uint8_t *U, uint8_t *V, int W, int CW) {
	char err[256] = "";
	if (mvc_get_frame(s, n, Y, W, U, CW, V, CW, err, sizeof err)) {
		fprintf(stderr, "  get_frame(%d) failed: %s\n", n, err);
		return -1;
	}
	return 0;
}

static const char *LAYOUT_NAME[] = { "base", "right", "tab", "sbs", "alt" };

static int compare_layout(const char *combined, const char *base, const char *dep, MvcLayout lay) {
	char err[256] = "";
	MvcSource *sc = mvc_open(combined, 0, lay, 0, 0, 0, 0, err, sizeof err);
	if (!sc) { fprintf(stderr, "  combined open failed: %s\n", err); return -1; }
	MvcSource *st = mvc_open2(base, dep, 0, lay, 0, 0, 0, 0, err, sizeof err);
	if (!st) { fprintf(stderr, "  two-file open failed: %s\n", err); mvc_close(sc); return -1; }

	const MvcInfo *ic = mvc_info(sc), *it = mvc_info(st);
	if (ic->num_frames != it->num_frames || ic->width != it->width ||
	    ic->height != it->height || ic->is_mvc != it->is_mvc || ic->layout != it->layout) {
		fprintf(stderr, "  info mismatch: combined %dx%d n=%d mvc=%d lay=%d vs two-file %dx%d n=%d mvc=%d lay=%d\n",
			ic->width, ic->height, ic->num_frames, ic->is_mvc, ic->layout,
			it->width, it->height, it->num_frames, it->is_mvc, it->layout);
		mvc_close(sc); mvc_close(st); return -1;
	}

	int W = ic->width, H = ic->height, CW = W / 2, CH = H / 2;
	size_t ysz = (size_t)W * H, csz = (size_t)CW * CH;
	uint8_t *cY = malloc(ysz), *cU = malloc(csz), *cV = malloc(csz);
	uint8_t *tY = malloc(ysz), *tU = malloc(csz), *tV = malloc(csz);
	int ok = 1;
	if (!cY || !cU || !cV || !tY || !tU || !tV) { fprintf(stderr, "  oom\n"); ok = 0; goto done; }

	for (int i = 0; i < ic->num_frames && ok; i++) {
		if (get(sc, i, cY, cU, cV, W, CW) || get(st, i, tY, tU, tV, W, CW)) { ok = 0; break; }
		if (memcmp(cY, tY, ysz) || memcmp(cU, tU, csz) || memcmp(cV, tV, csz)) {
			fprintf(stderr, "  [%s] frame %d MISMATCH\n", LAYOUT_NAME[lay], i);
			ok = 0;
		}
	}
	/* backward seek on the two-file source: re-read frame 0 after the last frame */
	if (ok && ic->num_frames > 1) {
		if (get(st, ic->num_frames - 1, tY, tU, tV, W, CW) ||
		    get(sc, 0, cY, cU, cV, W, CW) || get(st, 0, tY, tU, tV, W, CW)) { ok = 0; }
		else if (memcmp(cY, tY, ysz) || memcmp(cU, tU, csz) || memcmp(cV, tV, csz)) {
			fprintf(stderr, "  [%s] frame 0 after backward seek MISMATCH\n", LAYOUT_NAME[lay]);
			ok = 0;
		}
	}
	if (ok)
		printf("  [%s] %d frames bit-exact (two-file == combined) + backward seek\n",
			LAYOUT_NAME[lay], ic->num_frames);
done:
	free(cY); free(cU); free(cV); free(tY); free(tU); free(tV);
	mvc_close(sc); mvc_close(st);
	return ok ? 0 : -1;
}

/* The two-file path caches its interleave next to the dependent stream
 * (`<dep>.mvcidx`) so a reopen skips the full end-to-end scan of both streams.
 * Check that the sidecar is written, that a reopen (which loads it) decodes
 * identically to the fresh build, and that a corrupt sidecar is rejected and
 * rebuilt rather than trusted. Uses MVC_ALT and the last frame so the exercised
 * path spans both views and a backward seek. */
static int test_cache(const char *base, const char *dep) {
	char cpath[2048];
	snprintf(cpath, sizeof cpath, "%s.mvcidx", dep);
	remove(cpath);
	char err[256] = "";

	MvcSource *s = mvc_open2(base, dep, 0, MVC_ALT, 0, 0, 0, 0, err, sizeof err);
	if (!s) { printf("FAIL[cache]: build open failed: %s\n", err); return -1; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, fr = in->num_frames - 1;
	size_t ysz = (size_t)W * H, csz = (size_t)CW * CH;
	uint8_t *aY = malloc(ysz), *aU = malloc(csz), *aV = malloc(csz);
	uint8_t *bY = malloc(ysz), *bU = malloc(csz), *bV = malloc(csz);
	int rc = 0;
	if (!aY || !aU || !aV || !bY || !bU || !bV) { printf("FAIL[cache]: oom\n"); rc = -1; }
	else if (get(s, fr, aY, aU, aV, W, CW)) rc = -1;
	mvc_close(s);

	FILE *cf = fopen(cpath, "rb");
	if (!cf) { printf("FAIL[cache]: no sidecar written\n"); rc = -1; }
	else { fclose(cf); if (!rc) printf("ok[cache]: sidecar written next to the dependent stream\n"); }

	/* reopen: loads the sidecar, must decode identically */
	if (!rc) {
		MvcSource *s2 = mvc_open2(base, dep, 0, MVC_ALT, 0, 0, 0, 0, err, sizeof err);
		if (!s2) { printf("FAIL[cache]: reopen failed: %s\n", err); rc = -1; }
		else {
			if (get(s2, fr, bY, bU, bV, W, CW) ||
			    memcmp(aY, bY, ysz) || memcmp(aU, bU, csz) || memcmp(aV, bV, csz)) {
				printf("FAIL[cache]: cached reopen differs from the fresh build\n"); rc = -1;
			} else printf("ok[cache]: cached reopen bit-exact to the fresh build\n");
			mvc_close(s2);
		}
	}

	/* a corrupt sidecar must be rejected (rebuilt), never trusted */
	if (!rc) {
		FILE *w = fopen(cpath, "wb");
		if (w) { fwrite("MVC2FI02\xff\xff\xff\xff garbage", 1, 24, w); fclose(w); }
		MvcSource *s3 = mvc_open2(base, dep, 0, MVC_ALT, 0, 0, 0, 0, err, sizeof err);
		if (!s3) { printf("FAIL[cache]: open over a corrupt sidecar failed: %s\n", err); rc = -1; }
		else {
			if (get(s3, fr, bY, bU, bV, W, CW) ||
			    memcmp(aY, bY, ysz) || memcmp(aU, bU, csz) || memcmp(aV, bV, csz)) {
				printf("FAIL[cache]: corrupt sidecar not rebuilt correctly\n"); rc = -1;
			} else printf("ok[cache]: corrupt sidecar rejected and rebuilt\n");
			mvc_close(s3);
		}
	}

	remove(cpath);
	free(aY); free(aU); free(aV); free(bY); free(bU); free(bV);
	return rc;
}

/* Cold seeks: open a FRESH two-file source per target frame and require its
 * output byte-identical to the combined decode of the same frame. A fresh open
 * defeats the decoded-frame cache, so every target goes through seek_to and a
 * forward re-decode from the chosen seek point - on a stream with open-GOP
 * recovery points that exercises the two-file recovery-point seek points
 * (including the leading-picture discard, see SeekPoint.valid_from), and after
 * the first open it also exercises seek points loaded back from the sidecar. */
static int test_cold_seeks(const char *combined, const char *base, const char *dep) {
	char err[256] = "";
	MvcSource *sc = mvc_open(combined, 0, MVC_TAB, 0, 0, 0, 0, err, sizeof err);
	if (!sc) { printf("FAIL[cold]: combined open failed: %s\n", err); return -1; }
	const MvcInfo *in = mvc_info(sc);
	int N = in->num_frames, W = in->width, H = in->height, CW = W / 2, CH = H / 2;
	size_t ysz = (size_t)W * H, csz = (size_t)CW * CH;
	uint8_t *cY = malloc(ysz), *cU = malloc(csz), *cV = malloc(csz);
	uint8_t *tY = malloc(ysz), *tU = malloc(csz), *tV = malloc(csz);
	int rc = 0, checked = 0;
	if (!cY || !cU || !cV || !tY || !tU || !tV) { printf("FAIL[cold]: oom\n"); rc = -1; goto done; }
	/* last frame first: the farthest cold target from the mandatory frame-0 decode */
	int targets[] = { N - 1, 0, (2 * N) / 3, 1, N / 2, N - 2 };
	for (unsigned k = 0; k < sizeof targets / sizeof targets[0] && !rc; k++) {
		int t = targets[k];
		if (t < 0 || t >= N) continue;
		if (get(sc, t, cY, cU, cV, W, CW)) { rc = -1; break; }
		MvcSource *st = mvc_open2(base, dep, 0, MVC_TAB, 0, 0, 0, 0, err, sizeof err);
		if (!st) { printf("FAIL[cold]: two-file open failed: %s\n", err); rc = -1; break; }
		if (get(st, t, tY, tU, tV, W, CW)) rc = -1;
		else if (memcmp(cY, tY, ysz) || memcmp(cU, tU, csz) || memcmp(cV, tV, csz)) {
			printf("FAIL[cold]: frame %d differs from the combined decode\n", t);
			rc = -1;
		} else checked++;
		mvc_close(st);
	}
	if (!rc)
		printf("ok[cold]: %d cold-open seeks bit-exact to the combined decode\n", checked);
done:
	free(cY); free(cU); free(cV); free(tY); free(tU); free(tV);
	mvc_close(sc);
	return rc;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <combined.264> <base.264> <dependent.mvc>\n", argv[0]);
		return 2;
	}
	const char *combined = argv[1], *base = argv[2], *dep = argv[3];
	char err[256] = "";

	/* The combined stream must actually be MVC for the two-file path to mean
	 * anything; the committed fixture is. A 2D combined stream has no dependent
	 * view to pair, so skip rather than fail. */
	MvcSource *probe = mvc_open(combined, 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!probe) { fprintf(stderr, "cannot open combined %s: %s\n", combined, err); return 1; }
	int is_mvc = mvc_info(probe)->is_mvc;
	mvc_close(probe);
	if (!is_mvc) {
		printf("combined stream is 2D (no dependent view) - two-file path not applicable, skipping\n");
		return 0;
	}

	/* The base stream alone must decode as a plain 2D AVC clip (is_mvc == 0): that
	 * is exactly a demuxer's base output, and what makes it 2D-playable. If it
	 * still parsed as MVC the pair would not be a real base/dependent split. */
	MvcSource *b = mvc_open(base, 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!b) { printf("FAIL: base stream does not open standalone: %s\n", err); return 1; }
	int base_2d = !mvc_info(b)->is_mvc, base_frames = mvc_info(b)->num_frames;
	mvc_close(b);
	if (!base_2d) { printf("FAIL: base stream still detects as MVC (not a clean base view?)\n"); return 1; }
	printf("ok[base-2d]: base stream decodes standalone as a %d-frame 2D AVC clip\n", base_frames);

	int rc = 0;
	for (MvcLayout lay = MVC_BASE; lay <= MVC_ALT; lay++)
		rc |= compare_layout(combined, base, dep, lay);

	rc |= test_cold_seeks(combined, base, dep);
	rc |= test_cache(base, dep);

	printf(rc ? "RESULT: FAIL\n" : "RESULT: PASS (mvc_open2 bit-exact to the combined decode on a real demux)\n");
	return rc ? 1 : 0;
}
