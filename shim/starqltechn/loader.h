#pragma once
#include <stdint.h>

/* Shared with host_test.c so the test builds payloads with the same layout the
 * loader parses (both files assert the offsets). */

#define BOOT_LINE_LENGTH	608

/* The kernel this loader starts is built with __ARM_16K_PG__ (16 KiB pages), and
 * arm_vm_physmap_slide() (osfmk/arm64/arm_vm_init.c) asserts that
 * virtBase + (topOfKernelData - physBase) is a multiple of ARM_PGMASK (0x3fff).
 * virtBase and physBase both come from tools/mkpayload.py already 16K-clean, so
 * that reduces to topOfKernelData - i.e. boot_args must be placed on a 16 KiB
 * boundary, not a 4 KiB one. Same constant as shim/loader.c (sweet). */
#define PAGE_16K		0x4000UL

/* First 16 KiB boundary at or after the ADT ends: where boot_args goes.
 * Inline and shared with host_test.c so the loader and the test cannot drift:
 * the test sweeps every ADT size through this exact expression. */
static inline uintptr_t starq_boot_args_pa(uintptr_t adt_pa, uint64_t adt_size)
{
	return (adt_pa + (uintptr_t)adt_size + PAGE_16K - 1) &
	       ~(uintptr_t)(PAGE_16K - 1);
}

/* What XNU ends up seeing as args->topOfKernelData: the end of the boot_args
 * block, one 16 KiB page past its start. This is the value that must satisfy
 * (virtBase + (topOfKernelData - physBase)) & ARM_PGMASK == 0. */
static inline uintptr_t starq_top_of_kernel_data(uintptr_t adt_pa, uint64_t adt_size)
{
	return starq_boot_args_pa(adt_pa, adt_size) + PAGE_16K;
}

/* tools/mkpayload.py v1 container. */
struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};
struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};
struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

/* entry point, called from start.S; returns the XNU entry PA, or 0 on failure */
uint64_t dl_main(uintptr_t dtb, uint64_t el, uint64_t sctlr,
		 uintptr_t load, uint64_t len);
/* boot_args PA for start.S to pass in x0 */
extern uint64_t starq_boot_args;

/*
 * Console + ramoops logging for the helper files (shim/starqltechn/dsi.c).
 * Same stream as the loader's out()/hex()/kv(): every line ends up both on the
 * panel and in the two ramoops zones, in the existing "STARQ ..." style.
 */
void starq_out(const char *s);
void starq_hex(uint64_t v);
void starq_kv(const char *k, uint64_t v);

