/*
 * poctest - regression test for the display-order tripwire.
 *
 * mvc_get_frame counts decoder outputs blindly (idx = next_out++) and trusts
 * that the decoder emits exactly the pictures scan_index counted, in display
 * order. If that invariant ever breaks - a dropped/duplicated picture, a reorder
 * bug, decoder drift on a version bump - every later frame is served under the
 * wrong index, silently. The source now verifies DisplayPoc never decreases
 * across a decode run and fails loudly on a backward step.
 *
 * edge264_get_frame is intercepted via the linker's --wrap so one output's
 * DisplayPoc is forced non-monotone, standing in for any such divergence.
 *
 * usage: poctest <base_multigop.264>
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include "edge264.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int __real_edge264_get_frame(Edge264Decoder *dec, Edge264Frame *out, int borrow);

static int g_corrupt_next_poc = 0;

int __wrap_edge264_get_frame(Edge264Decoder *dec, Edge264Frame *out, int borrow) {
	int r = __real_edge264_get_frame(dec, out, borrow);
	if (r == 0 && g_corrupt_next_poc) { g_corrupt_next_poc = 0; out->DisplayPoc = -1; }
	return r;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <base_multigop.264>\n", argv[0]); return 2; }
	char err[256] = "";
	MvcSource *s = mvc_open(argv[1], 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err); /* first frame, no corruption */
	if (!s) { fprintf(stderr, "open failed: %s\n", err); return 2; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);

	/* read a couple of frames so a real (increasing) POC baseline is established */
	for (int i = 1; i <= 2 && i < N; i++)
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) { fprintf(stderr, "read %d: %s\n", i, err); return 2; }

	/* now force the next output's DisplayPoc backwards - must be caught */
	g_corrupt_next_poc = 1;
	int caught = 0;
	for (int i = 3; i < N; i++) {
		err[0] = 0;
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			printf("frame %d errored: %s\n", i, err);
			if (strstr(err, "POC") || strstr(err, "diverged")) caught = 1;
			break;
		}
	}

	mvc_close(s); free(Y); free(U); free(V);
	if (caught) { printf("RESULT: PASS (non-monotone display order caught)\n"); return 0; }
	printf("RESULT: FAIL (display-order divergence went undetected)\n");
	return 1;
}
