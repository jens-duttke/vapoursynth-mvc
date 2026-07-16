/*
 * mvcsource - host-independent H.264/MVC decode core built on edge264-mvc.
 *
 * Opens an Annex-B .264/.h264 elementary stream (2D or MVC 3D), exposes the
 * stream properties (dimensions, frame count) and serves any output frame by
 * display index, optionally combining the two MVC views into one frame
 * (top-and-bottom or side-by-side). All the real logic - the edge264 caller
 * loop, seeking and view assembly - lives here so it can be tested without a
 * frameserver runtime; the VapourSynth and AviSynth+ glues are thin wrappers on top.
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#ifndef MVCSOURCE_H
#define MVCSOURCE_H

#include <stddef.h>
#include <stdint.h>

typedef struct MvcSource MvcSource;

typedef enum {
	MVC_BASE  = 0, /* base / left view only (2D)          */
	MVC_RIGHT = 1, /* dependent / right view only         */
	MVC_TAB   = 2, /* top-and-bottom: base over dependent */
	MVC_SBS   = 3, /* side-by-side: base left, dep right  */
	MVC_ALT   = 4, /* alternating frames: base, dep, base, dep ... (2x the frames) */
} MvcLayout;

typedef struct {
	int width;         /* output frame width  (after layout) */
	int height;        /* output frame height (after layout) */
	int base_width;    /* per-view (cropped) width           */
	int base_height;   /* per-view (cropped) height          */
	int64_t fps_num;
	int64_t fps_den;
	int num_frames;    /* number of output frames: base pictures, or 2x that for MVC_ALT */
	int is_mvc;        /* stream actually carries a dependent view */
	MvcLayout layout;  /* effective layout (tab/sbs/alt downgraded to base on 2D) */
} MvcInfo;

/*
 * Open a stream. n_threads: 0 single-thread, -1 auto, >0 explicit. swaplr (0/1)
 * swaps the two views in every layout (base <-> dependent), so a caller can flip
 * a stream authored right-eye-first without changing the layout. fps is only a
 * hint written to the info (edge264's public API does not expose the VUI); pass
 * 0/0 to keep the default 24000/1001. MVC_ALT interleaves the two views into one
 * clip, so it reports twice the frame count and twice the frame rate.
 *
 * cachesize_mb bounds the decoded-frame cache (MiB); <= 0 selects the default.
 * The cache holds the most recently produced pictures, so backward / repeat /
 * Reverse() access is served from RAM instead of re-decoding from the preceding
 * IDR. A larger cache spans more of a long GOP, so a backward pass triggers fewer
 * re-seeks (the pathological Reverse() cost); a smaller one trades that for less
 * memory. Buffers are allocated lazily, so the budget is a ceiling, not an
 * up-front reservation.
 *
 * Returns NULL on failure and writes a message to err (if err != NULL).
 */
MvcSource *mvc_open(const char *path, int n_threads, MvcLayout layout, int swaplr,
	int64_t fps_num, int64_t fps_den, int cachesize_mb, char *err, size_t errsize);

const MvcInfo *mvc_info(const MvcSource *s);

/*
 * Fill dst planes with output frame n in the configured layout. Each plane
 * pointer/stride is caller-owned (e.g. a VapourSynth frame). Returns 0 on
 * success, non-zero on error (message in err).
 */
int mvc_get_frame(MvcSource *s, int n,
	uint8_t *dstY, ptrdiff_t strideY,
	uint8_t *dstU, ptrdiff_t strideU,
	uint8_t *dstV, ptrdiff_t strideV,
	char *err, size_t errsize);

void mvc_close(MvcSource *s);

#endif
