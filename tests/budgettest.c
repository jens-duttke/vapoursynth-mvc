/*
 * budgettest - regression test for the decoded-frame cache byte-budget sizing.
 *
 * mvc_open converts the cachesize (MiB) to a byte budget. The clamp permits up to
 * MAX_FRAME_CACHE_MB (16384) MiB, but a naive `(size_t)mb << 20` overflows a
 * 32-bit size_t once mb >= 4096 (4096 << 20 == 2^32 == 0 mod 2^32): exact
 * multiples of 4096 wrap to 0 (silently falling back to the default) and other
 * large values shrink, so a Reverse() / backward pass re-seeks far more than the
 * user asked for. cache_budget_bytes computes in 64-bit and caps at SIZE_MAX, so
 * the budget is always the intended size (or the largest a size_t can hold),
 * never a wrapped small value.
 *
 * The shared header cache_budget.h is compiled standalone here (no edge264), so
 * this also runs as a 32-bit build (-m32) where the wrap actually bites.
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "cache_budget.h"
#include <stdint.h>
#include <stdio.h>

int main(void) {
	printf("sizeof(size_t)=%zu\n", sizeof(size_t));
	int fail = 0;

	/* 16 MiB never wraps (16 << 20 == 2^24), so it pins the exact byte count and
	 * catches a computation that is off even at the low end. */
	size_t b16 = cache_budget_bytes(MIN_FRAME_CACHE_MB);
	if (b16 != (size_t)16 << 20) { printf("FAIL: 16 MiB -> %zu bytes (want %zu)\n", b16, (size_t)16 << 20); fail = 1; }

	/* Across the permitted range the budget must never be zero (the wrap symptom)
	 * and must be non-decreasing in the requested size. 4096/8192/16384 MiB are the
	 * exact-multiple-of-4096 values that wrap to 0 on a 32-bit size_t. */
	const int mb[] = { 16, 512, 2048, 4096, 8192, 16384 };
	size_t prev = 0;
	for (unsigned i = 0; i < sizeof mb / sizeof *mb; i++) {
		size_t b = cache_budget_bytes(mb[i]);
		printf("  %6d MiB -> %zu bytes\n", mb[i], b);
		if (b == 0) { printf("FAIL: %d MiB budget wrapped to 0\n", mb[i]); fail = 1; }
		if (b < prev) { printf("FAIL: %d MiB budget %zu < previous %zu (non-monotone: wrap)\n", mb[i], b, prev); fail = 1; }
		prev = b;
	}

	/* An out-of-range request clamps, it does not wrap: below MIN -> MIN, and the
	 * default (<= 0) is a sane nonzero budget. */
	if (cache_budget_bytes(1) != (size_t)MIN_FRAME_CACHE_MB << 20) { printf("FAIL: 1 MiB not clamped up to MIN\n"); fail = 1; }
	if (cache_budget_bytes(0) == 0) { printf("FAIL: default budget is zero\n"); fail = 1; }

	printf(fail ? "RESULT: FAIL\n" : "RESULT: PASS (cache budget never wraps, clamps correctly)\n");
	return fail;
}
