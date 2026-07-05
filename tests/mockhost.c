/*
 * mockhost - a minimal VapourSynth API4 host that loads the vapoursynth-mvc plugin
 * shared object and drives it through the real API, so the VapourSynth glue can
 * be tested end-to-end without a VapourSynth installation. It implements just
 * the handful of VSAPI/VSPLUGINAPI entry points the plugin uses, deliberately
 * with padded frame strides to catch stride-handling bugs.
 *
 * usage: mockhost <plugin.so> <file.264> <stack> [dumpframe dumpfile]
 *   verifies the reported clip properties and that frames decode without error;
 *   with dumpframe/dumpfile, writes that frame's planar YUV (tightly packed) for
 *   an external bit-exact cross-check.
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provide the concrete definitions for VapourSynth's opaque handle types before
 * including the header (which only forward-declares them). */
#define VS_MOCK 1

typedef struct VSMapEntry {
	char key[32];
	int type;          /* 0 unset, 1 int, 3 data, 5 node */
	int64_t i64;
	char *data;
	struct VSNode *node;
} VSMapEntry;

struct VSMap { VSMapEntry e[32]; int n; char error[512]; int has_error; };
struct VSCore { int _; };
struct VSPlugin { int _; };
struct VSFrameContext { char error[512]; int has_error; };

#include "VapourSynth4.h"

struct VSFrame {
	VSVideoFormat fmt;
	int width, height;
	uint8_t *plane[3];
	ptrdiff_t stride[3];
};
struct VSNode {
	VSFilterGetFrame getFrame;
	VSFilterFree freeFn;
	void *inst;
	VSVideoInfo vi;
};

/* ---- the captured registered Source function ---- */
static VSPublicFunction g_source_func;

/* ---- VSMap ---- */
static VSMapEntry *find(const VSMap *m, const char *key) {
	for (int i = 0; i < m->n; i++)
		if (!strcmp(m->e[i].key, key)) return (VSMapEntry *)&m->e[i];
	return NULL;
}
static VSMapEntry *put(VSMap *m, const char *key) {
	VSMapEntry *e = find(m, key);
	if (e) return e;
	e = &m->e[m->n++];
	snprintf(e->key, sizeof e->key, "%s", key);
	return e;
}
static VSMap *VS_CC mock_createMap(void) { return calloc(1, sizeof(VSMap)); }
static void VS_CC mock_freeMap(VSMap *m) {
	if (!m) return;
	for (int i = 0; i < m->n; i++) free(m->e[i].data); /* nodes are owned by the caller, not freed here */
	free(m);
}
static void VS_CC mock_mapSetError(VSMap *m, const char *msg) {
	snprintf(m->error, sizeof m->error, "%s", msg); m->has_error = 1;
}
static const char *VS_CC mock_mapGetError(const VSMap *m) { return m->has_error ? m->error : NULL; }
static const char *VS_CC mock_mapGetData(const VSMap *m, const char *key, int index, int *err) {
	(void)index; VSMapEntry *e = find(m, key);
	if (!e || e->type != 3) { if (err) *err = 1; return NULL; }
	if (err) *err = 0;
	return e->data;
}
static int VS_CC mock_mapGetDataSize(const VSMap *m, const char *key, int index, int *err) {
	(void)index; VSMapEntry *e = find(m, key);
	if (!e || e->type != 3) { if (err) *err = 1; return -1; }
	if (err) *err = 0;
	return (int)strlen(e->data);
}
static int VS_CC mock_mapSetData(VSMap *m, const char *key, const char *data, int size, int type, int append) {
	(void)type; (void)append; VSMapEntry *e = put(m, key);
	e->type = 3; free(e->data);
	int len = size >= 0 ? size : (int)strlen(data);
	e->data = malloc(len + 1); memcpy(e->data, data, len); e->data[len] = 0;
	return 0;
}
static int64_t VS_CC mock_mapGetInt(const VSMap *m, const char *key, int index, int *err) {
	(void)index; VSMapEntry *e = find(m, key);
	if (!e || e->type != 1) { if (err) *err = 1; return 0; }
	if (err) *err = 0;
	return e->i64;
}
static int VS_CC mock_mapGetIntSaturated(const VSMap *m, const char *key, int index, int *err) {
	return (int)mock_mapGetInt(m, key, index, err);
}
static int VS_CC mock_mapSetInt(VSMap *m, const char *key, int64_t i, int append) {
	(void)append; VSMapEntry *e = put(m, key); e->type = 1; e->i64 = i; return 0;
}
static VSNode *VS_CC mock_mapGetNode(const VSMap *m, const char *key, int index, int *err) {
	(void)index; VSMapEntry *e = find(m, key);
	if (!e || e->type != 5) { if (err) *err = 1; return NULL; }
	if (err) *err = 0;
	return e->node;
}
static int VS_CC mock_mapSetNode(VSMap *m, const char *key, VSNode *node, int append) {
	(void)append; VSMapEntry *e = put(m, key); e->type = 5; e->node = node; return 0;
}

/* ---- format ---- */
static int VS_CC mock_getVideoFormatByID(VSVideoFormat *f, uint32_t id, VSCore *core) {
	(void)core;
	f->colorFamily = (int)(id >> 28) & 0xf;
	f->sampleType  = (int)(id >> 24) & 0xf;
	f->bitsPerSample = (int)(id >> 16) & 0xff;
	f->subSamplingW = (int)(id >> 8) & 0xff;
	f->subSamplingH = (int)(id >> 0) & 0xff;
	f->bytesPerSample = (f->bitsPerSample + 7) / 8;
	if (f->bytesPerSample == 3) f->bytesPerSample = 4;
	f->numPlanes = (f->colorFamily == cfGray) ? 1 : 3;
	return 1;
}

/* ---- frames (padded strides to stress stride handling) ---- */
static int stride_for(int w) { int s = (w + 31) & ~31; return s > w ? s : s + 32; }
static VSFrame *VS_CC mock_newVideoFrame(const VSVideoFormat *fmt, int w, int h, const VSFrame *propSrc, VSCore *core) {
	(void)propSrc; (void)core;
	VSFrame *f = calloc(1, sizeof *f);
	f->fmt = *fmt; f->width = w; f->height = h;
	for (int p = 0; p < fmt->numPlanes; p++) {
		int pw = p ? w >> fmt->subSamplingW : w;
		int ph = p ? h >> fmt->subSamplingH : h;
		f->stride[p] = stride_for(pw);
		f->plane[p] = malloc((size_t)f->stride[p] * ph);
	}
	return f;
}
static void VS_CC mock_freeFrame(const VSFrame *cf) {
	VSFrame *f = (VSFrame *)cf;
	if (!f) return;
	for (int p = 0; p < 3; p++) free(f->plane[p]);
	free(f);
}
static ptrdiff_t VS_CC mock_getStride(const VSFrame *f, int plane) { return f->stride[plane]; }
static const uint8_t *VS_CC mock_getReadPtr(const VSFrame *f, int plane) { return f->plane[plane]; }
static uint8_t *VS_CC mock_getWritePtr(VSFrame *f, int plane) { return f->plane[plane]; }

/* ---- filter / node ---- */
static int is_reduced(int64_t num, int64_t den) { /* gcd(|num|,|den|) == 1 */
	int64_t a = num < 0 ? -num : num, b = den < 0 ? -den : den;
	while (b) { int64_t t = a % b; a = b; b = t; }
	return a == 1;
}
static int g_cvf_force_fail = 0; /* fault injection: force the createVideoFilter-fails contract (M3) */

/* Mirror VSCore::isValidVideoInfo just enough: positive dims/frames and an fps
 * fraction already in lowest terms. On an invalid vi the real core throws inside
 * the VSNode constructor *before* it takes ownership of inst/freeFn, and the
 * catch only sets an error - it creates no node and never frees inst (ownership
 * stays with the caller). Modelling that exactly is what lets this host exercise
 * the plugin's unreduced-fps error (M2) and its failure-path cleanup (M3). */
static void VS_CC mock_createVideoFilter(VSMap *out, const char *name, const VSVideoInfo *vi,
	VSFilterGetFrame getFrame, VSFilterFree freeFn, int filterMode,
	const VSFilterDependency *deps, int numDeps, void *inst, VSCore *core) {
	(void)name; (void)filterMode; (void)deps; (void)numDeps; (void)core;
	int valid = vi->width > 0 && vi->height > 0 && vi->numFrames > 0 &&
	            vi->fpsNum >= 0 && vi->fpsDen >= 0 &&
	            ((vi->fpsNum == 0 && vi->fpsDen == 0) ||
	             (vi->fpsNum > 0 && vi->fpsDen > 0 && is_reduced(vi->fpsNum, vi->fpsDen)));
	if (g_cvf_force_fail || !valid) {
		mock_mapSetError(out, "The VSVideoInfo structure passed by Source is invalid.");
		return; /* no node created; inst remains owned by the caller */
	}
	VSNode *node = calloc(1, sizeof *node);
	node->getFrame = getFrame; node->freeFn = freeFn; node->inst = inst; node->vi = *vi;
	mock_mapSetNode(out, "clip", node, 0);
}
static const VSVideoInfo *VS_CC mock_getVideoInfo(VSNode *node) { return &node->vi; }
static void VS_CC mock_setFilterError(const char *msg, VSFrameContext *ctx) {
	snprintf(ctx->error, sizeof ctx->error, "%s", msg); ctx->has_error = 1;
}

static VSAPI g_api;
static void init_api(void) {
	memset(&g_api, 0, sizeof g_api);
	g_api.createMap = mock_createMap;
	g_api.freeMap = mock_freeMap;
	g_api.mapSetError = mock_mapSetError;
	g_api.mapGetError = mock_mapGetError;
	g_api.mapGetData = mock_mapGetData;
	g_api.mapGetDataSize = mock_mapGetDataSize;
	g_api.mapSetData = mock_mapSetData;
	g_api.mapGetInt = mock_mapGetInt;
	g_api.mapGetIntSaturated = mock_mapGetIntSaturated;
	g_api.mapSetInt = mock_mapSetInt;
	g_api.mapGetNode = mock_mapGetNode;
	g_api.mapSetNode = mock_mapSetNode;
	g_api.getVideoFormatByID = mock_getVideoFormatByID;
	g_api.newVideoFrame = mock_newVideoFrame;
	g_api.freeFrame = mock_freeFrame;
	g_api.getStride = mock_getStride;
	g_api.getReadPtr = mock_getReadPtr;
	g_api.getWritePtr = mock_getWritePtr;
	g_api.createVideoFilter = mock_createVideoFilter;
	g_api.getVideoInfo = mock_getVideoInfo;
	g_api.setFilterError = mock_setFilterError;
}

/* ---- VSPLUGINAPI ---- */
static int VS_CC mock_configPlugin(const char *id, const char *ns, const char *name,
	int ver, int apiver, int flags, VSPlugin *plugin) {
	(void)ver; (void)apiver; (void)flags; (void)plugin;
	printf("  plugin: id=%s ns=%s name=\"%s\"\n", id, ns, name);
	return 1;
}
static int VS_CC mock_registerFunction(const char *name, const char *args, const char *rt,
	VSPublicFunction f, void *fdata, VSPlugin *plugin) {
	(void)args; (void)rt; (void)fdata; (void)plugin;
	printf("  registered: %s\n", name);
	if (!strcmp(name, "Source")) g_source_func = f;
	return 1;
}

static VSNode *call_source(const char *path, const char *stack, char *err, size_t errn) {
	VSMap *in = mock_createMap(), *out = mock_createMap();
	mock_mapSetData(in, "source", path, -1, 0, 0);
	if (stack) mock_mapSetData(in, "stack", stack, -1, 0, 0);
	VSCore core = {0};
	g_source_func(in, out, NULL, &core, &g_api);
	VSNode *node = NULL;
	const char *e = mock_mapGetError(out);
	if (e) { snprintf(err, errn, "%s", e); }
	else { int ee = 0; node = mock_mapGetNode(out, "clip", 0, &ee); if (ee) snprintf(err, errn, "no clip returned"); }
	mock_freeMap(in); mock_freeMap(out);
	return node;
}

/* Like call_source but also sets the swaplr int arg, so the view-swap semantics
 * can be driven through the real plugin entry point. */
static VSNode *call_source_swap(const char *path, const char *stack, int swaplr, char *err, size_t errn) {
	VSMap *in = mock_createMap(), *out = mock_createMap();
	mock_mapSetData(in, "source", path, -1, 0, 0);
	if (stack) mock_mapSetData(in, "stack", stack, -1, 0, 0);
	mock_mapSetInt(in, "swaplr", swaplr, 0);
	VSCore core = {0};
	g_source_func(in, out, NULL, &core, &g_api);
	VSNode *node = NULL;
	const char *e = mock_mapGetError(out);
	if (e) { snprintf(err, errn, "%s", e); }
	else { int ee = 0; node = mock_mapGetNode(out, "clip", 0, &ee); if (ee) snprintf(err, errn, "no clip returned"); }
	mock_freeMap(in); mock_freeMap(out);
	return node;
}

/* Drive Source with optional integer fps args (a present flag per field), so the
 * fps pairing semantics can be exercised. Returns the node (caller frees via
 * free_node) or NULL, and copies any error text into err. */
static VSNode *call_source_ints(const char *path, int have_num, int64_t fpsnum,
	int have_den, int64_t fpsden, char *err, size_t errn) {
	VSMap *in = mock_createMap(), *out = mock_createMap();
	mock_mapSetData(in, "source", path, -1, 0, 0);
	if (have_num) mock_mapSetInt(in, "fpsnum", fpsnum, 0);
	if (have_den) mock_mapSetInt(in, "fpsden", fpsden, 0);
	VSCore core = {0};
	g_source_func(in, out, NULL, &core, &g_api);
	VSNode *node = NULL;
	err[0] = 0;
	const char *e = mock_mapGetError(out);
	if (e) { snprintf(err, errn, "%s", e); }
	else { int ee = 0; node = mock_mapGetNode(out, "clip", 0, &ee); if (ee) snprintf(err, errn, "no clip returned"); }
	mock_freeMap(in); mock_freeMap(out);
	return node;
}

static void free_node(VSNode *n) {
	if (!n) return;
	VSCore core = {0};
	n->freeFn(n->inst, &core, &g_api);
	free(n);
}

/* M1: fpsnum/fpsden are a pair. A partially-specified or non-positive rate must
 * be rejected, not silently spliced with half of the default (e.g. 25/1001). */
static void test_fps_semantics(const char *path, int *fail) {
	char err[512];
	VSNode *n;

	n = call_source_ints(path, 0, 0, 0, 0, err, sizeof err); /* both omitted -> default */
	if (!n) { printf("FAIL[fps]: default rate errored: %s\n", err); *fail = 1; }
	else {
		const VSVideoInfo *vi = mock_getVideoInfo(n);
		if (vi->fpsNum != 24000 || vi->fpsDen != 1001) {
			printf("FAIL[fps]: default = %lld/%lld (want 24000/1001)\n",
				(long long)vi->fpsNum, (long long)vi->fpsDen); *fail = 1;
		} else printf("ok[fps]: default 24000/1001\n");
		free_node(n);
	}

	n = call_source_ints(path, 1, 30000, 1, 1001, err, sizeof err); /* both given, coprime */
	if (!n) { printf("FAIL[fps]: explicit 30000/1001 errored: %s\n", err); *fail = 1; }
	else {
		const VSVideoInfo *vi = mock_getVideoInfo(n);
		if (vi->fpsNum != 30000 || vi->fpsDen != 1001) {
			printf("FAIL[fps]: explicit -> %lld/%lld (want 30000/1001)\n",
				(long long)vi->fpsNum, (long long)vi->fpsDen); *fail = 1;
		} else printf("ok[fps]: explicit 30000/1001\n");
		free_node(n);
	}

	n = call_source_ints(path, 1, 25, 0, 0, err, sizeof err); /* lone fpsnum */
	if (n) {
		const VSVideoInfo *vi = mock_getVideoInfo(n);
		printf("FAIL[fps]: lone fpsnum=25 accepted -> %lld/%lld (want error)\n",
			(long long)vi->fpsNum, (long long)vi->fpsDen); *fail = 1; free_node(n);
	} else printf("ok[fps]: lone fpsnum rejected: %s\n", err);

	n = call_source_ints(path, 0, 0, 1, 2, err, sizeof err); /* lone fpsden */
	if (n) { printf("FAIL[fps]: lone fpsden=2 accepted (want error)\n"); *fail = 1; free_node(n); }
	else printf("ok[fps]: lone fpsden rejected: %s\n", err);

	n = call_source_ints(path, 1, -5, 1, 1, err, sizeof err); /* non-positive */
	if (n) { printf("FAIL[fps]: fpsnum=-5 accepted (want error)\n"); *fail = 1; free_node(n); }
	else printf("ok[fps]: non-positive fpsnum rejected: %s\n", err);

	/* M2: a non-coprime pair must be reduced before createVideoFilter, not
	 * forwarded raw (which the core rejects as an invalid VSVideoInfo). */
	n = call_source_ints(path, 1, 30000, 1, 1002, err, sizeof err);
	if (!n) { printf("FAIL[fps]: non-coprime 30000/1002 rejected: %s\n", err); *fail = 1; }
	else {
		const VSVideoInfo *vi = mock_getVideoInfo(n);
		if (vi->fpsNum != 5000 || vi->fpsDen != 167) {
			printf("FAIL[fps]: 30000/1002 -> %lld/%lld (want reduced 5000/167)\n",
				(long long)vi->fpsNum, (long long)vi->fpsDen); *fail = 1;
		} else printf("ok[fps]: 30000/1002 reduced to 5000/167\n");
		free_node(n);
	}
}

static uint64_t hash_plane0(const VSFrame *fr, int w, int h) {
	uint64_t x = 1469598103934665603ULL;
	const uint8_t *p = mock_getReadPtr(fr, 0);
	ptrdiff_t st = mock_getStride(fr, 0);
	for (int y = 0; y < h; y++)
		for (int c = 0; c < w; c++) { x ^= p[(ptrdiff_t)y * st + c]; x *= 1099511628211ULL; }
	return x;
}

/* Pull frame `n` of `node` through the real getFrame path and hash its luma. */
static int frameN_hash(VSNode *node, int n, uint64_t *out) {
	VSCore core = {0}; void *fd = NULL; VSFrameContext ctx = {0};
	const VSVideoInfo *vi = mock_getVideoInfo(node);
	const VSFrame *fr = node->getFrame(n, arInitial, node->inst, &fd, &ctx, &core, &g_api);
	if (!fr) return -1;
	*out = hash_plane0(fr, vi->width, vi->height);
	mock_freeFrame(fr);
	return 0;
}
static int frame0_hash(VSNode *node, uint64_t *out) { return frameN_hash(node, 0, out); }

/* M5: the "right" stack (dependent view alone) must actually map to MVC_RIGHT
 * and serve a different view than "base". On an MVC stream base and right share
 * per-view dimensions but differ in content; a broken mapping (right -> base)
 * would make them identical. Detect MVC via the tab layout stacking to 2x. */
static void test_right_view(const char *path, int *fail) {
	char err[512];
	VSNode *b = call_source(path, "base", err, sizeof err);
	VSNode *r = call_source(path, "right", err, sizeof err);
	VSNode *t = call_source(path, "tab", err, sizeof err);
	if (!b || !r || !t) { printf("FAIL[right]: could not open base/right/tab: %s\n", err); *fail = 1; goto done; }
	const VSVideoInfo *vib = mock_getVideoInfo(b), *vir = mock_getVideoInfo(r), *vit = mock_getVideoInfo(t);
	if (vir->width != vib->width || vir->height != vib->height) {
		printf("FAIL[right]: right dims %dx%d != base %dx%d\n", vir->width, vir->height, vib->width, vib->height);
		*fail = 1;
	}
	int is_mvc = vit->height == 2 * vib->height; /* tab stacks base over dependent */
	uint64_t hb = 0, hr = 0;
	if (frame0_hash(b, &hb) || frame0_hash(r, &hr)) { printf("FAIL[right]: getFrame failed\n"); *fail = 1; goto done; }
	if (is_mvc && hb == hr) {
		printf("FAIL[right]: right view identical to base on an MVC stream (mapping broken?)\n"); *fail = 1;
	} else {
		printf("ok[right]: right maps to the dependent view (mvc=%d, base=%016llx right=%016llx)\n",
			is_mvc, (unsigned long long)hb, (unsigned long long)hr);
	}
done:
	free_node(b); free_node(r); free_node(t);
}

/* swaplr swaps the two views in every layout. On an MVC stream base+swaplr must
 * equal right and right+swaplr must equal base; on a 2D stream (no dependent
 * view) swaplr is a no-op. */
static void test_swaplr(const char *path, int *fail) {
	char err[512];
	VSNode *b  = call_source(path, "base", err, sizeof err);
	VSNode *r  = call_source(path, "right", err, sizeof err);
	VSNode *t  = call_source(path, "tab", err, sizeof err);
	VSNode *bs = call_source_swap(path, "base", 1, err, sizeof err);
	VSNode *rs = call_source_swap(path, "right", 1, err, sizeof err);
	if (!b || !r || !t || !bs || !rs) { printf("FAIL[swaplr]: open failed: %s\n", err); *fail = 1; goto done; }
	const VSVideoInfo *vib = mock_getVideoInfo(b), *vit = mock_getVideoInfo(t);
	int is_mvc = vit->height == 2 * vib->height;
	uint64_t hb = 0, hr = 0, hbs = 0, hrs = 0;
	if (frameN_hash(b, 0, &hb) || frameN_hash(r, 0, &hr) ||
	    frameN_hash(bs, 0, &hbs) || frameN_hash(rs, 0, &hrs)) {
		printf("FAIL[swaplr]: getFrame failed\n"); *fail = 1; goto done;
	}
	int bad = 0;
	if (is_mvc) {
		if (hbs != hr) { printf("FAIL[swaplr]: base+swaplr != right\n"); bad = 1; }
		if (hrs != hb) { printf("FAIL[swaplr]: right+swaplr != base\n"); bad = 1; }
	} else if (hbs != hb) {
		printf("FAIL[swaplr]: 2D base+swaplr changed the view\n"); bad = 1;
	}
	if (bad) *fail = 1; else printf("ok[swaplr]: swaps the two views (mvc=%d)\n", is_mvc);
done:
	free_node(b); free_node(r); free_node(t); free_node(bs); free_node(rs);
}

/* L5: an unrecognized stack string must be rejected, not silently degraded to
 * the base view (a typo would otherwise yield a mono clip with no diagnostic).
 * The documented spellings and the base aliases must still be accepted. */
static void test_bad_stack(const char *path, int *fail) {
	char err[512];
	const char *bad[] = { "sbss", "top-bottom", "rihgt", "2d" };
	for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
		VSNode *n = call_source(path, bad[i], err, sizeof err);
		if (n) { printf("FAIL[stack]: '%s' accepted (want error)\n", bad[i]); *fail = 1; free_node(n); }
		else printf("ok[stack]: '%s' rejected: %s\n", bad[i], err);
	}
	const char *good[] = { "base", "left", "right", "tab", "sbs" };
	for (unsigned i = 0; i < sizeof good / sizeof *good; i++) {
		VSNode *n = call_source(path, good[i], err, sizeof err);
		if (!n) { printf("FAIL[stack]: '%s' rejected: %s\n", good[i], err); *fail = 1; }
		else { printf("ok[stack]: '%s' accepted\n", good[i]); free_node(n); }
	}
}

/* M3: when createVideoFilter fails, the node is never created and the core never
 * takes ownership of instanceData, so the plugin must free it itself. Force the
 * failure (g_cvf_force_fail) against a real, openable file so mvc_open actually
 * allocates the FilterData + MvcSource + decoder + seek index, then let
 * LeakSanitizer judge whether the plugin cleaned up. This mode frees everything
 * the host itself allocates, so any remaining leak is the plugin's. */
static int leak_check_mode(const char *path) {
	g_cvf_force_fail = 1;
	VSMap *in = mock_createMap(), *out = mock_createMap();
	mock_mapSetData(in, "source", path, -1, 0, 0);
	VSCore core = {0};
	g_source_func(in, out, NULL, &core, &g_api);
	int surfaced = mock_mapGetError(out) != NULL;
	int ee = 0; VSNode *node = mock_mapGetNode(out, "clip", 0, &ee);
	if (node) { free_node(node); } /* should not happen on the forced-fail path */
	mock_freeMap(in); mock_freeMap(out);
	int ok = surfaced && !node;
	if (ok) printf("leakcheck: forced createVideoFilter failure surfaced an error, no node\n");
	else printf("leakcheck: FAIL contract (surfaced=%d node=%d)\n", surfaced, node != NULL);
	return ok ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s <plugin.so> <file.264> <stack|__leakcheck__> [dumpframe dumpfile]\n", argv[0]); return 2; }
	const char *sofile = argv[1], *path = argv[2], *stack = argv[3];

	void *h = dlopen(sofile, RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
	VSInitPlugin init = (VSInitPlugin)dlsym(h, "VapourSynthPluginInit2");
	if (!init) { fprintf(stderr, "VapourSynthPluginInit2 not found: %s\n", dlerror()); return 1; }

	init_api();
	VSPLUGINAPI papi = {0};
	papi.configPlugin = mock_configPlugin;
	papi.registerFunction = mock_registerFunction;
	VSPlugin plugin = {0};
	init(&plugin, &papi);
	if (!g_source_func) { fprintf(stderr, "plugin did not register Source\n"); return 1; }

	/* dedicated failure-path leak check (run under LeakSanitizer) */
	if (!strcmp(stack, "__leakcheck__")) { int rc = leak_check_mode(path); dlclose(h); return rc; }

	int fail = 0;

	/* 1. error path: a bogus source must set an error, not a node */
	{
		char err[512] = "";
		VSNode *bad = call_source("/nonexistent/does-not-exist.264", "base", err, sizeof err);
		if (bad || !err[0]) { printf("FAIL: bogus source did not error\n"); fail = 1; }
		else printf("ok: bogus source errored: %s\n", err);
	}

	/* 2. open the real file */
	VSCore core = {0};
	char err[512] = "";
	VSNode *node = call_source(path, stack, err, sizeof err);
	if (!node) { fprintf(stderr, "Source failed: %s\n", err); return 1; }
	const VSVideoInfo *vi = mock_getVideoInfo(node);
	printf("vi: %dx%d  frames=%d  fps=%lld/%lld  cf=%d bits=%d subW=%d subH=%d planes=%d\n",
		vi->width, vi->height, vi->numFrames, (long long)vi->fpsNum, (long long)vi->fpsDen,
		vi->format.colorFamily, vi->format.bitsPerSample, vi->format.subSamplingW,
		vi->format.subSamplingH, vi->format.numPlanes);
	if (vi->width <= 0 || vi->height <= 0 || vi->numFrames <= 0) { printf("FAIL: bad vi\n"); fail = 1; }
	if (vi->format.colorFamily != cfYUV || vi->format.bitsPerSample != 8 ||
	    vi->format.subSamplingW != 1 || vi->format.subSamplingH != 1) { printf("FAIL: format is not YUV420P8\n"); fail = 1; }

	/* fps pairing semantics (M1) */
	test_fps_semantics(path, &fail);

	/* "right" (dependent view alone) coverage (M5) */
	test_right_view(path, &fail);

	/* "swaplr" (swap the two views in any layout) coverage */
	test_swaplr(path, &fail);

	/* unknown stack strings must be rejected, not silently degraded (L5) */
	test_bad_stack(path, &fail);

	/* helper to pull one frame through the real getFrame path */
	#define PULL(N) ({ void *fd = NULL; VSFrameContext ctx = {0}; \
		const VSFrame *fr = node->getFrame((N), arInitial, node->inst, &fd, &ctx, &core, &g_api); \
		if (!fr) { printf("FAIL: getFrame(%d) error: %s\n", (N), ctx.has_error ? ctx.error : "?"); fail = 1; } fr; })

	/* 3. pull a handful of frames (front and a middle one) */
	int probe[] = {0, 1, 2, vi->numFrames / 2, vi->numFrames - 1};
	for (unsigned i = 0; i < sizeof probe / sizeof *probe; i++) {
		int n = probe[i]; if (n < 0 || n >= vi->numFrames) continue;
		const VSFrame *fr = PULL(n);
		if (fr) {
			if (mock_getReadPtr(fr, 0) == NULL || mock_getStride(fr, 0) < vi->width) { printf("FAIL: frame %d planes\n", n); fail = 1; }
			mock_freeFrame(fr);
			printf("ok: frame %d decoded\n", n);
		}
	}

	/* 4. optional bit-exact dump of one frame (tightly packed) */
	if (argc >= 6) {
		int dn = atoi(argv[4]);
		const VSFrame *fr = PULL(dn);
		if (fr) {
			FILE *f = fopen(argv[5], "wb");
			for (int p = 0; p < 3; p++) {
				int pw = p ? vi->width >> 1 : vi->width;
				int ph = p ? vi->height >> 1 : vi->height;
				const uint8_t *pl = mock_getReadPtr(fr, p);
				ptrdiff_t st = mock_getStride(fr, p);
				for (int y = 0; y < ph; y++) fwrite(pl + (ptrdiff_t)y * st, 1, (size_t)pw, f);
			}
			fclose(f);
			mock_freeFrame(fr);
			fprintf(stderr, "dumped frame %d to %s\n", dn, argv[5]);
		}
	}

	node->freeFn(node->inst, &core, &g_api);
	free(node);
	dlclose(h);
	printf(fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
	return fail;
}
