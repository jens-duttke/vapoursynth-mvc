/*
 * Standalone test for the mvcsource decode core (no VapourSynth required).
 * Verifies stream info, sequential reads, and that a random-access read of a
 * frame is bit-identical to reading it sequentially.
 *
 * usage: coretest <file.264> [layout 0=base 1=right 2=tab 3=sbs 4=alt]
 */
#include "mvcsource.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv_plane(const uint8_t *p, ptrdiff_t stride, int w, int h) {
	uint64_t h64 = 1469598103934665603ULL;
	for (int y = 0; y < h; y++) {
		const uint8_t *r = p + (ptrdiff_t)y * stride;
		for (int x = 0; x < w; x++) { h64 ^= r[x]; h64 *= 1099511628211ULL; }
	}
	return h64;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <file.264> [layout 0..4]\n", argv[0]); return 2; }
	MvcLayout layout = argc > 2 ? (MvcLayout)atoi(argv[2]) : MVC_BASE;
	char err[256] = "";

	/* self-check: an out-of-range layout must be rejected, not accepted with a
	 * frame the assemble switch never fills (garbage planes) */
	{
		char e2[256] = "";
		MvcSource *bad = mvc_open(argv[1], 0, (MvcLayout)99, 0, 0, 0, 0, e2, sizeof e2);
		if (bad) { fprintf(stderr, "FAIL: mvc_open accepted invalid layout 99\n"); mvc_close(bad); return 1; }
	}

	MvcSource *s = mvc_open(argv[1], 0, layout, 0, 0, 0, 0, err, sizeof err);
	if (!s) { fprintf(stderr, "open failed: %s\n", err); return 1; }
	const MvcInfo *in = mvc_info(s);

	/* dump mode: coretest <file> <layout> <frame> <out.yuv> -> raw planar YUV */
	if (argc >= 5) {
		int fn = atoi(argv[3]);
		int W = in->width, H = in->height, CW = W / 2, CH = H / 2;
		uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
		if (!Y || !U || !V) { fprintf(stderr, "oom\n"); return 1; }
		if (mvc_get_frame(s, fn, Y, W, U, CW, V, CW, err, sizeof err)) {
			fprintf(stderr, "frame %d failed: %s\n", fn, err); return 1;
		}
		FILE *f = fopen(argv[4], "wb");
		if (!f) { fprintf(stderr, "cannot open %s for writing: %s\n", argv[4], strerror(errno)); return 1; }
		if (fwrite(Y, 1, (size_t)W * H, f) != (size_t)W * H ||
		    fwrite(U, 1, (size_t)CW * CH, f) != (size_t)CW * CH ||
		    fwrite(V, 1, (size_t)CW * CH, f) != (size_t)CW * CH ||
		    fclose(f) != 0) {
			fprintf(stderr, "writing %s failed: %s\n", argv[4], strerror(errno)); return 1;
		}
		fprintf(stderr, "dumped frame %d (%dx%d) to %s\n", fn, W, H, argv[4]);
		mvc_close(s); free(Y); free(U); free(V);
		return 0;
	}
	printf("info: %dx%d  base=%dx%d  fps=%lld/%lld  frames=%d  mvc=%d  layout=%d\n",
		in->width, in->height, in->base_width, in->base_height,
		(long long)in->fps_num, (long long)in->fps_den, in->num_frames, in->is_mvc, in->layout);

	int W = in->width, H = in->height, CW = W / 2, CH = H / 2;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
	if (!Y || !U || !V) { fprintf(stderr, "oom\n"); return 1; }

	int N = in->num_frames < 8 ? in->num_frames : 8;
	uint64_t ref[8];

	/* 1. sequential reference for the first N frames */
	for (int i = 0; i < N; i++) {
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			fprintf(stderr, "seq frame %d failed: %s\n", i, err); return 1;
		}
		ref[i] = fnv_plane(Y, W, W, H);
		printf("seq  frame %2d  Yhash=%016llx\n", i, (unsigned long long)ref[i]);
	}

	/* 2. touch the last frame to force a full forward decode */
	if (mvc_get_frame(s, in->num_frames - 1, Y, W, U, CW, V, CW, err, sizeof err)) {
		fprintf(stderr, "last frame %d failed: %s\n", in->num_frames - 1, err); return 1;
	}
	printf("last frame %d  Yhash=%016llx\n", in->num_frames - 1, (unsigned long long)fnv_plane(Y, W, W, H));

	/* 3. random-access: re-read the first N frames (now each triggers a seek)
	 *    and require bit-identical output to the sequential reference */
	int ok = 1;
	for (int i = 0; i < N; i++) {
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			fprintf(stderr, "seek frame %d failed: %s\n", i, err); return 1;
		}
		uint64_t h = fnv_plane(Y, W, W, H);
		int match = h == ref[i];
		printf("seek frame %2d  Yhash=%016llx  %s\n", i, (unsigned long long)h, match ? "OK" : "MISMATCH");
		ok &= match;
	}

	mvc_close(s);
	free(Y); free(U); free(V);
	printf(ok ? "RESULT: PASS (seek == sequential)\n" : "RESULT: FAIL (seek mismatch)\n");
	return ok ? 0 : 1;
}
