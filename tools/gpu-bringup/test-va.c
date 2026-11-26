/*
 * Host-side check of kernel/hakios.mali_va.h: no kernel headers, no GPU.
 *   gcc -std=gnu11 -Wall -Wextra -Ikernel test-va.c -o /tmp/test-va && /tmp/test-va
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hakios.mali_va.h"

#define GRANULES (MALI_VA_END / MALI_VA_GRANULE)

static unsigned char seen[GRANULES];

static void
occupy(uint64_t address, uint64_t size)
{
	uint64_t g = address / MALI_VA_GRANULE;
	uint64_t count = (size + MALI_VA_GRANULE - 1) / MALI_VA_GRANULE + 1;

	assert(address >= MALI_VA_FIRST && address % MALI_VA_GRANULE == 0);
	assert(g + count <= GRANULES);
	for (uint64_t i = g; i < g + count; i++) {
		assert(!seen[i]);
		seen[i] = 1;
	}
}

int
main(void)
{
	struct mali_va va;
	uint64_t a[64], sizes[64];

	memset(&va, 0, sizeof(va));
	memset(seen, 0, sizeof(seen));

	for (unsigned i = 0; i < 64; i++) {
		sizes[i] = MALI_VA_GRANULE * (1 + (i % 7));
		a[i] = mali_va_alloc(&va, sizes[i], i % 3 == 0);
		assert(a[i] != 0);
		occupy(a[i], sizes[i]);
	}
	assert(!mali_va_free(&va, MALI_VA_FIRST - MALI_VA_GRANULE, MALI_VA_GRANULE));
	assert(!mali_va_free(&va, a[0] + 1, MALI_VA_GRANULE));
	for (unsigned i = 0; i < 64; i += 2) {
		assert(mali_va_free(&va, a[i], sizes[i]));
		memset(seen + a[i] / MALI_VA_GRANULE, 0,
		    ((sizes[i] + MALI_VA_GRANULE - 1) / MALI_VA_GRANULE + 1));
	}
	for (unsigned i = 1; i < 64; i += 2) {
		uint64_t again = mali_va_alloc(&va, sizes[i], i % 3 == 0);

		assert(again != 0);
	}
	/* Executable buffers must not straddle a 16 MiB line. */
	memset(&va, 0, sizeof(va));
	for (unsigned i = 0; i < 32; i++) {
		uint64_t address = mali_va_alloc(&va, 3u << 20, true);

		assert(address != 0);
		assert(address / MALI_VA_EXEC_LINE == (address + (3u << 20) - 1) / MALI_VA_EXEC_LINE);
	}
	printf("test-va: allocator checks passed\n");
	return 0;
}
