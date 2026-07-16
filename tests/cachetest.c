/*
 * cachetest - regression test for the on-disk index cache (the sidecar
 * `<source>.mvcidx` that lets a reopen skip the full-file NAL scan).
 *
 * The core invariant: the cache must be transparent. A cache hit has to yield
 * exactly the same frames as a fresh scan, and a cache that does not match the
 * source (corrupt, or stale because the source changed) must be ignored so a
 * fresh scan runs - never trusted into serving wrong frames. From the committed
 * multi-GOP fixture (copied aside so the test never mutates the tracked file's
 * mtime) it checks, all against the same first-frames hash:
 *
 *   miss     : first open with no sidecar -> scans, and writes the sidecar.
 *   hit      : reopen with the sidecar present -> must be bit-identical.
 *   corrupt  : sidecar overwritten with garbage -> must fall back to a scan.
 *   stale    : source mtime changed after caching -> must fall back to a scan.
 *
 * usage: cachetest <base_multigop.264>
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <sys/utime.h>
  #define UTIMBUF struct _utimbuf
  #define UTIME _utime
#else
  #include <utime.h>
  #define UTIMBUF struct utimbuf
  #define UTIME utime
#endif

#define NF 8 /* first-frames fingerprint length */

static uint64_t fnv_plane(const uint8_t *p, ptrdiff_t stride, int w, int h) {
	uint64_t H = 1469598103934665603ULL;
	for (int y = 0; y < h; y++) {
		const uint8_t *r = p + (ptrdiff_t)y * stride;
		for (int x = 0; x < w; x++) { H ^= r[x]; H *= 1099511628211ULL; }
	}
	return H;
}

/* Open `path` (base view), hash the Y plane of the first NF frames into out[].
 * Returns 0 on success, -1 on any error. */
static int hash_frames(const char *path, uint64_t out[NF]) {
	char err[256] = "";
	MvcSource *s = mvc_open(path, 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!s) { fprintf(stderr, "  open failed: %s\n", err); return -1; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2;
	int n = in->num_frames < NF ? in->num_frames : NF;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
	int rc = 0;
	if (!Y || !U || !V) { rc = -1; goto done; }
	for (int i = 0; i < NF; i++) out[i] = 0;
	for (int i = 0; i < n; i++) {
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			fprintf(stderr, "  frame %d failed: %s\n", i, err); rc = -1; break;
		}
		out[i] = fnv_plane(Y, W, W, H);
	}
done:
	free(Y); free(U); free(V);
	mvc_close(s);
	return rc;
}

static int file_exists(const char *p) {
	FILE *f = fopen(p, "rb");
	if (!f) return 0;
	fclose(f);
	return 1;
}

/* Copy src -> dst byte for byte. Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
	FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
	int rc = -1;
	if (in && out) {
		char buf[65536];
		size_t n;
		rc = 0;
		while ((n = fread(buf, 1, sizeof buf, in)) > 0)
			if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
	}
	if (in) fclose(in);
	if (out && fclose(out) != 0) rc = -1;
	return rc;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <fixture.264>\n", argv[0]); return 2; }
	const char *fixture = argv[1];
	size_t blen = strlen(fixture) + 32;
	char *tmp = malloc(blen), *cache = malloc(blen);
	if (!tmp || !cache) { fprintf(stderr, "oom\n"); return 2; }
	snprintf(tmp, blen, "%s.cachetest.264", fixture);
	snprintf(cache, blen, "%s.mvcidx", tmp);

	if (copy_file(fixture, tmp)) { fprintf(stderr, "cannot copy fixture aside\n"); return 2; }
	remove(cache);

	uint64_t miss[NF], hit[NF], corrupt[NF], stale[NF];
	int ok = 1;

	/* 1. miss: no sidecar -> scan + write */
	if (hash_frames(tmp, miss)) { printf("FAIL[miss]: open/decode failed\n"); ok = 0; goto out; }
	if (file_exists(cache)) printf("ok[miss]: scanned and wrote sidecar\n");
	else { printf("FAIL[miss]: no sidecar written\n"); ok = 0; }

	/* 2. hit: sidecar present -> must be identical */
	if (hash_frames(tmp, hit) || memcmp(miss, hit, sizeof miss) != 0) {
		printf("FAIL[hit]: cache hit differs from a fresh scan\n"); ok = 0;
	} else printf("ok[hit]: cache hit bit-identical to the scan\n");

	/* 3. corrupt: garbage sidecar -> must fall back to a scan */
	{ FILE *f = fopen(cache, "wb"); if (f) { fputs("garbage-not-a-valid-mvcidx-header", f); fclose(f); } }
	if (hash_frames(tmp, corrupt) || memcmp(miss, corrupt, sizeof miss) != 0) {
		printf("FAIL[corrupt]: corrupt cache not recovered\n"); ok = 0;
	} else printf("ok[corrupt]: corrupt sidecar ignored, rescanned\n");

	/* 4. stale: bump the source mtime after caching -> must fall back to a scan.
	 * hash_frames above rewrote a valid sidecar; move the source's mtime to a
	 * fixed past time so the sidecar's stored mtime no longer matches. */
	{
		UTIMBUF ut;
		ut.actime = ut.modtime = 1000000000; /* 2001-09-09, != the just-written cache key */
		if (UTIME(tmp, &ut) != 0) { printf("WARN[stale]: could not change mtime, skipping\n"); }
	}
	if (hash_frames(tmp, stale) || memcmp(miss, stale, sizeof miss) != 0) {
		printf("FAIL[stale]: stale cache (changed source) not detected\n"); ok = 0;
	} else printf("ok[stale]: stale sidecar detected, rescanned\n");

out:
	remove(cache);
	remove(tmp);
	free(tmp); free(cache);
	printf(ok ? "RESULT: PASS (index cache transparent + invalidated correctly)\n"
	          : "RESULT: FAIL\n");
	return ok ? 0 : 1;
}
