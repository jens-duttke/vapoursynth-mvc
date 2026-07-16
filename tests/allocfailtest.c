/*
 * allocfailtest - regression test for decoder-allocation failure during a seek.
 *
 * A seek tears the decoder down and recreates it. If edge264_alloc fails (OOM),
 * reset_decoder returns failure - but seek_to must not ignore it: proceeding with
 * a NULL decoder yields a misleading "unexpected end of stream" (or a generic
 * "decoder rejected input") instead of a clear allocation error, and leaves the
 * seek state half-applied. The core must report the allocation failure and leave
 * the position retriable, so a later request (once memory is available) recovers.
 *
 * edge264_alloc is intercepted via the linker's --wrap so the seek-time
 * reallocation fails deterministically, without real memory pressure.
 *
 * usage: allocfailtest <base_multigop.264>
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include "edge264.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Edge264Decoder *__real_edge264_alloc(int n_threads, Edge264LogCb log_cb, void *log_arg,
	int log_mbs, Edge264AllocCb alloc_cb, Edge264FreeCb free_cb, void *alloc_arg);

static int g_fail_next_alloc = 0;

Edge264Decoder *__wrap_edge264_alloc(int n_threads, Edge264LogCb log_cb, void *log_arg,
	int log_mbs, Edge264AllocCb alloc_cb, Edge264FreeCb free_cb, void *alloc_arg) {
	if (g_fail_next_alloc) { g_fail_next_alloc = 0; return NULL; }
	return __real_edge264_alloc(n_threads, log_cb, log_arg, log_mbs, alloc_cb, free_cb, alloc_arg);
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <base_multigop.264>\n", argv[0]); return 2; }
	char err[256] = "";
	MvcSource *s = mvc_open(argv[1], 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err); /* alloc #1 succeeds */
	if (!s) { fprintf(stderr, "open failed: %s\n", err); return 2; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);

	/* full forward decode so next_out is at the end; then a backward seek forces
	 * reset_decoder -> edge264_alloc, which we make fail. */
	if (mvc_get_frame(s, N - 1, Y, W, U, CW, V, CW, err, sizeof err)) {
		fprintf(stderr, "read last failed: %s\n", err); return 2;
	}

	g_fail_next_alloc = 1;
	err[0] = 0;
	int rc = mvc_get_frame(s, 1, Y, W, U, CW, V, CW, err, sizeof err); /* seek back -> alloc fails */
	int reported_alloc = rc != 0 && (strstr(err, "alloc") || strstr(err, "allocate"));
	printf("seek-with-failed-alloc: rc=%d err=\"%s\"\n", rc, err);

	/* retry after memory is available again: the position must still be usable */
	err[0] = 0;
	int rc2 = mvc_get_frame(s, 1, Y, W, U, CW, V, CW, err, sizeof err);
	int recovered = rc2 == 0;
	printf("retry-after-recovery: rc=%d err=\"%s\"\n", rc2, err);

	mvc_close(s); free(Y); free(U); free(V);
	if (reported_alloc && recovered) { printf("RESULT: PASS (alloc failure reported, position recovered)\n"); return 0; }
	printf("RESULT: FAIL (reported_alloc=%d recovered=%d)\n", reported_alloc, recovered);
	return 1;
}
