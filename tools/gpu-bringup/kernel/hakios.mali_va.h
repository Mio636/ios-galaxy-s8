/*
 * GPU addresses for the Galaxy S8's Mali-G71. Plain C with no kernel in it so
 * that it can be checked on a build machine.
 *
 * Adapted from /home/mio/s9-ref/core/drivers/gpu/hakios.mali_va.h. The range
 * is Linux panfrost's: 32 MB up to 4 GB. Mesa 25.0.7 assumes the lower 32 MB
 * are reserved and the space is 32-bit; on Bifrost a blend shader's address
 * is 32 bits and executable buffers must not cross a 16 MB line. Granules of
 * 64 KB are handed out and taken back, with one empty granule after each
 * buffer so a stale job faults in a hole rather than in its neighbour.
 */
#ifndef HAKIOS_MALI_VA_H
#define HAKIOS_MALI_VA_H

#include <stdbool.h>
#include <stdint.h>

#define MALI_VA_FIRST           0x02000000ull
#define MALI_VA_END             0x100000000ull
#define MALI_VA_GRANULE         0x10000ull
#define MALI_VA_GRANULES        (MALI_VA_END / MALI_VA_GRANULE)
#define MALI_VA_EXEC_LINE       0x1000000ull

struct mali_va {
	uint64_t used[MALI_VA_GRANULES / 64];
	uint64_t next;
	uint64_t taken;
};

static inline bool
mali_va_used(const struct mali_va *va, uint64_t g)
{
	return (va->used[g / 64] >> (g % 64)) & 1;
}

static inline void
mali_va_mark(struct mali_va *va, uint64_t first, uint64_t count, bool used)
{
	for (uint64_t g = first; g < first + count; g++) {
		if (used) {
			va->used[g / 64] |= 1ull << (g % 64);
		} else {
			va->used[g / 64] &= ~(1ull << (g % 64));
		}
	}
	va->taken = used ? va->taken + count : va->taken - count;
}

static inline uint64_t
mali_va_granules(uint64_t size)
{
	return (size + MALI_VA_GRANULE - 1) / MALI_VA_GRANULE + 1;
}

static inline uint64_t
mali_va_find(const struct mali_va *va, uint64_t from, uint64_t count, uint64_t size, bool executable)
{
	uint64_t g = from;

	while (g + count <= MALI_VA_GRANULES) {
		uint64_t start = g * MALI_VA_GRANULE, run = 0;

		if (executable && size <= MALI_VA_EXEC_LINE &&
		    start / MALI_VA_EXEC_LINE != (start + size - 1) / MALI_VA_EXEC_LINE) {
			g = (start / MALI_VA_EXEC_LINE + 1) * (MALI_VA_EXEC_LINE / MALI_VA_GRANULE);
			continue;
		}
		while (run < count && !mali_va_used(va, g + run)) {
			run++;
		}
		if (run == count) {
			return g;
		}
		g += run + 1;
	}
	return 0;
}

static inline uint64_t
mali_va_alloc(struct mali_va *va, uint64_t size, bool executable)
{
	uint64_t count = mali_va_granules(size), first = MALI_VA_FIRST / MALI_VA_GRANULE, g = 0;

	if (size == 0 || size > MALI_VA_END - MALI_VA_FIRST) {
		return 0;
	}
	if (va->next > first) {
		g = mali_va_find(va, va->next, count, size, executable);
	}
	if (g == 0) {
		g = mali_va_find(va, first, count, size, executable);
	}
	if (g == 0) {
		return 0;
	}
	mali_va_mark(va, g, count, true);
	va->next = g + count < MALI_VA_GRANULES ? g + count : first;
	return g * MALI_VA_GRANULE;
}

static inline bool
mali_va_free(struct mali_va *va, uint64_t address, uint64_t size)
{
	uint64_t g = address / MALI_VA_GRANULE, count = mali_va_granules(size);

	if (address < MALI_VA_FIRST || address % MALI_VA_GRANULE != 0 || size == 0 ||
	    g + count > MALI_VA_GRANULES) {
		return false;
	}
	for (uint64_t k = g; k < g + count; k++) {
		if (!mali_va_used(va, k)) {
			return false;
		}
	}
	mali_va_mark(va, g, count, false);
	return true;
}

#endif /* HAKIOS_MALI_VA_H */
