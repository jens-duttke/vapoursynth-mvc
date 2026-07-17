/*
 * mvcsource - H.264/MVC decode core on edge264-mvc. See mvcsource.h.
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include "cache_budget.h"
#include "h264poc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#include "edge264.h"

/*
 * A random-access point to seek to: an IDR, or an open-GOP recovery point (a
 * non-IDR I picture carrying a recovery_point SEI - what Blu-ray uses for entry
 * points between IDRs). `nal` points at the start of the access unit's leading
 * NALs (AUD/SPS/PPS...), so re-feeding from here re-establishes the parameter
 * sets before the picture.
 *
 * The two indices differ only at an open-GOP recovery point, and the difference
 * is the whole reason this type is not just an offset + a number:
 *
 *   frame      display index of the FIRST picture a cold decode here yields.
 *   valid_from display index of the first picture that is guaranteed CORRECT.
 *
 * At an IDR they are equal: it resets the DPB, so its own picture comes out
 * first. At a recovery point they are not. Its leading pictures follow it in
 * decode order but precede it in display order and reference the previous GOP,
 * which a cold decode has not seen - so the decoder emits (valid_from - frame)
 * wrong pictures before reaching the recovery point itself. Those must be
 * decoded (they carry no useful output but the decoder produces them) and then
 * discarded, never cached or served. valid_from is the recovery point's own
 * display index, derived from the picture order count during the scan; it is
 * what a seek binary-searches on, and what makes a recovery point a usable seek
 * target at all. Measured on a real 3D Blu-ray: 341 recovery points vs 103 IDRs,
 * and a cold decode from every one of them lands bit-exact on valid_from.
 */
typedef struct { const uint8_t *nal; int frame, valid_from; } SeekPoint;

/* A parameter-set NAL (SPS / PPS / subset-SPS) span in the mapped stream, in the
 * order it appears. On a seek the decoder is torn down and recreated, so the
 * parameter sets active at the seek point must be re-fed to the fresh decoder;
 * streams that carry the sets only once at the start do not repeat them per IDR. */
typedef struct { const uint8_t *nal, *end; } ParamNal;

/* A cached decoded source picture: an independent copy of the base (and, for
 * MVC, the dependent) view's packed planes, plus an Edge264Frame view over that
 * copy. Because the copy is independent of the decoder, it survives edge264_free
 * (a seek) and lets backward / repeat access - the pathological case for a
 * source filter, e.g. AviSynth Reverse() - hit RAM instead of re-decoding a GOP. */
typedef struct {
	int index;           /* source picture index held, or -1 if empty */
	uint8_t *buf;        /* packed base [+ dependent] Y/U/V planes */
	size_t bufcap;
	Edge264Frame frame;  /* view over buf: samples/samples_mvc + packed strides */
} FrameSlot;

/* Decoded-frame cache: a ring of the most recently produced pictures, so a
 * Reverse() / backward pass over a GOP is served from RAM instead of re-decoding
 * from the preceding IDR (the dominant cost of backward access on a 3D Blu-ray,
 * whose IDR spacing can exceed 600 frames). The byte budget (cache_budget.h) is a
 * ceiling, not an up-front reservation - slot buffers are allocated lazily - and
 * MAX_SLOTS is high enough that the byte budget, not the slot count, is the real
 * limit for normal frame sizes. */
#define FRAME_CACHE_MAX_SLOTS 4096

struct MvcSource {
	uint8_t *map;          /* mapped file base (see map_file_ro), or NULL */
	size_t map_size;
	const uint8_t *start;  /* first NAL (past the leading start code) */
	const uint8_t *end;    /* one past the last byte */
	int n_threads;
	size_t cache_budget;   /* decoded-frame cache ceiling in bytes (see ring_init) */
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
	int valid_from;        /* display index from which this decode run's output is
	                          correct: below it sit the leading pictures of the
	                          recovery point we started at (see SeekPoint) */
	int64_t last_poc;      /* DisplayPoc of the previous output in this run (INT64_MIN after a reset) */

	/* Ring of recently decoded source pictures (independent copies), so backward /
	 * repeat access serves from RAM instead of re-decoding. Sized once from the
	 * first frame to the memory budget (ring_init). The copies survive a decoder
	 * reset, so a seek that recreates the decoder does not invalidate them. cur is
	 * the picture mvc_get_frame currently assembles from (a slot's view, or the
	 * just-decoded borrowed frame if caching hit OOM). */
	FrameSlot *slots;
	int ring_cap;        /* number of slots (0 until ring_init) */
	int ring_head;       /* round-robin insert position */
	Edge264Frame cur;
};

static void set_err(char *err, size_t n, const char *msg) {
	if (err && n) { snprintf(err, n, "%s", msg); }
}

/* --- read-only file mapping (platform shim) ------------------------------- */

/* Map `path` read-only into memory; returns the base pointer (with *size set) or
 * NULL on failure (open error, or a file too short to hold a start code). Both
 * backends map lazily - a multi-GB .264 is paged in on demand, not read into RAM
 * up front - so the core's linear NAL scan and random-access seeks stay cheap.
 * *mtime is set to the file's last-write time (platform-native units) for use as
 * an index-cache freshness key; its only requirement is that it changes when the
 * file's contents change, and it is only ever compared against a value this same
 * platform wrote earlier, so the differing Win32/POSIX epochs do not matter. */
static uint8_t *map_file_ro(const char *path, size_t *size, int64_t *mtime) {
	*mtime = 0;
#ifdef _WIN32
	HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE) return NULL;
	LARGE_INTEGER sz;
	FILETIME ftw;
	if (!GetFileSizeEx(hf, &sz) || sz.QuadPart < 4) { CloseHandle(hf); return NULL; }
	if (GetFileTime(hf, NULL, NULL, &ftw))
		*mtime = ((int64_t)ftw.dwHighDateTime << 32) | ftw.dwLowDateTime;
	HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
	CloseHandle(hf); /* the mapping object holds its own reference to the file */
	if (!hm) return NULL;
	void *p = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
	CloseHandle(hm); /* the view keeps the file mapped until UnmapViewOfFile */
	if (!p) return NULL;
	*size = (size_t)sz.QuadPart;
	return (uint8_t *)p;
#else
	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 4) {
		if (fd >= 0) close(fd);
		return NULL;
	}
	*mtime = (int64_t)st.st_mtime;
	void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd); /* the mapping survives closing the fd */
	if (p == MAP_FAILED) return NULL;
	*size = (size_t)st.st_size;
	return (uint8_t *)p;
#endif
}

static void unmap_file(uint8_t *p, size_t size) {
	if (!p) return;
#ifdef _WIN32
	(void)size; /* UnmapViewOfFile takes only the base address */
	UnmapViewOfFile(p);
#else
	munmap(p, size);
#endif
}

/* --- on-disk index cache -------------------------------------------------- */

/*
 * scan_index reads the whole file once (it must, to find every VCL NAL and
 * count the pictures), which on a large stream on a slow disk is the entire
 * cost of opening - and it repeats on every reopen. This sidecar caches the
 * scan result next to the source (`<source>.mvcidx`, the convention lsmash's
 * `.lwi` established), keyed on the source's size + last-write time + a format
 * magic, so a reopen of an unchanged file skips the scan entirely. Only the
 * scan is cached, not decoding: mvc_open still decodes the first frame for exact
 * dimensions (that touches one GOP, not the whole file). Writing is best-effort
 * - a read-only directory just means no cache, never a failure to open - and a
 * short/corrupt cache fails the size/mtime check and triggers a fresh scan.
 * Machine-local artifact: written and read by the same build, so native field
 * sizes/order are fine (the magic gates any future format change).
 */
#define MVCIDX_MAGIC "MVCIDX04" /* bump the trailing digits to invalidate old caches (04: seek points are back on every random-access point, now carrying the POC-derived valid_from that 02 lacked) */

struct idx_hdr {
	char     magic[8];   /* MVCIDX_MAGIC, no NUL */
	uint64_t src_size;
	int64_t  src_mtime;
	int32_t  num_pics;
	int32_t  is_mvc;
	int32_t  nidx;       /* seek points */
	int32_t  nps;        /* parameter-set NALs */
}; /* 40 bytes, naturally aligned: no padding */

/* Load the cache at cpath into s (idx/ps/num_pics/is_mvc). Returns 1 only if the
 * cache is present, well-formed and matches this exact source (size + mtime);
 * any mismatch or I/O/format error returns 0 so the caller falls back to a
 * scan. All stored offsets are validated to lie within the mapping before a
 * pointer is formed, so a corrupt cache cannot point the decoder out of bounds. */
static int load_index_cache(MvcSource *s, const char *cpath, uint64_t src_size, int64_t src_mtime) {
	FILE *f = fopen(cpath, "rb");
	if (!f) return 0;
	struct idx_hdr h;
	int ok = 0;
	int64_t *ioff = NULL, *poff = NULL, *plen = NULL;
	int32_t *ifr = NULL, *ivf = NULL;
	SeekPoint *idx = NULL;
	ParamNal *ps = NULL;
	if (fread(&h, sizeof h, 1, f) != 1)
		goto done;
	if (memcmp(h.magic, MVCIDX_MAGIC, 8) != 0 || h.src_size != src_size ||
	    h.src_mtime != src_mtime || h.num_pics <= 0 || h.nidx < 0 || h.nps < 0)
		goto done;
	/* Caps guard against a corrupt/hostile count: nidx/nps bound the allocations,
	 * and num_pics bounds the frame total - unbounded it would give a wrong count
	 * and, via the MVC_ALT `num_pics * 2`, overflow the int num_frames negative so
	 * the clip rejects every frame. 1<<28 is far above any real stream and keeps
	 * the doubling within int (2^29 < INT_MAX). */
	if (h.nidx > (1 << 28) || h.nps > (1 << 26) || h.num_pics > (1 << 28))
		goto done;
	if (h.nidx) {
		ioff = malloc((size_t)h.nidx * sizeof *ioff);
		ifr = malloc((size_t)h.nidx * sizeof *ifr);
		ivf = malloc((size_t)h.nidx * sizeof *ivf);
		idx = malloc((size_t)h.nidx * sizeof *idx);
		if (!ioff || !ifr || !ivf || !idx) goto done;
		if (fread(ioff, sizeof *ioff, h.nidx, f) != (size_t)h.nidx ||
		    fread(ifr, sizeof *ifr, h.nidx, f) != (size_t)h.nidx ||
		    fread(ivf, sizeof *ivf, h.nidx, f) != (size_t)h.nidx) goto done;
	}
	if (h.nps) {
		poff = malloc((size_t)h.nps * sizeof *poff);
		plen = malloc((size_t)h.nps * sizeof *plen);
		ps = malloc((size_t)h.nps * sizeof *ps);
		if (!poff || !plen || !ps) goto done;
		if (fread(poff, sizeof *poff, h.nps, f) != (size_t)h.nps ||
		    fread(plen, sizeof *plen, h.nps, f) != (size_t)h.nps) goto done;
	}
	/* Validate the seek points as well as their byte offsets: seek_to binary-searches
	 * idx[].valid_from assuming it is in range and strictly increasing, and trusts
	 * idx[].frame as the display index the decoder's first output there carries. A
	 * corrupt/hostile array that is negative, >= num_pics, or non-monotone would make
	 * the search pick a seek point whose indices are inconsistent with the picture its
	 * offset points at, so a seek lands on the wrong display position and the clip
	 * serves the wrong frame. valid_from must additionally be >= frame, the invariant
	 * the scan establishes (a recovery point's own display index is at or after the
	 * first picture a cold decode there yields); a cache violating it would make
	 * ensure_source_frame discard real frames or serve leading-picture garbage. Reject
	 * the cache (fall back to a fresh scan) on any such value. */
	int prev_frame = -1, prev_valid = -1;
	for (int i = 0; i < h.nidx; i++) {
		if (ioff[i] < 0 || (uint64_t)ioff[i] >= src_size) goto done;
		if (ifr[i] <= prev_frame || ifr[i] >= h.num_pics) goto done; /* out of range or non-monotone */
		if (ivf[i] < ifr[i] || ivf[i] <= prev_valid || ivf[i] >= h.num_pics) goto done;
		prev_frame = ifr[i];
		prev_valid = ivf[i];
		idx[i].nal = s->map + ioff[i];
		idx[i].frame = ifr[i];
		idx[i].valid_from = ivf[i];
	}
	for (int i = 0; i < h.nps; i++) {
		if (poff[i] < 0 || plen[i] < 0 || (uint64_t)poff[i] + (uint64_t)plen[i] > src_size) goto done;
		ps[i].nal = s->map + poff[i];
		ps[i].end = s->map + poff[i] + plen[i];
	}
	s->idx = idx; s->nidx = s->idxcap = h.nidx; idx = NULL; /* ownership transferred */
	s->ps = ps; s->nps = s->pscap = h.nps; ps = NULL;
	s->num_pics = h.num_pics;
	s->info.is_mvc = h.is_mvc;
	ok = 1;
done:
	free(ioff); free(ifr); free(ivf); free(poff); free(plen);
	free(idx); free(ps); /* NULL after a successful transfer, so no double free */
	fclose(f);
	return ok;
}

/* Write s's scan result to cpath (best-effort). A failure to open (read-only
 * media) is silently ignored; a write failure removes the partial file so a
 * later open re-scans rather than trusting a truncated cache. */
static void save_index_cache(const MvcSource *s, const char *cpath, uint64_t src_size, int64_t src_mtime) {
	FILE *f = fopen(cpath, "wb");
	if (!f) return;
	struct idx_hdr h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, MVCIDX_MAGIC, 8);
	h.src_size = src_size;
	h.src_mtime = src_mtime;
	h.num_pics = s->num_pics;
	h.is_mvc = s->info.is_mvc;
	h.nidx = s->nidx;
	h.nps = s->nps;
	int ok = fwrite(&h, sizeof h, 1, f) == 1;
	for (int i = 0; i < s->nidx && ok; i++) { int64_t o = s->idx[i].nal - s->map; ok = fwrite(&o, sizeof o, 1, f) == 1; }
	for (int i = 0; i < s->nidx && ok; i++) { int32_t r = s->idx[i].frame;        ok = fwrite(&r, sizeof r, 1, f) == 1; }
	for (int i = 0; i < s->nidx && ok; i++) { int32_t v = s->idx[i].valid_from;   ok = fwrite(&v, sizeof v, 1, f) == 1; }
	for (int i = 0; i < s->nps && ok; i++) { int64_t o = s->ps[i].nal - s->map;   ok = fwrite(&o, sizeof o, 1, f) == 1; }
	for (int i = 0; i < s->nps && ok; i++) { int64_t l = s->ps[i].end - s->ps[i].nal; ok = fwrite(&l, sizeof l, 1, f) == 1; }
	if (fclose(f) != 0) ok = 0;
	if (!ok) remove(cpath);
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
	/* Correct as-is for an IDR (no leading pictures); flush_cvs refines it for an
	 * open-GOP recovery point once that sequence's picture order counts are known. */
	s->idx[s->nidx].valid_from = frame;
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

/* Picture order counts of the coded video sequence currently being scanned, in
 * decode order. Only one sequence is held at a time (it is consumed at the next
 * IDR), so this peaks at the longest IDR-to-IDR span, not the whole stream. */
typedef struct { int32_t *v; int n, cap; } PocBuf;

static int pocbuf_push(PocBuf *b, int32_t poc) {
	if (b->n == b->cap) {
		int cap = b->cap ? b->cap * 2 : 1024;
		int32_t *v = realloc(b->v, (size_t)cap * sizeof *v);
		if (!v) return -1;
		b->v = v; b->cap = cap;
	}
	b->v[b->n++] = poc;
	return 0;
}

static int cmp_i32(const void *a, const void *b) {
	int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
	return (x > y) - (x < y);
}

/*
 * Turn the finished coded video sequence's decode-order POCs into display indices
 * for the seek points recorded inside it (those from `first_sp` on). A sequence is
 * output in increasing POC order, so a picture's display index is the rank of its
 * POC within the sequence, offset by `base` - the display index of the sequence's
 * first picture, which equals its decode index because no picture crosses an IDR
 * in either order.
 *
 * Returns -1 if the POCs are not unique. That is the guard against a stream whose
 * POC derivation h264poc.h does not model (MMCO5 resets the count mid-sequence,
 * which collides with an earlier POC); the display map would be wrong, so the
 * caller must fall back to IDR-only seek points instead of trusting it. Also -1 on
 * allocation failure, which takes the same safe path.
 */
static int flush_cvs(MvcSource *s, const PocBuf *pb, int base, int first_sp) {
	if (pb->n == 0)
		return 0;
	int32_t *sorted = malloc((size_t)pb->n * sizeof *sorted);
	if (!sorted)
		return -1;
	memcpy(sorted, pb->v, (size_t)pb->n * sizeof *sorted);
	qsort(sorted, (size_t)pb->n, sizeof *sorted, cmp_i32);
	int ok = 1;
	for (int i = 1; i < pb->n; i++)
		if (sorted[i] == sorted[i - 1]) { ok = 0; break; }
	for (int i = first_sp; ok && i < s->nidx; i++) {
		int local = s->idx[i].frame - base; /* the seek point's decode index within this sequence */
		int32_t poc = pb->v[local];
		int lo = 0, hi = pb->n; /* rank = lower_bound(sorted, poc) */
		while (lo < hi) {
			int mid = (lo + hi) / 2;
			if (sorted[mid] < poc) lo = mid + 1; else hi = mid;
		}
		s->idx[i].valid_from = base + lo;
	}
	free(sorted);
	return ok ? 0 : -1;
}

/*
 * Single linear scan over the Annex-B NAL units to derive, without pixel
 * decoding: the number of output frames (= base primary coded pictures), the
 * presence of a dependent view, and the seek points. A base primary picture is a
 * type 1/5 NAL whose first_mb_in_slice == 0; ue(v)==0 is a single '1' bit, so
 * that is exactly the top bit of the first RBSP byte (emulation prevention cannot
 * affect the first byte).
 *
 * A base picture is only counted once a base-view SPS (type 7) and a PPS (type 8)
 * have appeared earlier in the stream. This mirrors the decoder, which returns
 * EBADMSG (and produces no frame) for a slice whose SPS/PPS is not yet
 * initialized: a stream cut mid-GOP begins with VCL slices that reference the
 * (now discarded) parameter sets of the previous GOP, and counting those would
 * overcount num_frames - shifting every later display index and running past the
 * real frames at the end. Cleanly demuxed streams start with SPS/PPS/IDR, so the
 * guard is inert for them.
 *
 * With use_poc, the slice headers are also parsed for their picture order count
 * (h264poc.h), which is what lets a seek point sit on an open-GOP recovery point
 * and not just on an IDR - the difference between re-decoding ~20 frames and
 * ~600 on a 3D Blu-ray. Returns -1 if that derivation cannot model the stream, so
 * the caller can rerun without it; without use_poc only IDRs become seek points,
 * where decode order and display order agree and no POC is needed.
 */
static int scan_index_pass(MvcSource *s, int use_poc) {
	const uint8_t *p = s->start, *end = s->end;
	const uint8_t *au_start = s->start; /* start of the current access unit's leading NALs */
	int in_vcl = 0;                     /* did we just pass this AU's VCL NALs? */
	int frames = 0, is_mvc = 0;
	int seen_sps = 0, seen_pps = 0;     /* a base-view SPS+PPS must precede a counted slice */
	int recovery_seen = 0;              /* a recovery_point SEI is pending for the next picture */
	int cvs_base = 0, cvs_first_sp = 0; /* start of the coded video sequence being scanned */
	int rc = 0;
	PocBuf pb = {0};
	H264PocCtx *pc = NULL;
	if (use_poc) {
		pc = malloc(sizeof *pc); /* ~35 KB: too big for a frameserver worker's stack */
		if (!pc)
			return -1;
		h264_poc_ctx_init(pc);
	}
	while (p < end) {
		int type = p[0] & 0x1f;
		int is_vcl = (type >= 1 && type <= 5) || type == 19 || type == 20;
		const uint8_t *sc = edge264_find_start_code(p, end, 0);
		const uint8_t *nend = sc < end ? sc : end;
		if (!is_vcl && in_vcl) { au_start = p; in_vcl = 0; } /* leading NAL of the next AU */
		if (type == 20 || type == 15)
			is_mvc = 1;
		if (type == 7)
			seen_sps = 1;
		else if (type == 8)
			seen_pps = 1;
		if (type == 7 || type == 8 || type == 13 || type == 15) /* SPS/PPS/SPS-ext/subset-SPS */
			ps_push(s, p, nend);
		if (use_poc) {
			/* Only the base SPS (7) feeds the POC parser, never the MVC subset SPS
			 * (15): they share the id numbering but are distinct sets, so a subset
			 * SPS would overwrite the base view's (see h264poc.h). */
			if (type == 7)
				h264_parse_sps(pc, p, nend);
			else if (type == 8)
				h264_parse_pps(pc, p, nend);
			else if (type == 6 && h264_sei_is_recovery_point(p, nend))
				recovery_seen = 1;
		}
		if ((type == 1 || type == 5) && (p + 1 < end) && (p[1] & 0x80) && seen_sps && seen_pps) { /* base primary picture */
			/* If this AU begins directly with its VCL NAL (no leading non-VCL
			 * NAL advanced au_start), the first slice itself is the AU start.
			 * Without this, au_start stays frozen at the previous boundary and
			 * every later seek point records the wrong byte offset. */
			if (in_vcl)
				au_start = p;
			if (use_poc) {
				H264PicInfo pi;
				if (!h264_parse_picture(pc, p, nend, &pi)) { rc = -1; goto out; }
				if (pi.is_idr) { /* an IDR ends the previous sequence and starts a new one */
					if (flush_cvs(s, &pb, cvs_base, cvs_first_sp) < 0) { rc = -1; goto out; }
					pb.n = 0;
					cvs_base = frames;
					cvs_first_sp = s->nidx;
					h264_poc_reset(pc);
				}
				if (pocbuf_push(&pb, pi.poc) < 0) { rc = -1; goto out; }
				/* Every random-access point becomes a seek point: an IDR, or an
				 * I picture preceded by a recovery_point SEI (an exact open-GOP
				 * entry point, which is what a 3D Blu-ray puts between its far
				 * rarer IDRs). flush_cvs fills in the recovery point's own display
				 * index; until then valid_from == frame, which idx_push set. */
				if (pi.is_idr || (recovery_seen && pi.is_intra))
					idx_push(s, au_start, frames);
			} else if (type == 5) {
				idx_push(s, au_start, frames);
			}
			frames++;
			in_vcl = 1;
			recovery_seen = 0; /* consumed by this picture */
		} else if (is_vcl) {
			in_vcl = 1;
		}
		p = (sc < end) ? sc + 3 : end;
	}
	if (use_poc)
		rc = flush_cvs(s, &pb, cvs_base, cvs_first_sp); /* the last sequence ends at EOF */
out:
	free(pb.v);
	free(pc);
	if (rc == 0) {
		s->num_pics = frames;
		s->info.is_mvc = is_mvc;
	}
	return rc;
}

static void scan_index(MvcSource *s) {
	if (scan_index_pass(s, 1) == 0)
		return;
	/* The stream is outside the POC derivation's scope (field coding, an unknown
	 * parameter set, an MMCO5 POC reset - see h264poc.h), so an open-GOP recovery
	 * point's display index cannot be established and it must not become a seek
	 * point. Rerun recording IDRs only: decode order and display order agree there,
	 * so no POC is needed. Seeks then re-decode from the preceding IDR, which is the
	 * behaviour that shipped before recovery-point seeking - slower on a long GOP,
	 * never wrong. The partial first pass is discarded wholesale so nothing it
	 * derived can leak into the fallback. */
	s->nidx = 0;
	s->nps = 0;
	s->num_pics = 0;
	s->info.is_mvc = 0;
	scan_index_pass(s, 0);
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
 * emits pictures in non-decreasing display order within a run (edge264 makes
 * DisplayPoc a monotone display-order key), so a DisplayPoc that steps *backwards*
 * means the output stream has diverged from what scan_index counted (a
 * dropped/duplicated picture or decoder drift) and every later frame would be
 * mislabeled. Fail loudly on a strict decrease. A plateau (equal DisplayPoc) is
 * tolerated to match edge264's own conformance contract (conformance_check.c uses
 * '<', not '<='): a legitimate open-GOP POC collision on a real 3D Blu-ray - two
 * successive output pictures sharing a raw POC - must not abort an otherwise
 * correct decode. Returns 1 on success, 0 on a divergence (err set); last_poc is
 * INT64_MIN after a reset. */
static int check_display_order(MvcSource *s, const Edge264Frame *out, char *err, size_t errsize) {
	if (s->last_poc != INT64_MIN && out->DisplayPoc < s->last_poc) {
		set_err(err, errsize, "decoder output order diverged (display POC decreased)");
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
			/* The output queue is full and get_frame drains nothing - on a
			 * well-formed stream every ENOBUFS releases at least one frame, so
			 * this only accumulates on degenerate/crafted input whose pictures
			 * never complete. edge264 checks its per-view fullness gate before the
			 * buf>=end drain branch, so the flush sentinel (s->end,s->end) also
			 * returns ENOBUFS and cannot arm the drain via the public API (its own
			 * harness reaches into the private `flushing` flag, which edge264.h
			 * does not expose). Forcing the sentinel here would spin forever at
			 * 100% CPU, so fail loudly instead - a source filter must not hang on
			 * untrusted media. Matches the ENOMEM/EINVAL data-loss handling below. */
			if (++stuck > 64) {
				set_err(err, errsize, "decoder stalled: output queue full but no frame available (corrupt stream?)");
				return -1;
			}
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
	/* the frame cache holds independent copies, so it survives the reset */
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
	/* Largest seek point that can actually serve `target`: the search keys on
	 * valid_from, not frame, because a recovery point does not yield correct output
	 * for the display indices its leading pictures occupy (see SeekPoint). Keying on
	 * frame would pick a recovery point for a target inside that range and serve a
	 * leading picture decoded without its references. */
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (s->idx[mid].valid_from <= target) { best = mid; lo = mid + 1; }
		else hi = mid;
	}
	int sp_frame = best >= 0 ? s->idx[best].frame : 0;
	int sp_valid = best >= 0 ? s->idx[best].valid_from : 0;
	const uint8_t *sp_nal = best >= 0 ? s->idx[best].nal : s->start;
	/* Restart if we must go backwards, a closer seek point lies ahead, or the
	 * decoder is absent - a prior seek's reset_decoder may have freed it and then
	 * failed to reallocate (OOM), and without the NULL check an in-window forward
	 * request (target >= next_out, nearest seek point <= next_out) would take the
	 * no-restart path and decode against the NULL decoder, a persistent failure
	 * that never re-attempts the allocation. */
	if (s->dec == NULL || target < s->next_out || sp_frame > s->next_out) {
		if (!reset_decoder(s))
			return -1;
		refeed_param_sets(s, sp_nal);
		s->nal = sp_nal;
		s->next_out = sp_frame;
		s->valid_from = sp_valid;
	}
	return 0;
}

/* --- view assembly -------------------------------------------------------- */

static void copy_plane(uint8_t *dst, ptrdiff_t dstride, const uint8_t *src,
	ptrdiff_t sstride, int w, int h) {
	for (int y = 0; y < h; y++)
		memcpy(dst + (ptrdiff_t)y * dstride, src + (ptrdiff_t)y * sstride, (size_t)w);
}

/* --- decoded-frame cache (ring) ------------------------------------------- */

/* Bytes to cache one source picture: base Y/U/V plus, for MVC, the dependent
 * view's Y/U/V, all stored packed (stride == width). */
static size_t slot_bytes(const Edge264Frame *f) {
	size_t one = (size_t)f->width_Y * f->height_Y + 2 * (size_t)f->width_C * f->height_C;
	return f->samples_mvc[0] ? 2 * one : one;
}

/* Size the ring from the first decoded frame: as many slots as fit s->cache_budget,
 * clamped to [2, MAX_SLOTS]. Buffers are allocated lazily by ring_store, so a
 * large budget costs memory only for frames actually cached. Returns 0, or -1 on
 * allocation failure. */
static int ring_init(MvcSource *s, const Edge264Frame *f) {
	size_t per = slot_bytes(f);
	size_t budget = s->cache_budget ? s->cache_budget : ((size_t)DEFAULT_FRAME_CACHE_MB << 20);
	int cap = per ? (int)(budget / per) : FRAME_CACHE_MAX_SLOTS;
	if (cap < 2) cap = 2;
	if (cap > FRAME_CACHE_MAX_SLOTS) cap = FRAME_CACHE_MAX_SLOTS;
	s->slots = calloc((size_t)cap, sizeof *s->slots);
	if (!s->slots) return -1;
	for (int i = 0; i < cap; i++) s->slots[i].index = -1;
	s->ring_cap = cap;
	s->ring_head = 0;
	return 0;
}

static FrameSlot *ring_find(MvcSource *s, int index) {
	for (int i = 0; i < s->ring_cap; i++)
		if (s->slots[i].index == index) return &s->slots[i];
	return NULL;
}

/* Copy borrowed decoder frame `f` into the next ring slot (round-robin) and
 * build a view over the copy. Returns the slot, or NULL on OOM (the caller then
 * falls back to the borrowed frame, which is valid until the next decode). */
static FrameSlot *ring_store(MvcSource *s, int index, const Edge264Frame *f) {
	if (s->ring_cap == 0) return NULL;
	FrameSlot *sl = &s->slots[s->ring_head];
	s->ring_head = (s->ring_head + 1) % s->ring_cap;
	size_t need = slot_bytes(f);
	if (sl->bufcap < need) {
		uint8_t *nb = realloc(sl->buf, need);
		if (!nb) { sl->index = -1; return NULL; }
		sl->buf = nb;
		sl->bufcap = need;
	}
	int w = f->width_Y, h = f->height_Y, cw = f->width_C, ch = f->height_C;
	size_t ysz = (size_t)w * h, csz = (size_t)cw * ch;
	uint8_t *d = sl->buf;
	copy_plane(d, w, f->samples[0], f->stride_Y, w, h);
	copy_plane(d + ysz, cw, f->samples[1], f->stride_C, cw, ch);
	copy_plane(d + ysz + csz, cw, f->samples[2], f->stride_C, cw, ch);
	sl->frame = *f; /* metadata; pointers/strides overridden to the packed copy */
	sl->frame.stride_Y = w;
	sl->frame.stride_C = cw;
	sl->frame.samples[0] = d;
	sl->frame.samples[1] = d + ysz;
	sl->frame.samples[2] = d + ysz + csz;
	if (f->samples_mvc[0]) {
		uint8_t *m = d + ysz + 2 * csz;
		copy_plane(m, w, f->samples_mvc[0], f->stride_Y, w, h);
		copy_plane(m + ysz, cw, f->samples_mvc[1], f->stride_C, cw, ch);
		copy_plane(m + ysz + csz, cw, f->samples_mvc[2], f->stride_C, cw, ch);
		sl->frame.samples_mvc[0] = m;
		sl->frame.samples_mvc[1] = m + ysz;
		sl->frame.samples_mvc[2] = m + ysz + csz;
	} else {
		sl->frame.samples_mvc[0] = sl->frame.samples_mvc[1] = sl->frame.samples_mvc[2] = NULL;
	}
	sl->index = index;
	return sl;
}

static void ring_free(MvcSource *s) {
	if (!s->slots) return;
	for (int i = 0; i < s->ring_cap; i++) free(s->slots[i].buf);
	free(s->slots);
	s->slots = NULL;
	s->ring_cap = 0;
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
	int64_t fps_num, int64_t fps_den, int cachesize_mb, char *err, size_t errsize) {
	if (layout < MVC_BASE || layout > MVC_ALT) { /* else assemble_plane's switch fills nothing */
		set_err(err, errsize, "invalid layout");
		return NULL;
	}
	MvcSource *s = calloc(1, sizeof *s);
	if (!s) { set_err(err, errsize, "out of memory"); return NULL; }
	s->n_threads = n_threads;
	/* clamp the cache budget; <= 0 selects the default (see cache_budget_bytes) */
	s->cache_budget = cache_budget_bytes(cachesize_mb);
	s->swaplr = swaplr != 0;

	int64_t src_mtime = 0;
	s->map = map_file_ro(path, &s->map_size, &src_mtime);
	if (!s->map) {
		set_err(err, errsize, "cannot open input file");
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

	/* Skip the full-file scan when a matching sidecar index exists; otherwise
	 * scan and (best-effort) write one for next time. See load_index_cache. */
	char *cpath = malloc(strlen(path) + sizeof ".mvcidx");
	int have_cache = 0;
	if (cpath) {
		snprintf(cpath, strlen(path) + sizeof ".mvcidx", "%s.mvcidx", path);
		have_cache = load_index_cache(s, cpath, s->map_size, src_mtime);
	}
	if (!have_cache)
		scan_index(s);
	if (s->num_pics <= 0) {
		free(cpath);
		set_err(err, errsize, "no decodable frames found");
		mvc_close(s);
		return NULL;
	}
	if (!have_cache && cpath)
		save_index_cache(s, cpath, s->map_size, src_mtime);
	free(cpath);
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
	s->valid_from = 0; /* a decode from the stream's start has no leading pictures to discard */
	s->last_poc = INT64_MIN;
	Edge264Frame f;
	int rc = decode_next_output(s, &f, err, errsize);
	if (rc <= 0) {
		if (rc == 0) set_err(err, errsize, "failed to decode the first frame"); /* rc<0: err already set */
		mvc_close(s);
		return NULL;
	}
	s->next_out = 1;
	/* size the frame cache from the first picture and cache frame 0, so the first
	 * get_frame(0) is a cache hit rather than a backward seek + re-decode */
	if (ring_init(s, &f) < 0) {
		set_err(err, errsize, "out of memory");
		mvc_close(s);
		return NULL;
	}
	ring_store(s, 0, &f);
	s->info.base_width = f.width_Y;
	s->info.base_height = f.height_Y;
	switch (s->info.layout) {
	case MVC_TAB: s->info.width = f.width_Y; s->info.height = f.height_Y * 2; break;
	case MVC_SBS: s->info.width = f.width_Y * 2; s->info.height = f.height_Y; break;
	default:      s->info.width = f.width_Y; s->info.height = f.height_Y; break;
	}
	return s;
}

/* Ensure source picture `src_n` is available in s->cur: from the frame cache if
 * present, else by seeking to the nearest random-access point and decoding
 * forward. Frames decoded on the way to the target (within the cache window just
 * before it) are cached too, so a subsequent backward / Reverse pass over the
 * same span hits RAM instead of re-decoding. This is also what lets MVC_ALT serve
 * both views of a picture from one decode (the second view is a cache hit).
 * Returns 0 on success, -1 on error (message in err). */
static int ensure_source_frame(MvcSource *s, int src_n, char *err, size_t errsize) {
	FrameSlot *hit = ring_find(s, src_n);
	if (hit) { s->cur = hit->frame; return 0; }
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
		/* Discard the leading pictures of the recovery point this run started at:
		 * they come out first (they precede it in display order) but reference the
		 * previous GOP, which a cold decode never saw, so they are wrong. They must
		 * be decoded - the decoder emits them and the ones after depend on the DPB
		 * state they leave - but never cached or served. seek_to guarantees the
		 * target is at or after valid_from, so this never skips the frame we want. */
		if (idx < s->valid_from)
			continue;
		/* cache frames in the window just before the target - exactly what a
		 * backward / Reverse pass asks for next; the target itself is always in
		 * the window (ring_cap >= 2). On OOM caching the target, fall back to the
		 * borrowed frame (valid until the next decode, which mvc_get_frame
		 * precedes with its assemble). */
		if (idx > src_n - s->ring_cap) {
			FrameSlot *sl = ring_store(s, idx, &f);
			if (idx == src_n) { s->cur = sl ? sl->frame : f; return 0; }
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
	ring_free(s);
	free(s->idx);
	free(s->ps);
	unmap_file(s->map, s->map_size);
	free(s);
}
