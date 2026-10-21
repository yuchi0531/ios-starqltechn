#pragma once
#include <stdint.h>

/* Shared with host_test.c so the test builds payloads with the same layout the
 * loader parses (both files assert the offsets). */

#define BOOT_LINE_LENGTH	608
#define PAGE_SIZE		4096UL

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

