/*
 * mvcsource - H.264/MVC decode core on edge264-mvc. See mvcsource.h.
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edge264.h"

/* A random-access seek point: an IDR access unit and the display index of the
 * first frame it produces. `nal` points at the start of the access unit's
 * leading NALs (AUD/SPS/PPS...), so re-feeding from here re-establishes the
 * parameter sets before the IDR. */
typedef struct { const uint8_t *nal; int frame; } SeekPoint;

/* A parameter-set NAL (SPS / PPS / subset-SPS) span in the mapped stream, in the
 * order it appears. On a seek the decoder is torn down and recreated, so the
 * parameter sets active at the seek point must be re-fed to the fresh decoder;
 * streams that carry the sets only once at the start do not repeat them per IDR. */
typedef struct { const uint8_t *nal, *end; } ParamNal;

struct MvcSource {
	uint8_t *map;          /* mmap base (or NULL if malloc'd) */
	size_t map_size;
	const uint8_t *start;  /* first NAL (past the leading start code) */
	const uint8_t *end;    /* one past the last byte */
	int n_threads;
	int swaplr;            /* emit the two views swapped (base <-> dependent) */
	int num_pics;          /* decoded source pictures (base primary); the output
	                          frame count is 2x this for MVC_ALT */
	MvcInfo info;

	SeekPoint *idx;
	int nidx, idxcap;

	ParamNal *ps;          /* parameter-set NALs in stream order */
	int nps, pscap;

	Edge264Decoder *dec;
	const uint8_t *nal;    /* current feed position */
	int next_out;          /* display index of the next frame get_frame will yield */
	int64_t last_poc;      /* DisplayPoc of the previous output in this run (INT64_MIN after a reset) */

	/* Last decoded source picture, cached so MVC_ALT emits both of its output
	 * frames (the two views) from one decode. edge264_get_frame(borrow=0) frames
	 * stay valid until the next edge264_decode_NAL, and a cache hit issues none;
	 * a decoder reset (edge264_free) invalidates them, so cur_valid is cleared
	 * there. cur_index is the source display index cur holds. */
	Edge264Frame cur;
	int cur_valid;
	int cur_index;
};

static void set_err(char *err, size_t n, const char *msg) {
	if (err && n) { snprintf(err, n, "%s", msg); }
}

/* --- NAL scan (indexing) -------------------------------------------------- */

static void idx_push(MvcSource *s, const uint8_t *nal, int frame) {
	if (s->nidx == s->idxcap) {
		int cap = s->idxcap ? s->idxcap * 2 : 256;
		SeekPoint *p = realloc(s->idx, (size_t)cap * sizeof *p);
		if (!p) return; /* out of memory: just stop adding seek points */
		s->idx = p; s->idxcap = cap;
	}
	s->idx[s->nidx].nal = nal;
	s->idx[s->nidx].frame = frame;
	s->nidx++;
}

static void ps_push(MvcSource *s, const uint8_t *nal, const uint8_t *end) {
	if (s->nps == s->pscap) {
		int cap = s->pscap ? s->pscap * 2 : 16;
		ParamNal *p = realloc(s->ps, (size_t)cap * sizeof *p);
		if (!p) return; /* out of memory: seeks may miss a parameter set, but no worse than before */
		s->ps = p; s->pscap = cap;
	}
	s->ps[s->nps].nal = nal;
	s->ps[s->nps].end = end;
	s->nps++;
}

/*
 * Single linear scan over the Annex-B NAL units to derive, without pixel
 * decoding: the number of output frames (= base primary coded pictures), the
 * presence of a dependent view, and one seek point per IDR. A base primary
 * picture is a type 1/5 NAL whose first_mb_in_slice == 0; ue(v)==0 is a single
 * '1' bit, so that is exactly the top bit of the first RBSP byte (emulation
 * prevention cannot affect the first byte).
 *
 * A base picture is only counted once a base-view SPS (type 7) and a PPS (type 8)
 * have appeared earlier in the stream. This mirrors the decoder, which returns
 * EBADMSG (and produces no frame) for a slice whose SPS/PPS is not yet
 * initialized: a stream cut mid-GOP begins with VCL slices that reference the
 * (now discarded) parameter sets of the previous GOP, and counting those would
 * overcount num_frames - shifting every later display index and running past the
 * real frames at the end. Cleanly demuxed streams start with SPS/PPS/IDR, so the
 * guard is inert for them.
 */
static void scan_index(MvcSource *s) {
	const uint8_t *p = s->start, *end = s->end;
	const uint8_t *au_start = s->start; /* start of the current access unit's leading NALs */
	int in_vcl = 0;                     /* did we just pass this AU's VCL NALs? */
	int frames = 0, is_mvc = 0;
	int seen_sps = 0, seen_pps = 0;     /* a base-view SPS+PPS must precede a counted slice */
	while (p < end) {
		int type = p[0] & 0x1f;
		int is_vcl = (type >= 1 && type <= 5) || type == 19 || type == 20;
		if (!is_vcl && in_vcl) { au_start = p; in_vcl = 0; } /* leading NAL of the next AU */
		if (type == 20 || type == 15)
			is_mvc = 1;
		if (type == 7)
			seen_sps = 1;
		else if (type == 8)
			seen_pps = 1;
		if (type == 7 || type == 8 || type == 13 || type == 15) { /* SPS/PPS/SPS-ext/subset-SPS */
			const uint8_t *sc = edge264_find_start_code(p, end, 0);
			ps_push(s, p, sc < end ? sc : end);
		}
		if ((type == 1 || type == 5) && (p + 1 < end) && (p[1] & 0x80) && seen_sps && seen_pps) { /* base primary picture */
			/* If this AU begins directly with its VCL NAL (no leading non-VCL
			 * NAL advanced au_start), the first slice itself is the AU start.
			 * Without this, au_start stays frozen at the previous boundary and
			 * every later seek point records the wrong byte offset. */
			if (in_vcl)
				au_start = p;
			if (type == 5)
				idx_push(s, au_start, frames);
			frames++;
			in_vcl = 1;
		} else if (is_vcl) {
			in_vcl = 1;
		}
		const uint8_t *sc = edge264_find_start_code(p, end, 0);
		p = (sc < end) ? sc + 3 : end;
	}
	s->num_pics = frames;
	s->info.is_mvc = is_mvc;
}

/* --- edge264 caller loop -------------------------------------------------- */

/*
 * Advance decoding until one frame is output. Returns 1 with *out filled, 0 at a
 * clean end of stream, or -1 on a hard decode error (message written to err).
 * Mirrors a robust player loop: skip ENOTSUP (unspecified NALs such as the
 * type-24 units on 3D Blu-rays), tolerate EBADMSG, and carry a progress guard so
 * an end-of-stream ENOBUFS cannot spin forever. ENOMEM/EINVAL are NOT skippable:
 * the NAL produced no frame, so swallowing it would drop a picture and shift
 * every later display index - fail loudly instead.
 */
/* Tripwire for the "blind output count == scan count" invariant: the decoder
 * emits pictures in strictly increasing display order within a run, so a
 * DisplayPoc that fails to advance means the output stream has diverged from
 * what scan_index counted (a dropped/duplicated picture or decoder drift) and
 * every later frame would be mislabeled. Fail loudly instead. Returns 1 on
 * success, 0 on a divergence (err set); last_poc is INT64_MIN after a reset. */
static int check_display_order(MvcSource *s, const Edge264Frame *out, char *err, size_t errsize) {
	if (s->last_poc != INT64_MIN && out->DisplayPoc <= s->last_poc) {
		set_err(err, errsize, "decoder output order diverged (display POC not increasing)");
		return 0;
	}
	s->last_poc = out->DisplayPoc;
	return 1;
}

static int decode_next_output(MvcSource *s, Edge264Frame *out, char *err, size_t errsize) {
	if (edge264_get_frame(s->dec, out, 0) == 0)
		return check_display_order(s, out, err, errsize) ? 1 : -1;
	int stuck = 0;
	for (;;) {
		int at_end = s->nal >= s->end;
		const uint8_t *buf = at_end ? s->end : s->nal;
		const uint8_t *e = at_end ? s->end : edge264_find_start_code(s->nal, s->end, 0);
		int r = edge264_decode_NAL(s->dec, buf, e, NULL, NULL);
		if (edge264_get_frame(s->dec, out, 0) == 0) {
			if (r != ENOBUFS && !at_end)
				s->nal = (e < s->end) ? e + 3 : s->end; /* avoid forming s->end + 3 (UB) */
			return check_display_order(s, out, err, errsize) ? 1 : -1;
		}
		if (r == ENOBUFS) {
			if (++stuck > 64) { s->nal = s->end; stuck = 0; } /* force the flush sentinel */
			continue;
		}
		stuck = 0;
		if (r == ENOMEM || r == EINVAL) { /* picture not produced: data loss, not a skip */
			set_err(err, errsize, r == ENOMEM ? "out of memory while decoding" : "decoder rejected input");
			return -1;
		}
		if (at_end)
			return 0; /* ENODATA: fully drained */
		/* mirror scan_index's guard: e can be s->end (last NAL), and forming
		 * s->end + 3 is UB (C11 6.5.6p8) even though it is only ever compared */
		s->nal = (e < s->end) ? e + 3 : s->end; /* success / ENOTSUP skip / EBADMSG tolerate */
	}
}

/* Reposition the decoder so the next output frame has display index >= target,
 * choosing the nearest usable seek point. The decoder is torn down and
 * recreated for a clean state: edge264_flush is documented for seeking but its
 * output queue is left in an end-of-stream state after a full drain (no shipped
 * harness exercises seek-then-decode), which would make get_frame keep
 * returning the last frame. free+alloc is unconditionally correct; seeks are
 * rare in the near-sequential access a source filter sees. */
static int reset_decoder(MvcSource *s) {
	edge264_free(&s->dec);
	s->dec = edge264_alloc(s->n_threads, NULL, NULL, 0, NULL, NULL, NULL);
	s->last_poc = INT64_MIN; /* display order restarts from the seek point */
	s->cur_valid = 0;        /* freeing the decoder invalidated cur's buffers */
	return s->dec != NULL;
}

/* Re-establish, on a freshly recreated decoder, the parameter sets a linear
 * decode would have seen before `sp_nal`: feed every SPS/PPS/subset-SPS NAL that
 * appears earlier in the stream, in order (later ones override earlier ones with
 * the same id, exactly as in a linear decode). Needed for streams that carry the
 * parameter sets only at the start rather than repeating them per IDR. */
static void refeed_param_sets(MvcSource *s, const uint8_t *sp_nal) {
	if (!s->dec) return;
	for (int i = 0; i < s->nps && s->ps[i].nal < sp_nal; i++)
		edge264_decode_NAL(s->dec, s->ps[i].nal, s->ps[i].end, NULL, NULL);
}

/* Returns 0 on success, -1 if the decoder could not be reallocated (OOM). On
 * failure s->nal/s->next_out are left untouched so a later request can retry the
 * allocation rather than decode against a NULL decoder. */
static int seek_to(MvcSource *s, int target) {
	int lo = 0, hi = s->nidx, best = -1;
	while (lo < hi) { /* largest seek point with frame <= target */
		int mid = (lo + hi) / 2;
		if (s->idx[mid].frame <= target) { best = mid; lo = mid + 1; }
		else hi = mid;
	}
	int sp_frame = best >= 0 ? s->idx[best].frame : 0;
	const uint8_t *sp_nal = best >= 0 ? s->idx[best].nal : s->start;
	/* only restart if we must go backwards or a closer seek point lies ahead */
	if (target < s->next_out || sp_frame > s->next_out) {
		if (!reset_decoder(s))
			return -1;
		refeed_param_sets(s, sp_nal);
		s->nal = sp_nal;
		s->next_out = sp_frame;
	}
	return 0;
}

/* --- view assembly -------------------------------------------------------- */

static void copy_plane(uint8_t *dst, ptrdiff_t dstride, const uint8_t *src,
	ptrdiff_t sstride, int w, int h) {
	for (int y = 0; y < h; y++)
		memcpy(dst + (ptrdiff_t)y * dstride, src + (ptrdiff_t)y * sstride, (size_t)w);
}

/* Write one plane (index 0=Y,1=U,2=V) of frame `f` into dst per `layout`. The
 * layout is passed in rather than read from s->info because MVC_ALT resolves,
 * per output frame, to a single-view MVC_BASE or MVC_RIGHT (see mvc_get_frame). */
static void assemble_plane(const MvcSource *s, const Edge264Frame *f, int plane,
	MvcLayout layout, uint8_t *dst, ptrdiff_t dstride) {
	int chroma = plane > 0;
	int w = chroma ? f->width_C : f->width_Y;
	int h = chroma ? f->height_C : f->height_Y;
	ptrdiff_t sstride = chroma ? f->stride_C : f->stride_Y;
	const uint8_t *base = f->samples[plane];
	/* a frame may miss its dependent view (lone base at a gap): reuse base */
	const uint8_t *dep = f->samples_mvc[plane] ? f->samples_mvc[plane] : base;
	/* swaplr swaps which physical view fills the left/first slot, so a stream
	 * authored right-eye-first can be flipped without changing the layout. */
	const uint8_t *left  = s->swaplr ? dep  : base;
	const uint8_t *right = s->swaplr ? base : dep;
	switch (layout) {
	case MVC_BASE:
		copy_plane(dst, dstride, left, sstride, w, h);
		break;
	case MVC_RIGHT:
		copy_plane(dst, dstride, right, sstride, w, h);
		break;
	case MVC_TAB:
		copy_plane(dst, dstride, left, sstride, w, h);
		copy_plane(dst + (ptrdiff_t)h * dstride, dstride, right, sstride, w, h);
		break;
	case MVC_SBS:
		copy_plane(dst, dstride, left, sstride, w, h);
		copy_plane(dst + w, dstride, right, sstride, w, h);
		break;
	case MVC_ALT: /* resolved per output frame in mvc_get_frame; a stray call
	                 falls back to the left view rather than leaving dst unfilled */
		copy_plane(dst, dstride, left, sstride, w, h);
		break;
	}
}

/* --- public API ----------------------------------------------------------- */

const MvcInfo *mvc_info(const MvcSource *s) { return &s->info; }

MvcSource *mvc_open(const char *path, int n_threads, MvcLayout layout, int swaplr,
	int64_t fps_num, int64_t fps_den, char *err, size_t errsize) {
	if (layout < MVC_BASE || layout > MVC_ALT) { /* else assemble_plane's switch fills nothing */
		set_err(err, errsize, "invalid layout");
		return NULL;
	}
	MvcSource *s = calloc(1, sizeof *s);
	if (!s) { set_err(err, errsize, "out of memory"); return NULL; }
	s->n_threads = n_threads;
	s->swaplr = swaplr != 0;

	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 4) {
		set_err(err, errsize, "cannot open input file");
		if (fd >= 0) close(fd);
		free(s);
		return NULL;
	}
	s->map_size = (size_t)st.st_size;
	s->map = mmap(NULL, s->map_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (s->map == MAP_FAILED) {
		set_err(err, errsize, "cannot mmap input file");
		free(s);
		return NULL;
	}
	const uint8_t *b = s->map;
	if (!(b[0] == 0 && b[1] == 0 && (b[2] == 1 || (b[2] == 0 && b[3] == 1)))) {
		set_err(err, errsize, "input is not an Annex-B H.264 elementary stream (no start code)");
		mvc_close(s);
		return NULL;
	}
	s->start = b + 3 + (b[2] == 0);
	s->end = b + s->map_size;

	scan_index(s);
	if (s->num_pics <= 0) {
		set_err(err, errsize, "no decodable frames found");
		mvc_close(s);
		return NULL;
	}
	s->info.fps_num = fps_num > 0 ? fps_num : 24000;
	s->info.fps_den = fps_den > 0 ? fps_den : 1001;
	/* the two-view layouts need a dependent view; on a 2D stream they degrade to
	 * the base (mono) view. */
	int two_view = (layout == MVC_TAB || layout == MVC_SBS || layout == MVC_ALT);
	s->info.layout = (two_view && !s->info.is_mvc) ? MVC_BASE : layout;
	/* MVC_ALT interleaves both views into one clip: twice the frames, and twice
	 * the frame rate so the clip keeps the source's wall-clock duration. */
	if (s->info.layout == MVC_ALT) {
		s->info.fps_num *= 2;
		s->info.num_frames = s->num_pics * 2;
	} else {
		s->info.num_frames = s->num_pics;
	}

	/* decode the first frame for exact (post-crop) per-view dimensions */
	s->dec = edge264_alloc(n_threads, NULL, NULL, 0, NULL, NULL, NULL);
	if (!s->dec) { set_err(err, errsize, "edge264_alloc failed"); mvc_close(s); return NULL; }
	s->nal = s->start;
	s->next_out = 0;
	s->last_poc = INT64_MIN;
	Edge264Frame f;
	int rc = decode_next_output(s, &f, err, errsize);
	if (rc <= 0) {
		if (rc == 0) set_err(err, errsize, "failed to decode the first frame"); /* rc<0: err already set */
		mvc_close(s);
		return NULL;
	}
	s->next_out = 1;
	/* keep the first picture cached: the decoder is untouched until the first
	 * get_frame, so frame 0 is served without a backward seek + re-decode */
	s->cur = f;
	s->cur_valid = 1;
	s->cur_index = 0;
	s->info.base_width = f.width_Y;
	s->info.base_height = f.height_Y;
	switch (s->info.layout) {
	case MVC_TAB: s->info.width = f.width_Y; s->info.height = f.height_Y * 2; break;
	case MVC_SBS: s->info.width = f.width_Y * 2; s->info.height = f.height_Y; break;
	default:      s->info.width = f.width_Y; s->info.height = f.height_Y; break;
	}
	return s;
}

/* Ensure source picture `src_n` is decoded and cached in s->cur, seeking if
 * needed. The cache lets MVC_ALT serve both views of a picture from a single
 * decode (the second view is a cache hit, so it issues no edge264_decode_NAL and
 * cur stays valid). Returns 0 on success, -1 on error (message in err). */
static int ensure_source_frame(MvcSource *s, int src_n, char *err, size_t errsize) {
	if (s->cur_valid && s->cur_index == src_n)
		return 0;
	if (seek_to(s, src_n) < 0) {
		set_err(err, errsize, "failed to allocate the decoder");
		return -1;
	}
	for (;;) {
		Edge264Frame f;
		int rc = decode_next_output(s, &f, err, errsize);
		if (rc <= 0) {
			if (rc == 0) set_err(err, errsize, "unexpected end of stream while seeking"); /* rc<0: err already set */
			return -1;
		}
		int idx = s->next_out++;
		if (idx == src_n) {
			s->cur = f;
			s->cur_valid = 1;
			s->cur_index = idx;
			return 0;
		}
	}
}

int mvc_get_frame(MvcSource *s, int n,
	uint8_t *dstY, ptrdiff_t strideY,
	uint8_t *dstU, ptrdiff_t strideU,
	uint8_t *dstV, ptrdiff_t strideV,
	char *err, size_t errsize) {
	if (n < 0 || n >= s->info.num_frames) {
		set_err(err, errsize, "frame index out of range");
		return -1;
	}
	/* MVC_ALT produces two output frames per source picture: even -> first view,
	 * odd -> second view. Every other layout maps the output index straight through. */
	MvcLayout eff = s->info.layout;
	int src_n = n;
	if (s->info.layout == MVC_ALT) {
		src_n = n >> 1;
		eff = (n & 1) ? MVC_RIGHT : MVC_BASE;
	}
	if (ensure_source_frame(s, src_n, err, errsize) < 0)
		return -1;
	assemble_plane(s, &s->cur, 0, eff, dstY, strideY);
	assemble_plane(s, &s->cur, 1, eff, dstU, strideU);
	assemble_plane(s, &s->cur, 2, eff, dstV, strideV);
	return 0;
}

void mvc_close(MvcSource *s) {
	if (!s) return;
	if (s->dec) edge264_free(&s->dec);
	free(s->idx);
	free(s->ps);
	if (s->map && s->map != MAP_FAILED) munmap(s->map, s->map_size);
	free(s);
}
