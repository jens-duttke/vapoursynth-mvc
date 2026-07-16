/*
 * cache_budget - decoded-frame cache sizing, split out so the decode core and its
 * regression test share one definition (the test compiles this header standalone,
 * including as a 32-bit build, without pulling in edge264).
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#ifndef CACHE_BUDGET_H
#define CACHE_BUDGET_H

#include <stddef.h>
#include <stdint.h>

/* Decoded-frame cache budget in MiB: default when the caller passes <= 0, and the
 * permitted range. The default matches the order of magnitude other frameserver
 * sources cache (BestSource defaults ~1 GB); the budget is a ceiling, not an
 * up-front reservation, as slot buffers are allocated lazily. */
#define DEFAULT_FRAME_CACHE_MB 512
#define MIN_FRAME_CACHE_MB 16
#define MAX_FRAME_CACHE_MB 16384

/* Clamp cachesize_mb to [MIN, MAX] (<= 0 selects the default) and convert to a
 * byte budget. Computed in 64-bit and capped at SIZE_MAX so a 32-bit size_t build
 * stays correct: MAX is 16384 MiB, and a plain `(size_t)mb << 20` overflows a
 * 32-bit size_t once mb >= 4096 (4096 << 20 == 2^32 == 0 mod 2^32), silently
 * zeroing or shrinking the cache. The SIZE_MAX cap is compiled only where size_t
 * is narrower than 64-bit (on an LP64 build the comparison would be a no-op the
 * compiler flags as always-false). */
static inline size_t cache_budget_bytes(int cachesize_mb) {
	int mb = cachesize_mb > 0 ? cachesize_mb : DEFAULT_FRAME_CACHE_MB;
	if (mb < MIN_FRAME_CACHE_MB) mb = MIN_FRAME_CACHE_MB;
	if (mb > MAX_FRAME_CACHE_MB) mb = MAX_FRAME_CACHE_MB;
	uint64_t bytes = (uint64_t)mb << 20;
#if SIZE_MAX < UINT64_MAX
	if (bytes > SIZE_MAX) return SIZE_MAX;
#endif
	return (size_t)bytes;
}

#endif
