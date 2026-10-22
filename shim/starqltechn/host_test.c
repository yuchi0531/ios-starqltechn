/*
 * Run the starqltechn loader on the host.
 *
 * There is no aarch64 cross compiler on a plain desktop and no emulator for
 * anything at EL1, so this is the only way shim/starqltechn/loader.c gets
 * executed before it runs on a phone. loader.c has no dependency on a running
 * kernel - it only reads and writes physical addresses and does cache
 * maintenance - and its cache/barrier ops compile away off aarch64. So: mmap
 * the real board addresses (framebuffer, ramoops, the low-RAM initrd slot, the
 * ramdisk ABL uses and the XNU RAM window), build a synthetic SWEETXNU payload
 * in the initrd slot and a real flattened device tree with
 * /chosen/linux,initrd-*, and call dl_main() like start.S does.
 *
 *   make -C shim/starqltechn host
 *
 * The constants under test are the loader's own defaults: nothing here
 * redefines FB_BASE, XNU_RAM_BASE, RAMOOPS_* or the test pattern.
 *
 * What this cannot prove: that ABL enters at the image start, that the panel
 * scans out of FB_BASE, that 0xd4000000+0x2b000000 is free of every carve-out on
 * this phone, or that fs/pstore parses the header we write. Those need the
 * hardware. What it does prove is the arithmetic: the window and the addresses
 * ABL really uses (framebuffer, ramoops, the ramdisk at 0xa2000000) do not
 * overlap.
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
#define T_XNU_BASE	0xd4000000UL
#define T_XNU_SIZE	0x2b000000UL

#define T_RD		(T_LOW_BASE + 0x10000000UL)	/* 0x90000000 */
#define T_RD_SIZE	0x200000UL
#define T_XNU_WIN	0x200000UL			/* payload mem_size: small, so
							 * zeroing it in the test is cheap */

/* Where ABL actually puts the BOOT ramdisk on this phone: /chosen/
 * linux,initrd-{start,end} read 0xa2000000..0xa2fd3df0 in the live DTB, and a
 * boot log from hardware shows the same (initrd=0xa2000000 end=0xa2fd3df0,
 * 0xfd3df0 = 15.8 MiB). The XNU window must not touch it - that collision is what
 * stopped the first real boot. */
#define T_ABL_RD	0xa2000000UL
#define T_ABL_RD_SIZE	0x200000UL

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
 * The bootargs property is what ABL's merged command line arrives in, and the
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
 * the FDT bound checks are worth exercising with something that does not fit,
 * and a real ABL line is 1793 bytes (so > 2048 must also be testable). */
static unsigned char fdt[16384];

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

static void build_payload_at(uint64_t rd, uint64_t phys_base, uint64_t mem_size,
			     const char *cmdline)
{
	struct payload *p = (struct payload *)rd;
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
	memset((void *)(rd + off), 0x5a, SEG0_SIZE);
	off += SEG0_SIZE;
	p->segs[1].vmaddr = VIRT_BASE + SEG1_OFF;
	p->segs[1].vmsize = SEG1_SIZE;
	p->segs[1].file_off = off;
	p->segs[1].file_size = SEG1_SIZE;
	memset((void *)(rd + off), 0xb4, SEG1_SIZE);
	off += SEG1_SIZE;
	p->adt_off = off;
	p->adt_size = ADT_SIZE;
	memset((void *)(rd + off), 0xa7, ADT_SIZE);
}

static void build_payload(uint64_t phys_base, uint64_t mem_size, const char *cmdline)
{
	build_payload_at(T_RD, phys_base, mem_size, cmdline);
}

/* One boot attempt: payload + FDT, then dl_main like start.S calls it. The
 * loader image (load/len) is put at T_RD: it must be outside the window too.
 * `bootargs` is what ABL leaves in /chosen/bootargs; NULL means "no such
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

/* One boot whose payload carries an arbitrary ADT size, so the sweep below can
 * ask what the boot_args placement does for *every* ADT the ADT builder might
 * emit rather than only for the one synthetic size used elsewhere. The ramdisk
 * length handed to the loader is stretched so that an ADT of up to 0xf000 bytes
 * passes the loader's "adt_size fits in the initrd" check (it sits right after
 * the two segments, at 0x181000). */
#define ADT_RD_LEN	0x190000ULL

static uint64_t boot_ok_adt(uint64_t adt_size)
{
	build_payload(T_XNU_BASE, T_XNU_WIN, "starqfb=0");
	((struct payload *)T_RD)->adt_size = adt_size;
	memset((void *)(T_RD + 0x181000ULL), 0xa7, adt_size);
	build_fdt(fdt, T_RD, T_RD + ADT_RD_LEN);
	memset((void *)T_FB_BASE, 0x11, 0x10000);
	memset((void *)T_RAMOOPS, 0x00, 0x10000);
	return dl_main((uintptr_t)fdt, 1, 0x30d00800UL, (uintptr_t)T_RD, 0x40000UL);
}

/* One boot with the payload where ABL really puts it (0xa2000000) and the
 * loader image at T_RD, i.e. the arrangement that failed on hardware. */
static uint64_t boot_ok_abl_rd(const char *cmdline, const char *bootargs)
{
	build_payload_at(T_ABL_RD, T_XNU_BASE, T_XNU_WIN, cmdline);
	build_fdt_args(fdt, T_ABL_RD, T_ABL_RD + PAYLOAD_LEN, bootargs);
	memset((void *)T_FB_BASE, 0x11, 0x10000);
	memset((void *)T_RAMOOPS, 0x00, 0x10000);
	return dl_main((uintptr_t)fdt, 1, 0x30d00800UL, (uintptr_t)T_RD, 0x40000UL);
}

/* The loader's own test (it is static there, so the arithmetic is repeated). */
static int overlaps(uint64_t a, uint64_t alen, uint64_t base, uint64_t size)
{
	return size && alen && a < base + size && base < a + alen;
}

/* fdt_chosen_str, but with the property built into a real FDT first: the reader
 * must cope with a bootloader line of any length, including one longer than the
 * caller's buffer. */
static int chosen_str_fdt(const char *bootargs, char *buf, unsigned buflen)
{
	build_fdt_args(fdt, 0x90000000UL, 0x90100000UL, bootargs);
	return fdt_chosen_str((uintptr_t)fdt, "bootargs", buf, buflen);
}

/* The bootloader line, as it looks on a real S9. */
#define BA_PX01 "console=ttyMSM0 androidboot.bootloader=G9600ZHS9FQL1 " \
		"msm_drm.dsi_display0=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0 " \
		"androidboot.hardware=qcom"
#define BA_PX01_TOKEN \
	"ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0"
#define BA_MQ01 "console=ttyMSM0 androidboot.bootloader=G9600ZHS9FQL1 " \
		"msm_drm.dsi_display0=ss_dsi_panel_S6E3HA8_AMB577MQ01_WQHD:config0 " \
		"androidboot.hardware=qcom"
#define BA_NONE "console=ttyMSM0 androidboot.bootloader=G9600ZHS9FQL1"

/*
 * The line ABL really leaves in /chosen/bootargs, in shape: the stock boot.img
 * cmdline as a literal prefix, ABL's own additions (root=, androidboot.serialno=,
 * ...) after it, and the panel token LAST. On this SC-38 that property is 1793
 * bytes (measured on data/live.dtb), so it is well past the old 512-byte buffer
 * - which truncated it mid-token and made the panel probe answer UNKNOWN on
 * hardware. `filler` is that line's own tokens.
 */
#define BA_FILLER \
	"rcupdate.rcu_expedited=1 console=null androidboot.hardware=qcom " \
	"video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237 " \
	"ehci-hcd.park=3 lpm_levels.sleep_disabled=1 " \
	"service_locator.enable=1 swiotlb=2048 androidboot.configfs=true " \
	"androidboot.usbcontroller=a600000.dwc3 " \
	"firmware_class.path=/vendor/firmware_mnt/image loop.max_part=7 " \
	"buildvariant=userdebug root=/dev/sda22 " \
	"androidboot.bootdevice=1d84000.ufshc " \
	"androidboot.serialno=2c69dbb021017ece androidboot.baseband=sdm "

/* at least `min` bytes of ABL-shaped line, panel token last */
static char ba_long[8192];
static const char *long_bootargs(const char *token, size_t min)
{
	ba_long[0] = 0;
	while (strlen(ba_long) + strlen(BA_FILLER) < min)
		strcat(ba_long, BA_FILLER);
	strcat(ba_long, "msm_drm.dsi_display0=");
	strcat(ba_long, token);
	strcat(ba_long, " lcd_id=0x910443 androidboot.revision=15");
	return ba_long;
}

/*
 * DSI_CTRL as a bootloaders leaves it. The vendor DTS says both S9 panels are
 * dsi_cmd_mode, so D_LINK_CMD is the likely real state and D_LINK_VIDEO the one
 * the earlier code assumed. Each case is exercised.
 */
#define D_LINK_VIDEO	0x000001b3u	/* ena|vid|clk|lane0-3 */
#define D_LINK_CMD	0x000001b5u	/* ena|cmd|clk|lane0-3 */
#define D_LINK_OFF	0x000001b4u	/* clock on, controller disabled */

/* Reset the DSI register file to a plausible live link and re-init the
 * driver, so the next boot starts from a known state. The read counter is
 * cleared too: "did the loader touch a DSI register at all" is a separate
 * question from "what did it write". starqdsisw is process state in dsi.c and
 * would otherwise leak from one boot() to the next. */
static void dsi_link(uint32_t ctrl)
{
	memset(starq_dsi_regs_ctrl, 0, sizeof(starq_dsi_regs_ctrl));
	memset(starq_dsi_regs_phy, 0, sizeof(starq_dsi_regs_phy));
	starq_dsi_regs_ctrl[0x004 / 4] = ctrl;
	starq_dsi_regs_ctrl[0x008 / 4] = 0;		/* idle */
	starq_dsi_regs_ctrl[0x028 / 4] = 0x0c5c0080u;	/* active_v start/end */
	starq_dsi_regs_phy[0x0ec / 4] = 1;		/* refgen ready */
	starq_dsi_trace_n = 0;
	starq_dsi_rd_n = 0;
	starq_dsi_set_sw_trigger(0);
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

/* How many register writes hit offset `off` with a non-zero value. */
static unsigned dsi_count_nz(uint32_t off)
{
	unsigned n = 0;

	for (unsigned i = 0; i < starq_dsi_trace_n; i++)
		if (starq_dsi_trace[i].off == off && starq_dsi_trace[i].val)
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
	map_pa(T_ABL_RD, T_ABL_RD_SIZE);

	/* The test's idea of the board must match the loader's. */
	printf("== constants\n");
	check(1, "framebuffer %#lx, ramoops %#lx, window %#lx+%#lx",
	      (unsigned long)T_FB_BASE, (unsigned long)T_RAMOOPS,
	      (unsigned long)T_XNU_BASE, (unsigned long)T_XNU_SIZE);

	printf("== boot_args / topOfKernelData are 16 KiB aligned for every ADT size\n");
	{
		/* XNU is built with __ARM_16K_PG__ here, and arm_vm_physmap_slide()
		 * asserts (virtBase + (topOfKernelData - physBase)) & 0x3fff == 0.
		 * The old loader rounded boot_args up by only 4 KiB, which held
		 * only when the ADT size happened to be a multiple of 16 KiB - the
		 * real ADT is 0x312c bytes, so on hardware topOfKernelData came out
		 * at remainder 0x1000 and the kernel panicked at
		 * arm_vm_init.c:1491. Sweep every ADT size through the loader's own
		 * expression: the property must hold unconditionally, not by luck. */
		unsigned bad = 0;
		uint64_t worst = 0;

		for (uint64_t adt = 0; adt <= 0x8000; adt++) {
			uintptr_t adt_pa = T_XNU_BASE;	/* kernel_end: 16K clean */
			uintptr_t top = starq_top_of_kernel_data(adt_pa, adt);

			if (top & 0x3fff) {
				if (!bad)
					worst = adt;
				bad++;
			}
		}
		check(bad == 0,
		      "ADT sizes 0..0x8000: topOfKernelData is 16K aligned in all %u cases (%u bad, first %#llx)",
		      0x8001, bad, (unsigned long long)worst);
		/* The specific shape that failed: the real board's payload. Measured
		 * from xnu/out-starqltechn/kernel.development.starqltechn via
		 * tools/mkpayload.py: virt_base 0xfffffe0006000000, kernel_end
		 * 0xfffffe00084c4000 (= 0xd64c4000 PA), ADT 12588 bytes (0x312c).
		 * Before this fix: top 0xd64c9000, orig_va
		 * 0xfffffe00084c9000, remainder 0x1000 -> the hardware panic. */
		check(starq_boot_args_pa(0xd64c4000UL, 0x312c) == 0xd64c8000UL &&
		      starq_top_of_kernel_data(0xd64c4000UL, 0x312c) == 0xd64cc000UL &&
		      (0xfffffe0006000000ULL +
		       (starq_top_of_kernel_data(0xd64c4000UL, 0x312c) - 0xd4000000UL)) ==
		      0xfffffe00084cc000ULL,
		      "the real payload: adt 0xd64c4000, boot_args 0xd64c8000, top 0xd64cc000, "
		      "orig_va 0xfffffe00084cc000 (16K clean)");
		/* And the old 4 KiB rule, stated as the counter-example. */
		check(((0xd64c4000UL + 0x312c + 0x1000 - 1) & ~0xfffUL) + 0x1000 ==
		      0xd64c9000UL &&
		      (0xfffffe00084c9000ULL & 0x3fff) == 0x1000,
		      "the old 4 KiB placement gave top 0xd64c9000 / orig_va 0xfffffe00084c9000, "
		      "remainder 0x1000 -> the arm_vm_physmap_slide() panic");
	}

	/* End to end: the same invariant, read back out of a live boot_args that
	 * the loader actually wrote, with several ADT sizes inside the payload's
	 * slack. */
	{
		unsigned bad = 0;
		static const uint64_t sizes[] = { 0x1000, 0x1004, 0x1810, 0x1fff, 0x2000 };

		for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			struct boot_args *b;

			if (boot_ok_adt(sizes[i]) == 0) {
				bad++;
				continue;
			}
			b = (struct boot_args *)(uintptr_t)starq_boot_args;
			if ((b->topOfKernelData % 0x4000) ||
			    ((b->virtBase + (b->topOfKernelData - b->physBase)) & 0x3fff))
				bad++;
		}
		check(bad == 0, "booted %u payloads with odd ADT sizes, all with a 16K-clean top",
		      (unsigned)(sizeof(sizes) / sizeof(sizes[0])));
	}

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
	check(ba->topOfKernelData == starq_boot_args + 0x4000, "topOfKernelData");
	/* XNU on this board is built with __ARM_16K_PG__ and
	 * arm_vm_physmap_slide() asserts (orig_va & ARM_PGMASK) == 0 with
	 * orig_va = virtBase + (topOfKernelData - physBase). This is the
	 * invariant a 4 KiB boot_args placement broke on hardware
	 * (arm_vm_init.c:1491 panic on "MEET EARLY 7"). */
	{
		uint64_t orig_va = ba->virtBase +
			(ba->topOfKernelData - ba->physBase);

		check(ba->topOfKernelData % 0x4000 == 0,
		      "topOfKernelData %#llx is 16K aligned (%#llx)",
		      (unsigned long long)ba->topOfKernelData,
		      (unsigned long long)(ba->topOfKernelData & 0x3fff));
		check((orig_va & 0x3fff) == 0,
		      "orig_va %#llx passes arm_vm_physmap_slide()'s assert (rem %#llx)",
		      (unsigned long long)orig_va,
		      (unsigned long long)(orig_va & 0x3fff));
		check(ba->physBase == T_XNU_BASE &&
		      ba->topOfKernelData > ba->physBase &&
		      ba->topOfKernelData <= ba->physBase + ba->memSize,
		      "topOfKernelData inside the window (adt %#llx, boot_args %#llx)",
		      (unsigned long long)(T_XNU_BASE + kernel_end_va() - VIRT_BASE),
		      (unsigned long long)starq_boot_args);
	}
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

	printf("== the XNU window vs what ABL leaves behind\n");
	{
		/* Pure arithmetic first, so a wrong window fails here with the
		 * numbers on screen instead of as a mystifying boot refusal. */
		check(!overlaps(T_ABL_RD, T_ABL_RD_SIZE, T_XNU_BASE, T_XNU_SIZE),
		      "the ABL ramdisk (%#lx) is outside the window (%#lx+%#lx)",
		      (unsigned long)T_ABL_RD, (unsigned long)T_XNU_BASE,
		      (unsigned long)T_XNU_SIZE);
		check(!overlaps(T_FB_BASE, T_FB_SIZE, T_XNU_BASE, T_XNU_SIZE),
		      "the framebuffer (%#lx) is outside the window",
		      (unsigned long)T_FB_BASE);
		check(!overlaps(T_RAMOOPS, T_RAMOOPS_SIZE, T_XNU_BASE, T_XNU_SIZE),
		      "ramoops (%#lx) is outside the window", (unsigned long)T_RAMOOPS);
		check(T_XNU_BASE + T_XNU_SIZE == 0xff000000UL,
		      "the window ends exactly at qseecom_ta_region (%#lx)",
		      (unsigned long)(T_XNU_BASE + T_XNU_SIZE));
		check(T_XNU_SIZE == 0x2b000000UL, "688 MiB, not the old 384 MiB (%#lx)",
		      (unsigned long)T_XNU_SIZE);
		/* The old window: this is the bug, stated as arithmetic. It
		 * started exactly on the ramdisk ABL loads. */
		check(overlaps(T_ABL_RD, T_ABL_RD_SIZE, 0xa2000000UL, 0x18000000UL),
		      "the OLD window 0xa2000000+0x18000000 did overlap the ramdisk");

		/* ...and the same through dl_main, with the payload where ABL
		 * actually puts it. This is the arrangement that failed on
		 * hardware; it must boot now. */
		entry = boot_ok_abl_rd("starqfb=0", BA_PX01);
		check(entry == T_XNU_BASE + ENTRY_OFF,
		      "payload at 0xa2000000 boots with the new window (entry %#lx)",
		      (unsigned long)entry);
		check(*(volatile uint64_t *)T_XNU_BASE == 0x5a5a5a5a5a5a5a5aULL,
		      "and the segments landed in the window, not on the ramdisk");
		/* the ramdisk itself is untouched by the zeroing */
		check(*(volatile char *)T_ABL_RD == 'S',
		      "the payload at 0xa2000000 was not zeroed (%c)",
		      *(volatile char *)T_ABL_RD);

		/* The lenient direction must stay closed: ask for the old window
		 * on the boot-args, with the payload inside it, and the overlap
		 * check must still refuse. Moving the window is the fix, not
		 * weakening the check. */
		{
			build_payload_at(T_ABL_RD, T_ABL_RD, T_XNU_WIN,
					 "starqfb=0 starqwin=0xa2000000:0x18000000");
			build_fdt_args(fdt, T_ABL_RD, T_ABL_RD + PAYLOAD_LEN, BA_PX01);
			check(dl_main((uintptr_t)fdt, 1, 0x30d00800UL,
				      (uintptr_t)T_RD, 0x40000UL) == 0,
			      "starqwin=0xa2000000 with the payload there is still refused");
			check(log_has("payload sits inside the XNU RAM window"),
			      "and the log names the overlap");
		}
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
	check(boot_ok("starqwin=0xd4000000:0x200000") != 0,
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
		check(memmem(text, len, "phys=0xd4000000", 15) != NULL, "payload RAM logged");
		/* The boot_args placement is the thing the kernel asserts on, so the
		 * evidence must be in the log *before* the jump, not only in the
		 * host test: adt/boot_args/top/orig_va and the alignment verdict. */
		check(memmem(text, len, "adt=0x", 6) != NULL &&
		      memmem(text, len, "boot_args=0x", 12) != NULL &&
		      memmem(text, len, "top=0x", 6) != NULL &&
		      memmem(text, len, "orig_va=0x", 10) != NULL,
		      "adt / boot_args / top / orig_va logged");
		check(memmem(text, len, "top 16K-aligned: yes", 20) != NULL,
		      "and the 16K alignment verdict is explicitly logged as yes");
		check(text[0] == 'S' && !memcmp(text, "S9LOADER BEGIN", 14),
		      "text starts at zone+12 with the header line");
		/* This board's bootloader is Qualcomm ABL, not an Exynos S-Boot:
		 * the line on the panel must not claim otherwise. */
		check(memmem(text, len, "starqltechn ABL XNU loader", 26) != NULL &&
		      memmem(text, len, "S-Boot", 6) == NULL,
		      "the header line says ABL, not S-Boot");
		/* the zone must not run past its 0x40000 bytes */
		check(len <= 0x40000 - 12, "log fits the zone (%u)", len);
		/* the ftrace zone at 0xa13c0000 must be untouched */
		check(*(volatile uint32_t *)(0xa13c0000UL) == 0, "ftrace zone untouched");
	}

	printf("== DSI: panel probe from the bootloader cmdline\n");
	{
		char tok[96];
		uint64_t rd_start;

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

		/* An Image.gz-dtb appends the DTBs to the kernel as an unpadded
		 * cat, so the tree the bootloader hands over can start at an odd
		 * address (the stock starqltechn set has a member at 0xb5c9d).
		 * With the MMU off
		 * that makes an unaligned u32 load fault, and aligning absolute
		 * addresses instead of in-tree offsets misparses the tree. */
		build_fdt_args(bad + 1, 0x90000000UL, 0x90100000UL, BA_PX01);
		check(fdt_chosen_str((uintptr_t)(bad + 1), "bootargs", full, sizeof(full)) &&
		      strcmp(full, BA_PX01) == 0,
		      "an odd-offset FDT parses: offsets align inside the tree, not as addresses");
		check(fdt_chosen_u64((uintptr_t)(bad + 1), "linux,initrd-start", &rd_start) &&
		      rd_start == 0x90000000UL,
		      "the odd-offset tree yields the initrd start too");

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

	printf("== a real-length ABL bootargs, panel token LAST\n");
	{
		/* The live DTB on this phone (data/live.dtb) carries a 1793-byte
		 * /chosen/bootargs: the stock cmdline, then ABL's own additions
		 * (root=, androidboot.serialno=, ...), then
		 * msm_drm.dsi_display0=<panel>: - LAST. A 512-byte buffer cut it
		 * mid-token at "msm_drm.dsi_di" and the phone printed
		 * panel=UNKNOWN. These cases are that exact shape. */
		char buf[2048 + 16];
		const char *s;
		size_t n;
		int guard;

		s = long_bootargs(BA_PX01_TOKEN, 1700);
		n = strlen(s);
		check(n > 512 && n <= 2048,
		      "PX01 line is %zu bytes (> 512, the real line is 1793)", n);

		memset(buf, 0xaa, sizeof(buf));
		check(chosen_str_fdt(s, buf, 2048) && strcmp(buf, s) == 0,
		      "fdt_chosen_str returns the whole %zu-byte line", n);
		check(starq_dsi_panel_from_cmdline(s, buf, sizeof(buf)) ==
		      STARQ_PANEL_PX01 &&
		      strcmp(buf, BA_PX01_TOKEN) == 0,
		      "token at the END of a >512-byte line -> PX01 (\"%s\")", buf);

		/* ...through the loader, which is where the 512-byte buffer was */
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		dsi_link(D_LINK_VIDEO);
		check(boot_ok_args("starqfb=0", s) != 0, "booted with the long line");
		check(log_has("panel=PX01 name=" BA_PX01_TOKEN),
		      "the loader logs PX01 with the full token");
		check(log_has("msm_drm.dsi_display0=" BA_PX01_TOKEN),
		      "and prints the whole bootargs, not a 512-byte prefix");

		s = long_bootargs("ss_dsi_panel_S6E3HA8_AMB577MQ01_WQHD:config0", 1700);
		memset(buf, 0xaa, sizeof(buf));
		check(starq_dsi_panel_from_cmdline(s, buf, sizeof(buf)) ==
		      STARQ_PANEL_MQ01,
		      "MQ01 token at the END of a >512-byte line -> MQ01");
		dsi_link(D_LINK_VIDEO);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0", s) != 0, "booted");
		check(log_has("panel=MQ01 name=ss_dsi_panel_S6E3HA8_AMB577MQ01_WQHD:config0"),
		      "the loader logs MQ01 with the full token");
		check(log_has("dcs b5 8f dc 0a"),
		      "and the MQ01 ELVSS byte (0x0a) follows from it");

		/* A line longer than the buffer must truncate and terminate, not
		 * overrun: the read is bootloader input. */
		s = long_bootargs(BA_PX01_TOKEN, 3000);
		n = strlen(s);
		check(n > 2048, "the oversize line is %zu bytes (> 2048)", n);
		memset(buf, 0xaa, sizeof(buf));
		check(chosen_str_fdt(s, buf, 2048) == 1, "an oversize line is read");
		check(strlen(buf) == 2047, "truncated to 2047 chars + NUL (%zu)",
		      strlen(buf));
		guard = 1;
		for (unsigned i = 2048; i < sizeof(buf); i++)
			if ((unsigned char)buf[i] != 0xaa)
				guard = 0;
		check(guard, "nothing is written past the 2048-byte buffer");
		/* The token is past the cut, so the honest answer is UNKNOWN -
		 * and the loader must still boot and say so. */
		dsi_link(D_LINK_VIDEO);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0", s) != 0,
		      "a >2048-byte bootargs boots (no overflow, no fault)");
		check(log_has("panel=UNKNOWN"), "and reports UNKNOWN, not garbage");
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

		/* --- starqelvss=off: not one DSI register is accessed ---
		 * The point of the switch is the avoidance of the mode probe
		 * itself: a read of an unclocked MDSS block can hang the
		 * interconnect, so "nothing sent" is not good enough. The host
		 * driver counts every rd32(), so this asserts the strong
		 * property rather than the weak one. */
		dsi_link(D_LINK_CMD);	/* a probe would read 0x1b5 */
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqelvss=off", BA_PX01) != 0, "booted");
		check(starq_dsi_rd_n == 0, "starqelvss=off: 0 DSI register reads (%u)",
		      starq_dsi_rd_n);
		check(dsi_count(0x090, 1) == 0, "starqelvss=off sends nothing (%u)",
		      dsi_count(0x090, 1));
		check(log_has("starqelvss=off, no DSI register access at all"),
		      "and says that no register was touched");
		check(!log_has("DSI_CTRL=") && !log_has("panel in COMMAND mode") &&
		      !log_has("STARQ DSI: ctrl="),
		      "no probe line, no mode verdict, no controller address");
		/* The panel probe is string parsing only: it still runs, and it is
		 * labelled STARQ panel:, not STARQ DSI:, so the log never claims
		 * DSI activity the switch has forbidden. */
		check(log_has("STARQ panel: panel=PX01 name=" BA_PX01_TOKEN),
		      "the panel token is still logged, as STARQ panel:");

		/* --- starqdsi=0: the same guarantee, independently --- */
		dsi_link(D_LINK_CMD);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqdsi=0", BA_MQ01) != 0, "booted");
		check(starq_dsi_rd_n == 0, "starqdsi=0: 0 DSI register reads (%u)",
		      starq_dsi_rd_n);
		check(dsi_count(0x090, 1) == 0, "starqdsi=0 sends nothing (%u)",
		      dsi_count(0x090, 1));
		check(log_has("STARQ DSI: starqdsi=0, no DSI register access at all"),
		      "and names the switch that fired");
		check(!log_has("DSI_CTRL=") && !log_has("STARQ DSI: ctrl="),
		      "no probe line and no controller address");

		/* --- both switches: off wins, and the log says which fired --- */
		dsi_link(D_LINK_CMD);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqelvss=off starqdsi=0", BA_PX01) != 0,
		      "booted");
		check(starq_dsi_rd_n == 0, "both switches: 0 DSI register reads (%u)",
		      starq_dsi_rd_n);
		check(log_has("STARQ DSI: starqelvss=off and starqdsi=0, "
			      "no DSI register access at all"),
		      "both are named, and the stronger statement holds");

		/* --- neither switch: the probe does run and does read --- */
		dsi_link(D_LINK_CMD);	/* the register says command mode */
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0", BA_PX01) != 0, "booted");
		check(starq_dsi_rd_n > 0, "no switch: the probe reads registers (%u)",
		      starq_dsi_rd_n);
		check(log_has("STARQ DSI: ctrl=0xae94000 phy=0xae94400"),
		      "the controller and PHY addresses are logged");
		check(log_has("DSI_CTRL=0x000001b5 en=1 video=0 cmd=1"),
		      "the mode probe line prints all eight hex digits");
		check(log_has("ack_err=0x00000000 timeout=0x00000000"),
		      "the Ack_Err and Timeout status words are logged");
		check(log_has("panel in COMMAND mode"), "command mode recognised");
		check(log_has("ack_err=") && log_has("timeout="),
		      "DSI_ACK_ERR_STATUS and DSI_TIMEOUT_STATUS are logged");

		/* --- a non-zero or unparsable starqdsi leaves DSI on --- */
		dsi_link(D_LINK_VIDEO);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqdsi=1", BA_PX01) != 0, "booted");
		check(starq_dsi_rd_n > 0 && dsi_count(0x090, 1) == 3,
		      "starqdsi=1 keeps DSI on (%u reads, %u sends)",
		      starq_dsi_rd_n, dsi_count(0x090, 1));
		dsi_link(D_LINK_VIDEO);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqdsi=zz", BA_PX01) != 0, "booted");
		check(starq_dsi_rd_n > 0 && log_has("bad starqdsi"),
		      "an unparsable starqdsi is logged and DSI stays on");

		/* --- a boot-arg still overrides everything --- */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqelvss=b5,19,dc,0a", BA_PX01) != 0,
		      "booted");
		check(dsi_count(0x090, 1) == 1 && log_has("dcs b5 19 dc 0a"),
		      "starqelvss= replaces the default (1 command, as given)");
		check(log_has("explicit sequence"), "and is logged as explicit");

		/* --- the switch is a token, not the rest of the cmdline ---
		 * arg_val() returns everything after "needle"; before the
		 * token-bounded read, "starqelvss=off starqfb=0" compared as
		 * "off starqfb=0" and the kill switch silently did not fire. */
		dsi_link(D_LINK_CMD);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 starqelvss=off trailing=1",
				   BA_PX01) != 0, "booted");
		check(starq_dsi_rd_n == 0,
		      "starqelvss=off with args after it still reads no register (%u)",
		      starq_dsi_rd_n);
		check(log_has("starqelvss=off, no DSI register access at all"),
		      "and fires with trailing boot-args present");
		/* ...and the reverse order, which the boot-arg list invites. */
		dsi_link(D_LINK_CMD);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqdsi=0 starqelvss=off", BA_PX01) != 0, "booted");
		check(starq_dsi_rd_n == 0 && dsi_count(0x090, 1) == 0,
		      "either order: 0 reads, 0 sends (%u)", starq_dsi_rd_n);
		/* A lookalike must still not match. */
		dsi_link(D_LINK_VIDEO);
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqfb=0 nostarqdsi=0 starqdsioff=1", BA_PX01) != 0,
		      "booted");
		check(starq_dsi_rd_n > 0 && dsi_count(0x090, 1) == 3,
		      "nostarqdsi=0 / starqdsioff=1 do not disable DSI");

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
		/* ...and only the exact token: "10" is not "1". */
		dsi_link(D_LINK_VIDEO);
		starq_dsi_trace_n = 0;
		memset((void *)T_RAMOOPS, 0, T_RAMOOPS_SIZE);
		check(boot_ok_args("starqdsisw=10 starqelvss=b5,8f,dc,16", BA_PX01) != 0,
		      "booted");
		check(dsi_count_nz(0x100) > 0,
		      "starqdsisw=10 does not match starqdsisw=1 (schedule is programmed)");

		/* --- a command-mode link never programs the schedule --- */
		dsi_link(D_LINK_CMD);
		starq_dsi_trace_n = 0;
		check(boot_ok_args("starqfb=0", BA_PX01) != 0, "booted");
		check(dsi_count_nz(0x100) == 0 && dsi_count(0x090, 1) == 3,
		      "command mode: 3 commands, no DMA schedule programmed");

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
