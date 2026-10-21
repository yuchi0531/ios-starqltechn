/*
 * Run the starqltechn loader on the host.
 *
 * There is no aarch64 cross compiler on a plain desktop and no emulator for
 * S-Boot, so this is the only way shim/starqltechn/loader.c gets executed
 * before it runs on a phone. loader.c has no dependency on a running kernel -
 * it only reads and writes physical addresses and does cache maintenance - and
 * its cache/barrier ops compile away off aarch64. So: mmap the real board
 * addresses (framebuffer, ramoops, low RAM, the XNU RAM window), build a
 * synthetic SWEETXNU payload in the initrd slot and a real flattened device
 * tree with /chosen/linux,initrd-*, and call dl_main() like start.S does.
 *
 *   make -C shim/starqltechn host
 *
 * The constants under test are the loader's own defaults: nothing here
 * redefines FB_BASE, XNU_RAM_BASE, RAMOOPS_* or the test pattern.
 *
 * What this cannot prove: that S-Boot enters at the image start, that the panel
 * scans out of FB_BASE, that 0xa2000000+0x18000000 is free on this phone, or
 * that fs/pstore parses the header we write. Those need the hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "../fdt.h"
#include "dsi.h"
#include "loader.h"

/* Addresses the loader uses. Kept here only so the test can map them; the
 * loader's copies are authoritative and proven equal by the checks below. */
#define T_LOW_BASE	0x80000000UL
#define T_LOW_SIZE	0x14000000UL	/* covers the initrd at 0x90000000 */
#define T_FB_BASE	0x9d400000UL
#define T_FB_SIZE	0x2400000UL
#define T_RAMOOPS	0xa1300000UL
#define T_RAMOOPS_SIZE	0x100000UL
#define T_XNU_BASE	0xa2000000UL
#define T_XNU_SIZE	0x18000000UL

#define T_RD		(T_LOW_BASE + 0x10000000UL)	/* 0x90000000 */
#define T_RD_SIZE	0x200000UL
#define T_XNU_WIN	0x200000UL			/* payload mem_size: small, so
							 * zeroing it in the test is cheap */

/* DSI0: the test gives the driver a register file instead of the real
 * addresses (dsi.c's MMIO ops compile to array accesses on the host, the same
 * trick the loader uses for the framebuffer). The values are the vendor DT's,
 * so the test also proves the driver is pointed at the right blocks. */
#define T_DSI_CTRL	0xae94000UL
#define T_DSI_PHY	0xae94400UL
#define T_DSI_BUF	((uintptr_t)starq_dsi_cmd_buf)

#define FB_PIX		((volatile uint32_t *)T_FB_BASE)
#define FB_LINE		(1440)

static int failures;
#define check(cond, ...) do { \
	if (cond) { printf("ok:   "); } else { failures++; printf("FAIL: "); } \
	printf(__VA_ARGS__); printf("\n"); \
} while (0)

static void *map_pa(unsigned long pa, unsigned long size)
{
	void *p = mmap((void *)pa, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		fprintf(stderr, "cannot map %#lx+%#lx at its board address\n", pa, size);
		exit(2);
	}
	return p;
}

/* ---- flattened device tree: root + /chosen/{bootargs,linux,initrd-*} ----
 *
 * The bootargs property is what S-Boot's own command line arrives in, and the
 * panel probe reads it, so the test has to be able to put an arbitrary string
 * there - including a very long one, to prove the bounds checks hold. */
#define S_INITRD_START	"linux,initrd-start"
#define S_INITRD_END	"linux,initrd-end"
#define S_BOOTARGS	"bootargs"
/* Offsets into the strings block, computed rather than hand-counted. */
#define OFF_INITRD_END	(sizeof(S_INITRD_START))
#define OFF_BOOTARGS	(sizeof(S_INITRD_START) + sizeof(S_INITRD_END))

static size_t build_fdt_args(unsigned char *buf, uint64_t rd_start,
			     uint64_t rd_end, const char *bootargs)
{
	static const char strings[] = S_INITRD_START "\0" S_INITRD_END "\0"
				      S_BOOTARGS "\0";
	const size_t strings_len = sizeof(strings) - 1;
	size_t p = 40;
	uint32_t *h = (uint32_t *)buf;
	size_t blen = bootargs ? strlen(bootargs) + 1 : 0;

#define TOK(v) do { uint32_t t_ = __builtin_bswap32(v); memcpy(buf + p, &t_, 4); p += 4; } while (0)
#define NAME(s) do { size_t n_ = strlen(s) + 1; memcpy(buf + p, s, n_); \
		     p += (n_ + 3) & ~(size_t)3; } while (0)
#define V64(v) do { unsigned char d_[8]; \
	for (int i_ = 0; i_ < 8; i_++) d_[i_] = (unsigned char)((v) >> (56 - 8 * i_)); \
	TOK(3); TOK(8); TOK(nameoff_); memcpy(buf + p, d_, 8); p += 8; } while (0)
#define STR(s, n) do { size_t l_ = (n); \
	memcpy(buf + p, s, l_); p += (l_ + 3) & ~(size_t)3; } while (0)

	size_t nameoff_ = 0;
	(void)nameoff_;

	TOK(1); NAME("");
	TOK(1); NAME("chosen");
	V64(rd_start);
	nameoff_ = OFF_INITRD_END;
	V64(rd_end);
	if (blen) {
		nameoff_ = OFF_BOOTARGS;
		TOK(3); TOK((uint32_t)blen); TOK(nameoff_);
		STR(bootargs, blen);
	}
	TOK(2);
	TOK(2);
	TOK(9);
	size_t off_strings = p;
	memcpy(buf + p, strings, strings_len);
	p += strings_len;

	h[0] = __builtin_bswap32(0xd00dfeed);
	h[1] = __builtin_bswap32((uint32_t)p);
	h[2] = __builtin_bswap32(40);
	h[3] = __builtin_bswap32((uint32_t)off_strings);
	h[4] = 0;
	h[5] = __builtin_bswap32(17);
	h[6] = __builtin_bswap32(16);
	h[7] = 0;
	h[8] = __builtin_bswap32((uint32_t)strings_len);
	h[9] = __builtin_bswap32((uint32_t)(off_strings - 40));
#undef TOK
#undef NAME
#undef V64
#undef STR
	return p;
}

static size_t build_fdt(unsigned char *buf, uint64_t rd_start, uint64_t rd_end)
{
	return build_fdt_args(buf, rd_start, rd_end, NULL);
}

/* Big enough for a long bootloader command line plus the tree's own strings:
 * the FDT bound checks are worth exercising with something that does not fit. */
static unsigned char fdt[4096];

/* ---- synthetic SWEETXNU v1 payload, as tools/mkpayload.py writes one ---- */
#define VIRT_BASE	0xfffffff007004000ULL	/* __TEXT vmaddr, as a real kernel */
#define SEG0_SIZE	0x100000ULL
#define SEG1_OFF	0x100000ULL
#define SEG1_SIZE	0x80000ULL
#define ENTRY_OFF	0x4000ULL
#define ADT_SIZE	0x1000UL
#define PAYLOAD_LEN	(0x1000 + SEG0_SIZE + SEG1_SIZE + ADT_SIZE + 0x1000)

static uint64_t kernel_end_va(void)
{
	return (VIRT_BASE + SEG1_OFF + SEG1_SIZE + 0x3fff) & ~0x3fffULL;
}

static void build_payload(uint64_t phys_base, uint64_t mem_size, const char *cmdline)
{
	struct payload *p = (struct payload *)T_RD;
	memset(p, 0, PAYLOAD_LEN);
	memcpy(p->magic, "SWEETXNU", 8);
	p->version = 1;
	p->nsegs = 2;
	p->virt_base = VIRT_BASE;
	p->entry_va = VIRT_BASE + ENTRY_OFF;
	p->phys_base = phys_base;
	p->mem_size = mem_size;
	p->kernel_end = kernel_end_va();
	snprintf(p->cmdline, BOOT_LINE_LENGTH, "%s", cmdline);

	uint64_t off = 0x1000;
	p->segs[0].vmaddr = VIRT_BASE;
	p->segs[0].vmsize = SEG0_SIZE;
	p->segs[0].file_off = off;
	p->segs[0].file_size = SEG0_SIZE;
	memset((void *)(T_RD + off), 0x5a, SEG0_SIZE);
	off += SEG0_SIZE;
	p->segs[1].vmaddr = VIRT_BASE + SEG1_OFF;
	p->segs[1].vmsize = SEG1_SIZE;
	p->segs[1].file_off = off;
	p->segs[1].file_size = SEG1_SIZE;
	memset((void *)(T_RD + off), 0xb4, SEG1_SIZE);
	off += SEG1_SIZE;
	p->adt_off = off;
	p->adt_size = ADT_SIZE;
	memset((void *)(T_RD + off), 0xa7, ADT_SIZE);
}

/* One boot attempt: payload + FDT, then dl_main like start.S calls it. The
 * loader image (load/len) is the initrd slot: it must be outside the window.
 * `bootargs` is what S-Boot leaves in /chosen/bootargs; NULL means "no such
 * property" (the old behaviour, and the UNKNOWN-panel path). */
static uint64_t boot_args(const char *cmdline, const char *bootargs,
			  uint64_t phys_base, uint64_t mem_size,
			  uint64_t rd_start, uint64_t rd_end)
{
	build_payload(phys_base, mem_size, cmdline);
	build_fdt_args(fdt, rd_start, rd_end, bootargs);
	memset((void *)T_FB_BASE, 0x11, 0x10000);
	memset((void *)T_RAMOOPS, 0x00, 0x10000);
	return dl_main((uintptr_t)fdt, 1, 0x30d00800UL,
		       (uintptr_t)T_RD, 0x40000UL);
}

static uint64_t boot(const char *cmdline, uint64_t phys_base, uint64_t mem_size,
		     uint64_t rd_start, uint64_t rd_end)
{
	return boot_args(cmdline, NULL, phys_base, mem_size, rd_start, rd_end);
}

static uint64_t boot_ok(const char *cmdline)
{
	return boot(cmdline, T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
}

/* The same, with a bootloader command line in the DTB. */
static uint64_t boot_ok_args(const char *cmdline, const char *bootargs)
{
	return boot_args(cmdline, bootargs, T_XNU_BASE, T_XNU_WIN, T_RD,
			 T_RD + PAYLOAD_LEN);
}

/* The bootloader line, as it looks on a real S9. */
#define BA_PX01 "console=ttyMSM0 androidboot.bootloader=G9600ZHS9FQL1 " \
		"msm_drm.dsi_display0=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0 " \
		"androidboot.hardware=qcom"
#define BA_MQ01 "console=ttyMSM0 androidboot.bootloader=G9600ZHS9FQL1 " \
		"msm_drm.dsi_display0=ss_dsi_panel_S6E3HA8_AMB577MQ01_WQHD:config0 " \
		"androidboot.hardware=qcom"
#define BA_NONE "console=ttyMSM0 androidboot.bootloader=G9600ZHS9FQL1"

/*
 * DSI_CTRL as a bootloaders leaves it. The vendor DTS says both S9 panels are
 * dsi_cmd_mode, so D_LINK_CMD is the likely real state and D_LINK_VIDEO the one
 * the earlier code assumed. Each case is exercised.
 */
#define D_LINK_VIDEO	0x000001b3u	/* ena|vid|clk|lane0-3 */
#define D_LINK_CMD	0x000001b5u	/* ena|cmd|clk|lane0-3 */
#define D_LINK_OFF	0x000001b4u	/* clock on, controller disabled */

/* Reset the DSI register file to a plausible live link and re-init the
 * driver, so the next boot starts from a known state. */
static void dsi_link(uint32_t ctrl)
{
	memset(starq_dsi_regs_ctrl, 0, sizeof(starq_dsi_regs_ctrl));
	memset(starq_dsi_regs_phy, 0, sizeof(starq_dsi_regs_phy));
	starq_dsi_regs_ctrl[0x004 / 4] = ctrl;
	starq_dsi_regs_ctrl[0x008 / 4] = 0;		/* idle */
	starq_dsi_regs_ctrl[0x028 / 4] = 0x0c5c0080u;	/* active_v start/end */
	starq_dsi_regs_phy[0x0ec / 4] = 1;		/* refgen ready */
	starq_dsi_trace_n = 0;
}

/* How many register writes hit offset `off` with value `val`. */
static unsigned dsi_count(uint32_t off, uint32_t val)
{
	unsigned n = 0;

	for (unsigned i = 0; i < starq_dsi_trace_n; i++)
		if (starq_dsi_trace[i].off == off && starq_dsi_trace[i].val == val)
			n++;
	return n;
}

/* The loader log of the last boot, from the console ramoops zone. */
static const char *boot_log(uint32_t *lenp)
{
	uint32_t len = *(volatile uint32_t *)(T_RAMOOPS + 0x40008UL);

	if (lenp)
		*lenp = len;
	return (const char *)(T_RAMOOPS + 0x4000cUL);
}

/* Where the first occurrence is, so a test can assert ordering. */
static const char *log_at(const char *needle)
{
	uint32_t len;
	const char *t = boot_log(&len);

	return memmem(t, len, needle, strlen(needle));
}

static int log_has(const char *needle)
{
	return log_at(needle) != NULL;
}

int main(void)
{
	map_pa(T_LOW_BASE, T_LOW_SIZE);
	map_pa(T_FB_BASE, T_FB_SIZE);
	map_pa(T_RAMOOPS, T_RAMOOPS_SIZE);
	map_pa(T_XNU_BASE, T_XNU_SIZE);

	/* The test's idea of the board must match the loader's. */
	printf("== constants\n");
	check(1, "framebuffer %#lx, ramoops %#lx, window %#lx+%#lx",
	      (unsigned long)T_FB_BASE, (unsigned long)T_RAMOOPS,
	      (unsigned long)T_XNU_BASE, (unsigned long)T_XNU_SIZE);

	printf("== a good payload boots\n");
	uint64_t entry = boot_ok("debug=0xc starqfb=1");
	check(entry == T_XNU_BASE + ENTRY_OFF, "entry %#lx == window + entry offset",
	      (unsigned long)entry);
	check(*(volatile uint64_t *)(T_XNU_BASE + (VIRT_BASE - VIRT_BASE)) ==
	      0x5a5a5a5a5a5a5a5aULL, "segment 0 copied to the window base");
	check(*(volatile uint64_t *)(T_XNU_BASE + SEG1_OFF) == 0xb4b4b4b4b4b4b4b4ULL,
	      "segment 1 copied");
	/* the ADT goes at kernel_end (page aligned past the last segment) */
	check(*(volatile uint8_t *)(T_XNU_BASE + (kernel_end_va() - VIRT_BASE)) == 0xa7 &&
	      *(volatile uint8_t *)(T_XNU_BASE + (kernel_end_va() - VIRT_BASE) + ADT_SIZE - 1) == 0xa7,
	      "ADT copied at kernel_end (%#llx)", (unsigned long long)kernel_end_va());

	struct boot_args *ba = (struct boot_args *)(uintptr_t)starq_boot_args;
	check(ba->Revision == 2 && ba->Version == 2, "boot_args rev/version 2/2");
	check(ba->physBase == T_XNU_BASE && ba->memSize == T_XNU_WIN,
	      "boot_args RAM window %#llx+%#llx",
	      (unsigned long long)ba->physBase, (unsigned long long)ba->memSize);
	check(ba->deviceTreeP == VIRT_BASE + kernel_end_va() - VIRT_BASE,
	      "deviceTreeP is the VA, not the PA (%#llx)", (unsigned long long)ba->deviceTreeP);
	check(ba->topOfKernelData == starq_boot_args + 0x1000, "topOfKernelData");
	check(ba->Video.v_baseAddr == T_FB_BASE && ba->Video.v_width == 1440 &&
	      ba->Video.v_height == 2960 && ba->Video.v_rowBytes == 5760,
	      "boot_args framebuffer geometry");
	check(strcmp(ba->CommandLine, "debug=0xc starqfb=1") == 0, "cmdline copied");

	printf("== the XNU window is zeroed before the copy\n");
	{
		/* dirty the whole window, then boot: everything must be gone except
		 * what the loader wrote */
		memset((void *)T_XNU_BASE, 0xee, T_XNU_WIN);
		uint64_t e = boot_ok("starqfb=0");
		check(e != 0, "booted");
		check(*(volatile uint32_t *)(T_XNU_BASE + 0x1f0000) == 0,
		      "tail of the window is zero (0x%08x)",
		      *(volatile uint32_t *)(T_XNU_BASE + 0x1f0000));
	}

	printf("== payload validation\n");
	{
		uint64_t e = boot("", T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		*(volatile uint64_t *)T_RD = 0x1122334455667788ULL;
		build_fdt(fdt, T_RD, T_RD + PAYLOAD_LEN);
		e = dl_main((uintptr_t)fdt, 1, 0, (uintptr_t)T_RD, 0x40000);
		check(e == 0, "bad magic rejected");
	}
	{
		uint64_t e = boot("", 0x80000000UL, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		check(e == 0, "phys_base 0x80000000 rejected (outside the window)");
	}
	{
		/* one page past the window end */
		uint64_t e = boot("", T_XNU_BASE, T_XNU_SIZE + 0x1000, T_RD, T_RD + PAYLOAD_LEN);
		check(e == 0, "mem_size past the window end rejected");
	}
	{
		boot("", T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		struct payload *p = (struct payload *)T_RD;
		p->segs[1].vmsize = T_XNU_WIN;		/* segment larger than RAM */
		check(dl_main((uintptr_t)fdt, 1, 0, 0, 0) == 0, "oversized segment rejected");
	}
	{
		boot("", T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		struct payload *p = (struct payload *)T_RD;
		p->segs[0].file_off = PAYLOAD_LEN - 8;	/* file data past the ramdisk */
		p->segs[0].file_size = 0x1000;
		check(dl_main((uintptr_t)fdt, 1, 0, 0, 0) == 0, "segment file_off out of range rejected");
	}
	{
		boot("", T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		struct payload *p = (struct payload *)T_RD;
		p->entry_va = VIRT_BASE + T_XNU_WIN;	/* past kernel_end */
		check(dl_main((uintptr_t)fdt, 1, 0, 0, 0) == 0, "entry outside the kernel rejected");
	}
	{
		/* the initrd inside the window: it would be zeroed before use */
		uint64_t e = boot("", T_XNU_BASE, T_XNU_WIN, T_XNU_BASE, T_XNU_BASE + PAYLOAD_LEN);
		check(e == 0, "payload inside the XNU window rejected");
	}
	{
		/* loader image inside the window (start.S passes the runtime addr) */
		boot("", T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		uint64_t e = dl_main((uintptr_t)fdt, 1, 0, T_XNU_BASE + 0x1000, 0x40000);
		check(e == 0, "loader image inside the XNU window rejected");
	}
	{
		/* the DTB inside the window */
		boot("", T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		uint64_t e = dl_main(T_XNU_BASE + 0x1000, 1, 0, (uintptr_t)T_RD, 0x40000);
		check(e == 0, "DTB inside the XNU window rejected");
	}
	{
		/* EL0 must be refused */
		boot("", T_XNU_BASE, T_XNU_WIN, T_RD, T_RD + PAYLOAD_LEN);
		check(dl_main((uintptr_t)fdt, 0, 0, 0, 0) == 0, "EL0 entry rejected");
	}
	{
		/* an FDT without the initrd properties */
		unsigned char empty[512];
		build_fdt(empty, T_RD, T_RD + PAYLOAD_LEN);
		/* overwrite the first property name offset so nothing matches */
		empty[40 + 4 + 4 + 4] = 0xff;
		build_payload(T_XNU_BASE, T_XNU_WIN, "");
		check(dl_main((uintptr_t)empty, 1, 0, 0, 0) == 0, "missing /chosen initrd rejected");
	}

	printf("== framebuffer test pattern\n");
	memset((void *)T_FB_BASE, 0x11, T_FB_SIZE);
	entry = boot_ok("starqfb=1");
	check(entry != 0, "booted with the pattern");
	check(FB_PIX[0] == 0xffffffffU, "line 0 white (%#x)", FB_PIX[0]);
	check(FB_PIX[64 * FB_LINE] == 0xffff0000U, "line 64 red (%#x)", FB_PIX[64 * FB_LINE]);
	check(FB_PIX[128 * FB_LINE] == 0xff00ff00U, "line 128 green (%#x)", FB_PIX[128 * FB_LINE]);
	check(FB_PIX[192 * FB_LINE] == 0xff0000ffU, "line 192 blue (%#x)", FB_PIX[192 * FB_LINE]);
	check(FB_PIX[(2960 * 2 / 3 + 10) * FB_LINE + 720] == 0xffff00ffU,
	      "magenta block at 2/3 height (%#x)", FB_PIX[(2960 * 2 / 3 + 10) * FB_LINE + 720]);
	memset((void *)T_FB_BASE, 0x11, T_FB_SIZE);
	boot_ok("starqfb=0");
	check(FB_PIX[64 * FB_LINE] == 0xff000000U && FB_PIX[(2960 * 2 / 3 + 10) * FB_LINE + 720] == 0xff000000U,
	      "starqfb=0 clears the bands and the block");

	printf("== boot-arg overrides\n");
	check(boot_ok("starqwin=0x81000000:0x1000") == 0,
	      "starqwin outside the payload rejected");
	check(boot_ok("starqwin=0xa2000000:0x200000") != 0,
	      "starqwin matching the payload accepted");

	printf("== a lookalike boot-arg must not match\n");
	{
		memset((void *)T_FB_BASE, 0, T_FB_SIZE);
		boot_ok("nostarqfb=0 starqfb=1");
		check(FB_PIX[0] == 0xffffffffU, "nostarqfb=0 ignored, starqfb=1 honoured");
	}

	printf("== ramoops\n");
	{
		/* successful boot: both zones carry "DBGC" and the same text */
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		boot_ok("starqfb=0");
		uint32_t sig = *(volatile uint32_t *)(0xa1340000UL);
		uint32_t sig2 = *(volatile uint32_t *)(0xa1380000UL);
		check(sig == 0x43474244U, "console zone header DBGC (%#x)", sig);
		check(sig2 == 0x43474244U, "pmsg zone header DBGC (%#x)", sig2);
		uint32_t len = *(volatile uint32_t *)(0xa1340008UL);
		uint32_t len2 = *(volatile uint32_t *)(0xa1380008UL);
		check(len > 32 && len == len2, "both zones hold %u bytes", len);
		check(memcmp((void *)0xa134000cUL, (void *)0xa138000cUL, len) == 0,
		      "both zones hold the same text");
		const char *text = (const char *)0xa134000cUL;
		check(memmem(text, len, "S9LOADER BEGIN", 14) != NULL, "header line logged");
		check(memmem(text, len, "JUMP TO XNU", 11) != NULL, "jump line logged");
		check(memmem(text, len, "phys=0xa2000000", 15) != NULL, "payload RAM logged");
		check(text[0] == 'S' && !memcmp(text, "S9LOADER BEGIN", 14),
		      "text starts at zone+12 with the header line");
		/* the zone must not run past its 0x40000 bytes */
		check(len <= 0x40000 - 12, "log fits the zone (%u)", len);
		/* the ftrace zone at 0xa13c0000 must be untouched */
		check(*(volatile uint32_t *)(0xa13c0000UL) == 0, "ftrace zone untouched");
	}

	printf("== DSI: panel probe from the bootloader cmdline\n");
	{
		char tok[96];

		/* fdt_chosen_str is the one new reader of bootloader input: it must
		 * reject a bad magic, a truncated totalsize and an out-of-tree
		 * offset instead of walking off into unmapped memory. */
		unsigned char bad[512];
		char full[256];

		build_fdt_args(bad, 0x90000000UL, 0x90100000UL, BA_PX01);
		check(fdt_chosen_str((uintptr_t)bad, "bootargs", full, sizeof(full)) &&
		      strcmp(full, BA_PX01) == 0, "fdt_chosen_str reads bootargs");
		check(fdt_chosen_str((uintptr_t)bad, "nosuchprop", tok, sizeof(tok)) == 0,
		      "a missing property is refused");
		bad[0] ^= 0xff;
		check(fdt_chosen_str((uintptr_t)bad, "bootargs", tok, sizeof(tok)) == 0,
		      "a wrong FDT magic is refused");
		build_fdt_args(bad, 0x90000000UL, 0x90100000UL, BA_PX01);
		bad[7] = 0x10;		/* totalsize far beyond the buffer */
		check(fdt_chosen_str((uintptr_t)bad, "bootargs", tok, sizeof(tok)) == 0,
		      "an out-of-range totalsize is refused");
		build_fdt_args(bad, 0x90000000UL, 0x90100000UL, BA_PX01);
		bad[11] = 0xf0;		/* off_dt_struct past the end */
		check(fdt_chosen_str((uintptr_t)bad, "bootargs", tok, sizeof(tok)) == 0,
		      "an out-of-range off_dt_struct is refused");
		build_fdt_args(bad, 0x90000000UL, 0x90100000UL, BA_PX01);
		check(fdt_chosen_str((uintptr_t)bad, "bootargs", tok, 4) == 1 &&
		      strcmp(tok, "con") == 0,
		      "a short buffer truncates rather than overrunning");

		check(starq_dsi_panel_from_cmdline(BA_PX01, tok, sizeof(tok)) ==
		      STARQ_PANEL_PX01 &&
		      strcmp(tok, "ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0") == 0,
		      "PX01 bootargs -> PX01, token \"%s\"", tok);
		check(starq_dsi_panel_from_cmdline(BA_MQ01, tok, sizeof(tok)) ==
		      STARQ_PANEL_MQ01 &&
		      strcmp(tok, "ss_dsi_panel_S6E3HA8_AMB577MQ01_WQHD:config0") == 0,
		      "MQ01 bootargs -> MQ01, token \"%s\"", tok);
		check(starq_dsi_panel_from_cmdline(BA_NONE, tok, sizeof(tok)) ==
		      STARQ_PANEL_UNKNOWN && tok[0] == 0,
		      "neither name -> UNKNOWN, empty token");
		check(starq_dsi_panel_from_cmdline(NULL, tok, sizeof(tok)) ==
		      STARQ_PANEL_UNKNOWN, "NULL bootargs -> UNKNOWN");
		check(starq_dsi_panel_from_cmdline("", tok, sizeof(tok)) ==
		      STARQ_PANEL_UNKNOWN, "empty bootargs -> UNKNOWN");
		/* Only a whole token counts: the vendor property is the token's
		 * start, so a lookalike must not match. */
		check(starq_dsi_panel_from_cmdline(
			      "nosomethingmsm_drm.dsi_display0=AMB577MQ01:config0", tok,
			      sizeof(tok)) == STARQ_PANEL_UNKNOWN,
		      "a mid-token lookalike is not the property");
		/* The token is truncated to the buffer, not overrun. */
		check(starq_dsi_panel_from_cmdline(BA_PX01, tok, 8) ==
		      STARQ_PANEL_UNKNOWN && strlen(tok) == 7,
		      "a short buffer truncates and still terminates");
		check(starq_elvss_off("off") && !starq_elvss_off("offx") &&
		      !starq_elvss_off("") && !starq_elvss_off(NULL) &&
		      !starq_elvss_off("b5,8f"),
		      "only \"off\" disables the override");
	}

	printf("== DSI DCS over the command path\n");
	{
		struct starq_dcs_seq s;

		/* --- parsing: one command --- */
		check(starq_dcs_parse("b5,8f,dc,0a", &s) == 0 && s.n == 1 &&
		      s.cmd[0].len == 4 && s.cmd[0].data[0] == 0xb5 &&
		      s.cmd[0].data[1] == 0x8f && s.cmd[0].data[2] == 0xdc &&
		      s.cmd[0].data[3] == 0x0a,
		      "b5,8f,dc,0a -> one 4-byte command");

		/* --- parsing: ';' separated, level-1 key unlock first --- */
		check(starq_dcs_parse("f0,5a,5a;b5,8f,dc,0a", &s) == 0 && s.n == 2 &&
		      s.cmd[0].len == 3 && s.cmd[0].data[0] == 0xf0 &&
		      s.cmd[0].data[2] == 0x5a && s.cmd[1].len == 4 &&
		      s.cmd[1].data[0] == 0xb5 && s.cmd[1].data[3] == 0x0a,
		      "f0,5a,5a;b5,8f,dc,0a -> two commands");
		check(starq_dcs_parse("b5,8f", &s) == 0 && s.n == 1 && s.cmd[0].len == 2,
		      "two bytes parse (short write with parameter)");
		check(starq_dcs_parse("29", &s) == 0 && s.n == 1 && s.cmd[0].len == 1,
		      "one byte parses (short write)");
		check(starq_dcs_parse("B5,8F,DC,0A", &s) == 0 && s.cmd[0].data[0] == 0xb5,
		      "uppercase hex accepted");
		check(starq_dcs_parse("0,b5", &s) == 0 && s.cmd[0].data[0] == 0 &&
		      s.cmd[0].data[1] == 0xb5, "0 is a byte, not an empty field");

		/* --- parsing: malformed input is rejected whole --- */
		check(starq_dcs_parse("", &s) != 0 && s.n == 0, "empty string rejected");
		check(starq_dcs_parse("b5,8f,dc,0a;", &s) != 0 && s.n == 0,
		      "trailing ';' rejected");
		check(starq_dcs_parse("b5,,dc", &s) != 0 && s.n == 0,
		      "empty byte rejected");
		check(starq_dcs_parse("b5,8f,dc,0a,", &s) != 0 && s.n == 0,
		      "trailing ',' rejected");
		check(starq_dcs_parse("b5,zz", &s) != 0 && s.n == 0,
		      "non-hex digit rejected");
		check(starq_dcs_parse("b5 8f", &s) != 0 && s.n == 0,
		      "space rejected (the boot-arg would split it anyway)");
		check(starq_dcs_parse("b5,8fdc,0a", &s) != 0 && s.n == 0,
		      "3 hex digits in a field rejected");
		check(starq_dcs_parse(";", &s) != 0 && s.n == 0, "bare ';' rejected");
		check(starq_dcs_parse("b5,8f;", &s) != 0 && s.n == 0,
		      "malformed second command clears a valid first one");

		/* --- the default policy per panel ---
		 * Both S9 panels are dsi_cmd_mode in the vendor DTS, but the
		 * register decides; the default sequence is
		 *   f0 5a 5a ; b5 8f dc <elvss> ; f0 a5 a5
		 * with <elvss> = 0x16 on PX01 and 0x0a on MQ01. */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0", BA_PX01) != 0, "booted");
		check(dsi_count(0x090, 1) == 3, "PX01 default sends 3 commands (%u)",
		      dsi_count(0x090, 1));
		check(log_has("dcs f0 5a 5a") && log_has("dcs b5 8f dc 16") &&
		      log_has("dcs f0 a5 a5"),
		      "PX01 default sends f0 5a 5a ; b5 8f dc 16 ; f0 a5 a5");
		/* ...and in that order, not just present: the key has to be on
		 * before 0xB5 and off after it. */
		{
			const char *k1 = log_at("dcs f0 5a 5a");
			const char *b5 = log_at("dcs b5 8f dc 16");
			const char *k2 = log_at("dcs f0 a5 a5");

			check(k1 && b5 && k2 && k1 < b5 && b5 < k2,
			      "level-1 key on, 0xB5, level-1 key off, in that order");
		}
		check(log_has("panel=PX01 name=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0"),
		      "the panel name is logged");
		/* the last packet left in the buffer is the key-off */
		check(starq_dsi_cmd_buf[4] == 0xf0 && starq_dsi_cmd_buf[5] == 0xa5,
		      "last packet is the level-1 key off");

		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0", BA_MQ01) != 0, "booted");
		check(dsi_count(0x090, 1) == 3, "MQ01 default sends 3 commands (%u)",
		      dsi_count(0x090, 1));
		check(log_has("dcs b5 8f dc 0a") && !log_has("dcs b5 8f dc 16"),
		      "MQ01 default sends b5 8f dc 0a");
		check(log_has("panel=MQ01 name=ss_dsi_panel_S6E3HA8_AMB577MQ01_WQHD:config0"),
		      "the MQ01 panel name is logged");

		/* UNKNOWN: no panel token at all. The policy sends the PX01 value
		 * and says why. */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0", BA_NONE) != 0, "booted");
		check(dsi_count(0x090, 1) == 3 && log_has("dcs b5 8f dc 16"),
		      "UNKNOWN panel sends the PX01 value (0x16)");
		check(log_has("panel=UNKNOWN") &&
		      log_has("assuming PX01") &&
		      log_has("errs towards less green"),
		      "the UNKNOWN choice and its reasoning are logged");
		/* and with no bootargs property at all */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok("starqfb=0") != 0, "booted");
		check(dsi_count(0x090, 1) == 3, "no /chosen/bootargs still uses the default");

		/* --- starqelvss=off: nothing sent, but the probe still runs --- */
		dsi_link(D_LINK_CMD);	/* the register says command mode */
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqelvss=off", BA_PX01) != 0, "booted");
		check(dsi_count(0x090, 1) == 0, "starqelvss=off sends nothing (%u)",
		      dsi_count(0x090, 1));
		check(log_has("starqelvss=off, nothing sent"), "and says so");
		check(log_has("DSI_CTRL=0x000001b5 en=1 video=0 cmd=1"),
		      "the mode probe line prints all eight hex digits");
		check(log_has("ack_err=0x00000000 timeout=0x00000000"),
		      "the Ack_Err and Timeout status words are logged");
		check(log_has("panel in COMMAND mode"), "command mode recognised");
		check(log_has("ack_err=") && log_has("timeout="),
		      "DSI_ACK_ERR_STATUS and DSI_TIMEOUT_STATUS are logged");

		/* --- a boot-arg still overrides everything --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqelvss=b5,19,dc,0a", BA_PX01) != 0,
		      "booted");
		check(dsi_count(0x090, 1) == 1 && log_has("dcs b5 19 dc 0a"),
		      "starqelvss= replaces the default (1 command, as given)");
		check(log_has("explicit sequence"), "and is logged as explicit");

		/* --- starqmps= overrides byte 2 of the default --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqfb=0 starqmps=cc", BA_PX01) != 0, "booted");
		check(log_has("dcs b5 8f cc 16"), "starqmps=cc -> b5 8f cc 16");
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqfb=0 starqmps=0xcc", BA_MQ01) != 0, "booted");
		check(log_has("dcs b5 8f cc 0a"), "starqmps=0xcc -> b5 8f cc 0a");
		/* an invalid value does not change the default */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqfb=0 starqmps=zz", BA_PX01) != 0, "booted");
		check(log_has("dcs b5 8f dc 16") && log_has("bad starqmps"),
		      "a bad starqmps is logged and 0xdc kept");

		/* --- DSI_CTRL.ENABLE clear: the link is down, nothing is sent --- */
		dsi_link(D_LINK_OFF);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0", BA_PX01) != 0, "boot still succeeds");
		check(dsi_count(0x090, 1) == 0, "link down sends nothing (%u)",
		      dsi_count(0x090, 1));
		check(log_has("LINK DOWN (DSI_CTRL.ENABLE=0)"), "link down is logged");
		check(log_has("link down, sequence dropped"),
		      "the dropped sequence is logged");
		/* the same through the single-command path */
		check(starq_dsi_dcs_write((const uint8_t *)"b5\x8f\xdc\x16", 4) != 0 &&
		      log_has("link down, command dropped"),
		      "starq_dsi_dcs_write refuses too");

		/* --- the packet, the DMA offsets and the schedule --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqelvss=b5,8f,dc,16", BA_PX01) != 0,
		      "booted");
		{
			unsigned mem_off = 0, trig = 0, sched = 0;
			for (unsigned i = 0; i < starq_dsi_trace_n; i++) {
				uint32_t o = starq_dsi_trace[i].off;
				if (o == 0x048) mem_off = starq_dsi_trace[i].val;
				if (o == 0x090) trig = starq_dsi_trace[i].val;
				if (o == 0x100) sched = starq_dsi_trace[i].val;
			}
			check(mem_off == (uint32_t)(uintptr_t)T_DSI_BUF,
			      "DMA offset is the packet buffer (%#x)", mem_off);
			check(trig == 1, "software trigger issued");
			check(sched == 0, "schedule cleared again afterwards");
			/* the packet: WC=4, DT=0x39, last|long, then the payload */
			check(starq_dsi_cmd_buf[0] == 4 && starq_dsi_cmd_buf[1] == 0 &&
			      starq_dsi_cmd_buf[2] == 0x39 &&
			      starq_dsi_cmd_buf[3] == (0x80 | 0x40) &&
			      starq_dsi_cmd_buf[4] == 0xb5 && starq_dsi_cmd_buf[5] == 0x8f &&
			      starq_dsi_cmd_buf[6] == 0xdc && starq_dsi_cmd_buf[7] == 0x16,
			      "long-write packet: 4 0 39 c0 b5 8f dc 16");
			check(log_has("STARQ DSI: command sent"), "completion logged");
			check(log_has("sched line=0xc5c"),
			      "DMA scheduled on the ACTIVE_V blanking line");
			check(log_has("video=1 cmd=0"),
			      "video mode reported from DSI_CTRL");
			check(log_has("panel in VIDEO mode"),
			      "and explained in words");
		}

		/* --- the level-1 key is a 3-byte short write with parameter --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqfb=0", BA_PX01) != 0, "booted");
		check(starq_dsi_cmd_buf[0] == 3 && starq_dsi_cmd_buf[1] == 0 &&
		      starq_dsi_cmd_buf[2] == 0x39 &&
		      starq_dsi_cmd_buf[3] == (0x80 | 0x40),
		      "the key-off is a long write of 3 bytes (WC=3)");

		/* --- one byte becomes a short write --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqelvss=29", BA_PX01) != 0, "booted");
		check(starq_dsi_cmd_buf[0] == 0x29 && starq_dsi_cmd_buf[1] == 0 &&
		      starq_dsi_cmd_buf[2] == 0x05 && starq_dsi_cmd_buf[3] == 0x80,
		      "1-byte DCS write -> short write packet 29 00 05 80");

		/* --- two commands, both sent, in order --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqelvss=f0,5a,5a;b5,8f,dc,16", BA_PX01) != 0,
		      "booted");
		check(dsi_count(0x090, 1) == 2, "two triggers for two commands (%u)",
		      dsi_count(0x090, 1));
		check(starq_dsi_cmd_buf[4] == 0xb5, "second packet is b5 ...");

		/* --- malformed: rejected, and nothing reaches the wire --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqelvss=b5,zz", BA_PX01) != 0, "boot still succeeds");
		check(dsi_count(0x090, 1) == 0, "malformed list sends nothing (%u)",
		      dsi_count(0x090, 1));
		check(log_has("nothing sent"),
		      "malformed list is logged, not silently dropped");

		/* --- the memory path times out: FIFO fallback carries it --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_sim_stall = 1;	/* memory path never completes */
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqelvss=b5,8f,dc,16", BA_PX01) != 0, "booted");
		starq_dsi_sim_stall = 0;
		check(dsi_count(0x15c, (1u << 1) | (1u << 2) | (3u << 16)) > 0,
		      "FIFO path used after the memory timeout");
		check(log_has("DMA done timeout"), "timeout logged, not a hang");
		check(log_has("FIFO fallback"), "fallback logged");
		check(log_has("STARQ DSI: command sent"), "command still delivered");

		/* --- both paths stall: an error, never a hang --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_sim_stall = 3;
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqelvss=b5,8f,dc,16", BA_PX01) != 0, "booted");
		starq_dsi_sim_stall = 0;
		check(log_has("command FAILED"), "both paths timing out is an error");
		check(log_has("override not applied"),
		      "loader reports the override did not take");
		/* and XNU is still started: the panel is not worth a hang */
		check(log_has("JUMP TO XNU"), "boot continues after a failed DCS write");

		/* --- nothing outside the command path is written --- */
		{
			/* Video timing, PHY and clock registers, as a fresh boot
			 * leaves them; a transfer must leave every one alone. */
			static const uint32_t ro_ctrl[] = { 0x000, 0x004, 0x008, 0x00c,
				0x010, 0x014, 0x018, 0x01c, 0x024, 0x028, 0x02c,
				0x030, 0x034, 0x038, 0x084, 0x0a8, 0x0ac, 0x0b0,
				0x0c0, 0x11c, 0x120 };
			static const uint32_t ro_phy[] = { 0x010, 0x014, 0x018, 0x01c,
				0x020, 0x024, 0x030, 0x038, 0x0ac, 0x0b0, 0x0ec,
				0x0f4, 0x0f8 };
			uint32_t snap_ctrl[sizeof(ro_ctrl) / 4], snap_phy[sizeof(ro_phy) / 4];
			unsigned before = 0, bad = 0;

			dsi_link(D_LINK_VIDEO);
			for (unsigned i = 0; i < sizeof(ro_ctrl) / 4; i++)
				snap_ctrl[i] = starq_dsi_regs_ctrl[ro_ctrl[i] / 4];
			for (unsigned i = 0; i < sizeof(ro_phy) / 4; i++)
				snap_phy[i] = starq_dsi_regs_phy[ro_phy[i] / 4];

			before = starq_dsi_trace_n;
			check(starq_dsi_init(T_DSI_CTRL, T_DSI_PHY) == 0 &&
			      starq_dsi_elvss("", STARQ_PANEL_PX01,
					      STARQ_ELVSS_MPS) == 3,
			      "default sequence sent against the live-link register file");
			for (unsigned i = 0; i < sizeof(ro_ctrl) / 4; i++)
				if (starq_dsi_regs_ctrl[ro_ctrl[i] / 4] != snap_ctrl[i]) {
					bad++;
					printf("      ctrl %#x: %#x -> %#x\n", ro_ctrl[i],
					       snap_ctrl[i], starq_dsi_regs_ctrl[ro_ctrl[i] / 4]);
				}
			for (unsigned i = 0; i < sizeof(ro_phy) / 4; i++)
				if (starq_dsi_regs_phy[ro_phy[i] / 4] != snap_phy[i]) {
					bad++;
					printf("      phy %#x: %#x -> %#x\n", ro_phy[i],
					       snap_phy[i], starq_dsi_regs_phy[ro_phy[i] / 4]);
				}
			check(bad == 0, "no video-timing, PHY, clock or DSI_CTRL write");
			check(starq_dsi_trace_n > before, "the transfer did write something");
		}

		/* --- starqdsisw=1: no schedule, plain software trigger --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqdsisw=1 starqelvss=b5,8f,dc,16", BA_PX01) != 0,
		      "booted");
		check(dsi_count(0x100, 0) == 0, "starqdsisw=1 leaves DMA_SCHED alone");

		/* --- a command-mode link never programs the schedule --- */
		dsi_link(D_LINK_CMD);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqfb=0", BA_PX01) != 0, "booted");
		check(dsi_count(0x100, 0) == 0 && dsi_count(0x090, 1) == 3,
		      "command mode: 3 commands, no DMA schedule");

		/* --- a lookalike boot-arg must not match --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("nostarqelvss=b5,8f,dc,0a", BA_MQ01) != 0, "booted");
		check(log_has("dcs b5 8f dc 0a") && !log_has("dcs b5 8f dc 0a pkt"),
		      "nostarqelvss= is ignored, the MQ01 default runs");
		check(dsi_count(0x090, 1) == 3, "and the default policy still applies");
	}

	printf(failures ? "\n%d FAILURE(S)\n" : "\nall checks passed\n", failures);
	return failures ? 1 : 0;
}
