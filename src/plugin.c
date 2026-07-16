/*
 * vapoursynth-mvc - VapourSynth (API4) source filter exposing edge264-mvc as
 * core.mvc.Source(). Thin glue over the decode core in mvcsource.c.
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "VapourSynth4.h"
#include "VSHelper4.h"
#include "mvcsource.h"
#include "edge264.h" /* guard: the glue's callback names must not shadow edge264's public API */

typedef struct {
	MvcSource *src;
	VSVideoInfo vi;
} FilterData;

/* Returns a layout (0..3), or -1 for an unrecognized string so the caller can
 * report it instead of silently degrading a typo to the base (mono) view. An
 * unset stack (s == NULL) is the documented default: base. */
static int parse_layout(const char *s) {
	if (!s || !strcasecmp(s, "base") || !strcasecmp(s, "left") || !strcasecmp(s, "l")) return MVC_BASE;
	if (!strcasecmp(s, "right") || !strcasecmp(s, "r")) return MVC_RIGHT;
	if (!strcasecmp(s, "tab") || !strcasecmp(s, "tb") || !strcasecmp(s, "topbottom")) return MVC_TAB;
	if (!strcasecmp(s, "sbs") || !strcasecmp(s, "lr") || !strcasecmp(s, "sidebyside")) return MVC_SBS;
	if (!strcasecmp(s, "alt") || !strcasecmp(s, "alternate") || !strcasecmp(s, "alternating")) return MVC_ALT;
	return -1; /* unknown */
}

static const VSFrame *VS_CC vs_source_get_frame(int n, int activationReason,
	void *instanceData, void **frameData, VSFrameContext *frameCtx,
	VSCore *core, const VSAPI *vsapi) {
	(void)frameData;
	FilterData *d = instanceData;
	if (activationReason != arInitial)
		return NULL; /* a source has no upstream frames to wait for */

	VSFrame *f = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, NULL, core);
	char err[256];
	if (mvc_get_frame(d->src, n,
			vsapi->getWritePtr(f, 0), vsapi->getStride(f, 0),
			vsapi->getWritePtr(f, 1), vsapi->getStride(f, 1),
			vsapi->getWritePtr(f, 2), vsapi->getStride(f, 2), err, sizeof err)) {
		vsapi->freeFrame(f);
		vsapi->setFilterError(err, frameCtx);
		return NULL;
	}
	return f;
}

static void VS_CC vs_source_free(void *instanceData, VSCore *core, const VSAPI *vsapi) {
	(void)core; (void)vsapi;
	FilterData *d = instanceData;
	mvc_close(d->src);
	free(d);
}

static void VS_CC vs_source_create(const VSMap *in, VSMap *out, void *userData,
	VSCore *core, const VSAPI *vsapi) {
	(void)userData;
	int e;
	const char *source = vsapi->mapGetData(in, "source", 0, &e);
	if (e || !source) {
		vsapi->mapSetError(out, "mvc.Source: 'source' (path to an .264/.h264 Annex-B file) is required");
		return;
	}
	const char *stack = vsapi->mapGetData(in, "stack", 0, &e);
	int layout = parse_layout(e ? NULL : stack);
	if (layout < 0) {
		char buf[128];
		snprintf(buf, sizeof buf, "mvc.Source: unknown stack mode '%s' (use base/right/tab/sbs/alt)", stack);
		vsapi->mapSetError(out, buf);
		return;
	}
	/* swaplr: flip the two views in whatever layout was chosen (default off). Any
	 * nonzero value means on; mvc_open normalises it. */
	int swaplr = vsapi->mapGetIntSaturated(in, "swaplr", 0, &e);
	if (e) swaplr = 0;
	/* threads: edge264 internal decode parallelism. Default -1 (auto-detect cores):
	 * getFrame is serialised for this node (fmUnordered), so internal MT is safe,
	 * bit-exact, and makes a seek's forward re-decode several times faster. Pass 0
	 * for single-thread, or an explicit count. */
	int threads = vsapi->mapGetIntSaturated(in, "threads", 0, &e);
	if (e) threads = -1;
	/* cachesize: decoded-frame cache ceiling in MiB (0 = core default). Larger =
	 * fewer re-seeks on a backward / Reverse() pass over a long GOP. */
	int cachesize = vsapi->mapGetIntSaturated(in, "cachesize", 0, &e);
	if (e) cachesize = 0;
	/* fpsnum/fpsden are a pair: an unset key sets e (peUnset). Treat them
	 * atomically - reject a half-specified or non-positive rate rather than
	 * splice a user value with half of the default (e.g. fpsnum=25 -> 25/1001).
	 * Only 0/0 (both absent) reaches mvc_open, which then keeps its default. */
	int64_t fpsnum = vsapi->mapGetInt(in, "fpsnum", 0, &e);
	int have_num = !e;
	int64_t fpsden = vsapi->mapGetInt(in, "fpsden", 0, &e);
	int have_den = !e;
	if (have_num != have_den) {
		vsapi->mapSetError(out, "mvc.Source: fpsnum and fpsden must be specified together");
		return;
	}
	if ((have_num && fpsnum <= 0) || (have_den && fpsden <= 0)) {
		vsapi->mapSetError(out, "mvc.Source: fpsnum and fpsden must be positive");
		return;
	}
	if (!have_num) { fpsnum = 0; fpsden = 0; }

	char emsg[256];
	MvcSource *src = mvc_open(source, threads, (MvcLayout)layout, swaplr, fpsnum, fpsden, cachesize, emsg, sizeof emsg);
	if (!src) {
		char buf[320];
		snprintf(buf, sizeof buf, "mvc.Source: %s", emsg);
		vsapi->mapSetError(out, buf);
		return;
	}
	const MvcInfo *info = mvc_info(src);

	FilterData *d = calloc(1, sizeof *d);
	if (!d) { mvc_close(src); vsapi->mapSetError(out, "mvc.Source: out of memory"); return; }
	d->src = src;
	vsapi->getVideoFormatByID(&d->vi.format, pfYUV420P8, core);
	d->vi.width = info->width;
	d->vi.height = info->height;
	d->vi.numFrames = info->num_frames;
	/* VapourSynth requires the frame-rate fraction in lowest terms; a raw pair
	 * (e.g. 30000/1002) makes createVideoFilter reject the VSVideoInfo. Reducing
	 * preserves the exact rational value. */
	d->vi.fpsNum = info->fps_num;
	d->vi.fpsDen = info->fps_den;
	vsh_reduceRational(&d->vi.fpsNum, &d->vi.fpsDen);

	/* fmUnordered: a source reads a file and mutates internal state per request,
	 * so VapourSynth serialises getFrame calls for this node. */
	vsapi->createVideoFilter(out, "Source", &d->vi, vs_source_get_frame, vs_source_free,
		fmUnordered, NULL, 0, d, core);
	/* On failure the core sets an error and never takes ownership of d (no node,
	 * so freeFunc is never called); free it here to avoid leaking the mmap +
	 * decoder + threads. On success ownership has transferred - do not touch d. */
	if (vsapi->mapGetError(out)) {
		mvc_close(d->src);
		free(d);
	}
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
	vspapi->configPlugin("de.duttke.mvc", "mvc",
		"H.264 MVC (3D) and AVC source, built on edge264-mvc",
		VS_MAKE_VERSION(0, 4), VAPOURSYNTH_API_VERSION, 0, plugin);
	/* New optional args are appended so existing positional calls keep their
	 * indices (swaplr after the v0.1.0 set, cachesize after that). */
	vspapi->registerFunction("Source",
		"source:data;stack:data:opt;threads:int:opt;fpsnum:int:opt;fpsden:int:opt;swaplr:int:opt;cachesize:int:opt;",
		"clip:vnode;",
		vs_source_create, NULL, plugin);
}
