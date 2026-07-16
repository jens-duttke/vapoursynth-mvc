/*
 * avisynth-mvc - AviSynth+ source filter exposing edge264-mvc as MVCSource().
 *
 * Uses the AviSynth *C* interface (avisynth_c.h), not the C++ API, on purpose:
 * the C ABI is stable across compilers, so this MinGW-cross-built .dll loads in
 * the official MSVC-built Windows AviSynth+. A C++ plugin (AvisynthPluginInit3 +
 * IScriptEnvironment) would not - GCC and MSVC lay out the IScriptEnvironment
 * vtable differently (its virtual destructor takes two slots under the Itanium
 * ABI, one under MSVC), so AddFunction would dispatch to the wrong method and the
 * function would silently fail to register. Thin glue over the decode core in
 * mvcsource.c; all the real logic lives there.
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "avisynth_c.h"
#include "mvcsource.h"

/* Returns a layout (0..4), or -1 for an unrecognized string so the caller can
 * report it instead of silently degrading a typo to the base (mono) view. Same
 * spellings as the VapourSynth plugin's parse_layout. */
static int parse_layout(const char *s) {
	if (!s || !strcasecmp(s, "base") || !strcasecmp(s, "left") || !strcasecmp(s, "l")) return MVC_BASE;
	if (!strcasecmp(s, "right") || !strcasecmp(s, "r")) return MVC_RIGHT;
	if (!strcasecmp(s, "tab") || !strcasecmp(s, "tb") || !strcasecmp(s, "topbottom")) return MVC_TAB;
	if (!strcasecmp(s, "sbs") || !strcasecmp(s, "lr") || !strcasecmp(s, "sidebyside")) return MVC_SBS;
	if (!strcasecmp(s, "alt") || !strcasecmp(s, "alternate") || !strcasecmp(s, "alternating")) return MVC_ALT;
	return -1; /* unknown */
}

/* --- filter callbacks (plain C function pointers: ABI-stable) -------------- */

/* MT_SERIALIZED (=3 in the C++ MtMode enum) has no named constant in the C API.
 * The core mutates internal state per request (file position, decoder,
 * seek-by-reset), so it must run serialized - the counterpart to VapourSynth's
 * fmUnordered. */
#define MVC_MT_SERIALIZED 3

static AVS_VideoFrame *AVSC_CC mvc_cb_get_frame(AVS_FilterInfo *fi, int n) {
	MvcSource *src = (MvcSource *)fi->user_data;
	AVS_VideoFrame *f = avs_new_video_frame_a(fi->env, &fi->vi, AVS_FRAME_ALIGN);
	char err[256];
	/* AVS_PLANAR_U/V give the logical U/V planes; AviSynth's YV12 stores V before
	 * U physically, but the plane constants hide that, so no manual swap. */
	if (mvc_get_frame(src, n,
			(uint8_t *)avs_get_write_ptr_p(f, AVS_PLANAR_Y), avs_get_pitch_p(f, AVS_PLANAR_Y),
			(uint8_t *)avs_get_write_ptr_p(f, AVS_PLANAR_U), avs_get_pitch_p(f, AVS_PLANAR_U),
			(uint8_t *)avs_get_write_ptr_p(f, AVS_PLANAR_V), avs_get_pitch_p(f, AVS_PLANAR_V),
			err, sizeof err)) {
		/* SaveString keeps the message alive past this call; AviSynth reads
		 * fi->error after get_frame returns NULL. */
		fi->error = avs_save_string(fi->env, err, -1);
		avs_release_video_frame(f);
		return NULL;
	}
	return f;
}

static int AVSC_CC mvc_cb_get_parity(AVS_FilterInfo *fi, int n) {
	(void)fi; (void)n;
	return 1; /* progressive */
}

static int AVSC_CC mvc_cb_get_audio(AVS_FilterInfo *fi, void *buf, int64_t start, int64_t count) {
	(void)fi; (void)buf; (void)start; (void)count;
	return 0; /* no audio */
}

static int AVSC_CC mvc_cb_set_cache_hints(AVS_FilterInfo *fi, int cachehints, int frame_range) {
	(void)fi; (void)frame_range;
	return cachehints == AVS_CACHE_GET_MTMODE ? MVC_MT_SERIALIZED : 0;
}

static void AVSC_CC mvc_cb_free_filter(AVS_FilterInfo *fi) {
	mvc_close((MvcSource *)fi->user_data);
}

/* --- MVCSource(source[, stack, threads, fpsnum, fpsden, swaplr, cachesize]) - */

static AVS_Value AVSC_CC Create_MVCSource(AVS_ScriptEnvironment *env, AVS_Value args, void *user_data) {
	(void)user_data;
	AVS_Value a_source = avs_array_elt(args, 0);
	if (!avs_defined(a_source))
		return avs_new_value_error("MVCSource: 'source' (path to an .264/.h264 Annex-B file) is required");
	const char *source = avs_as_string(a_source);
	AVS_Value a_stack = avs_array_elt(args, 1);
	const char *stack = avs_defined(a_stack) ? avs_as_string(a_stack) : "base";
	int layout = parse_layout(stack);
	if (layout < 0) {
		char buf[128];
		snprintf(buf, sizeof buf, "MVCSource: unknown stack mode '%s' (use base/right/tab/sbs/alt)", stack);
		return avs_new_value_error(avs_save_string(env, buf, -1));
	}
	/* threads: edge264 internal decode parallelism. Default -1 (auto-detect cores):
	 * this filter reports MVC_MT_SERIALIZED, so GetFrame is serialised and internal
	 * MT is safe, bit-exact, and speeds up a seek's forward re-decode. 0 = single. */
	AVS_Value a_threads = avs_array_elt(args, 2);
	int threads = avs_defined(a_threads) ? avs_as_int(a_threads) : -1;
	/* fpsnum/fpsden are a pair: reject a half-specified or non-positive rate
	 * rather than splice a user value with half of the default. Only both-absent
	 * reaches mvc_open as 0/0, which keeps its 24000/1001 default. */
	AVS_Value a_fpsnum = avs_array_elt(args, 3);
	AVS_Value a_fpsden = avs_array_elt(args, 4);
	int have_num = avs_defined(a_fpsnum), have_den = avs_defined(a_fpsden);
	if (have_num != have_den)
		return avs_new_value_error("MVCSource: fpsnum and fpsden must be specified together");
	int64_t fpsnum = have_num ? (int64_t)avs_as_int(a_fpsnum) : 0;
	int64_t fpsden = have_den ? (int64_t)avs_as_int(a_fpsden) : 0;
	if ((have_num && fpsnum <= 0) || (have_den && fpsden <= 0))
		return avs_new_value_error("MVCSource: fpsnum and fpsden must be positive");
	AVS_Value a_swaplr = avs_array_elt(args, 5);
	int swaplr = (avs_defined(a_swaplr) && avs_as_bool(a_swaplr)) ? 1 : 0;
	/* cachesize: decoded-frame cache ceiling in MiB (0/absent = core default).
	 * Larger = fewer re-seeks on a backward / Reverse() pass over a long GOP. */
	AVS_Value a_cachesize = avs_array_elt(args, 6);
	int cachesize = avs_defined(a_cachesize) ? avs_as_int(a_cachesize) : 0;

	char emsg[256];
	MvcSource *src = mvc_open(source, threads, (MvcLayout)layout, swaplr, fpsnum, fpsden, cachesize, emsg, sizeof emsg);
	if (!src) {
		char buf[320];
		snprintf(buf, sizeof buf, "MVCSource: %s", emsg);
		return avs_new_value_error(avs_save_string(env, buf, -1));
	}
	const MvcInfo *info = mvc_info(src);

	/* Source filter: no child clip, so store_child = 0 and every callback is
	 * defined (AviSynth would otherwise forward undefined ones to the child). */
	AVS_FilterInfo *fi = NULL;
	AVS_Clip *clip = avs_new_c_filter(env, &fi, avs_void, 0);
	if (!clip) {
		mvc_close(src);
		return avs_new_value_error("MVCSource: avs_new_c_filter failed");
	}
	memset(&fi->vi, 0, sizeof fi->vi);
	fi->vi.width = info->width;
	fi->vi.height = info->height;
	fi->vi.num_frames = info->num_frames;
	fi->vi.pixel_type = AVS_CS_YV12; /* YUV420P8, matching the core's output */
	fi->vi.fps_numerator = (unsigned)info->fps_num;
	fi->vi.fps_denominator = (unsigned)info->fps_den;
	fi->vi.audio_samples_per_second = 0; /* no audio */
	fi->get_frame = mvc_cb_get_frame;
	fi->get_parity = mvc_cb_get_parity;
	fi->get_audio = mvc_cb_get_audio;
	fi->set_cache_hints = mvc_cb_set_cache_hints;
	fi->free_filter = mvc_cb_free_filter;
	fi->user_data = src;

	AVS_Value ret = avs_new_value_clip(clip);
	avs_release_clip(clip); /* the returned value holds its own reference */
	return ret;
}

/* --- plugin entry (C interface: no vtables, ABI-stable across compilers) --- */

#if defined(_WIN32) || defined(_WIN64)
	#define MVC_PLUGIN_EXPORT __declspec(dllexport)
#else
	#define MVC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

MVC_PLUGIN_EXPORT const char *AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment *env) {
	avs_add_function(env, "MVCSource",
		"[source]s[stack]s[threads]i[fpsnum]i[fpsden]i[swaplr]b[cachesize]i",
		Create_MVCSource, 0);
	return "MVCSource: H.264 MVC (3D) and AVC source, built on edge264-mvc";
}
