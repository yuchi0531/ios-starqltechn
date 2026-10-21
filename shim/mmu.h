#pragma once
#include <stdint.h>
#include "console.h"	/* FB_BASE: the ADT framebuffer the shim draws into */

/* The framebuffer's 32MB block is mapped Normal non-cacheable (write-combining)
 * rather than Device, so stores can merge. Defaults to the console's FB_BASE;
 * build.sh only needs to set FB_BASE per board. */
#ifndef FB_PHYS
#define FB_PHYS	FB_BASE
#endif

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

int mmu_enable(struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);
