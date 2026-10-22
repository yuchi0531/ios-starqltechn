/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy S9 (SM-G9600 / SC-02K, "starqltechn", Qualcomm SDM845) XNU loader.
 *
 * The bootloader on this phone is Qualcomm **ABL** (`bota` carries `abl.elf`),
 * as on sweet. Here it boots the kernel slot of a Samsung boot.img the way a
 * Linux bootloader would: it loads it at the header's kernel_addr, enters at
 * EL1 with the MMU and caches off with x0 = DTB, and hands the ramdisk over
 * through /chosen/linux,initrd-{start,end}. (The S8 reference port this file
 * descends from ran on Samsung's Exynos S-Boot, which uses the same Android
 * boot.img contract - that is why the shape transferred.) The ramdisk holds a
 * tools/mkpayload.py "SWEETXNU" v1 container (Mach-O segments, ADT, boot-args),
 * the same format the sweet shim consumes, so this file is the ABL ->
 * boot_args translation layer: find the payload, validate it, zero the RAM
 * window XNU is about to own, copy the segments, build boot_args and jump.
 *
 * Ported from Floppikrob/ios-galaxy-s8 shim/dreamlte/loader.c (Exynos 8895,
 * reaches the iOS lock screen). Everything Exynos is gone:
 *   - the DECON HW_SW_TRIG unmask at 0x12860070 (S9 has MDSS/DSI),
 *   - the PMIC/MUIC RTC read and the "s8rtc=" boot-arg (SPMI PM8998 here, and
 *     patch 0124 that consumes it does not exist in this tree),
 *   - the ufs/smp/touch/pmic/display probes and the 2 GiB high-bank DMA target.
 * Kept, because it is the part that makes the hand-off safe:
 *   - the whole validation chain (payload magic/version, segment bounds, the
 *     phys_base window, entry inside the kernel),
 *   - zeroing the XNU RAM window (ABL leaves the previous OS's RAM behind;
 *     XNU hung in vm_mem_bootstrap on the S8 depending on how the phone had
 *     been rebooted, which is why the S8 port zeroes it),
 *   - the dual-zone ramoops log, for the same forced-reboot-corrupts-a-header
 *     reason.
 *
 * Boot-args / build-arg overrides (all optional):
 *   starqfb=0          do not paint the framebuffer test pattern
 *   starqfb=1          paint it even when built with STARQ_FB_TEST=0
 *   starqwin=BASE:SIZE accept a different XNU RAM window (hex, ": " separated),
 *                      e.g. starqwin=0xD4000000:0x2B000000. Only the accepted
 *                      window changes: the payload's phys_base/mem_size from
 *                      the ADT stay authoritative for the copies.
 *   starqelvss=LIST    override the panel ELVSS write. LIST is a DCS hex list,
 *                      e.g. starqelvss=b5,8f,dc,0a or
 *                      starqelvss=f0,5a,5a;b5,8f,dc,0a: bytes are hex, ','
 *                      separates the bytes of one command, ';' the commands.
 *                      starqelvss=off disables the override AND every DSI
 *                      register access (no mode probe either). Absent -> the
 *                      panel-aware default (see STARQ_ELVSS_*), which is ON:
 *                      level-1 key, "b5 8f dc <elvss>", level-1 key off.
 *   starqdsi=0         same guarantee as starqelvss=off, independently: no DSI
 *                      register access at all. Same name and meaning as the
 *                      kernel-side switch in xnu/patches/0083. Either switch
 *                      is enough; this one is the one to reach for when the
 *                      payload cmdline still has to carry starqelvss= values.
 *   starqmps=VAL       byte 2 (MPS) of the default 0xB5 write, 1-2 hex digits.
 *                      Default 0xdc, which is what the vendor driver uses above
 *                      39 cd; 0xcc below that.
 *   starqdsisw=1       trigger the command DMA immediately instead of
 *                      scheduling it into the vertical blanking window.
 */
#include <stdint.h>
#include "../fdt.h"
#include "../font.h"
#include "dsi.h"
#include "loader.h"

/* ---- board facts (sdm845-samsung-starqltechn.dts, sdm845.dtsi) ----
 * Every hardware address is #ifndef-guarded so that host_test.c can point it at
 * a host buffer and run this file's validation and copy path on a desktop. */
#ifndef FB_BASE
#define FB_BASE			0x9d400000UL	/* reserved-memory framebuffer_mem */
#endif
#ifndef FB_W
#define FB_W			1440
#endif
#ifndef FB_H
#define FB_H			2960
#endif
#ifndef FB_STRIDE
#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
#endif
#ifndef FB_BYTES
#define FB_BYTES		0x2400000UL	/* the whole reserved carve-out */
#endif

/* ramoops@0xa1300000, 0x100000 (mainline sdm845-samsung-starqltechn.dts and
 * the vendor DT: record-size = console-size = ftrace-size = pmsg-size =
 * 0x40000). fs/pstore/ram.c allocates the zones in this fixed order whatever
 * order the DT properties are in:
 *
 *   0xa1300000  dmesg/record 0x40000  -> /sys/fs/pstore/dmesg-ramoops-0
 *   0xa1340000  console      0x40000  -> console-ramoops-0   <- our log
 *   0xa1380000  pmsg         0x40000  -> pmsg-ramoops-0      <- our log (2nd copy)
 *   0xa13c0000  ftrace       0x40000     (left untouched)
 *
 * Each zone starts with {u32 "DBGC", u32 start, u32 size} (struct
 * persistent_ram_buffer, fs/pstore/ram_core.c) and the text follows at +12.
 * Writing sequentially with start == size == length makes the kernel hand the
 * zone back as one linear string (data[start..size] then data[0..start]).
 * tools/starq-flash.sh pull-log reads both files. */
#ifndef RAMOOPS_BASE
#define RAMOOPS_BASE		0xa1300000UL
#endif
#ifndef RAMOOPS_SIZE
#define RAMOOPS_SIZE		0x100000UL
#endif
#ifndef RAMOOPS_CONSOLE
#define RAMOOPS_CONSOLE		0xa1340000UL
#endif
#ifndef RAMOOPS_PMSG
#define RAMOOPS_PMSG		0xa1380000UL
#endif
#ifndef RAMOOPS_ZONE_SIZE
#define RAMOOPS_ZONE_SIZE	0x40000UL
#endif
#define PRZ_SIG			0x43474244U	/* "DBGC" */

/* The RAM window this board is allowed to hand to XNU.
 *
 * 0xd4000000 + 0x2b000000 (688 MiB) is the second-largest free gap in the live
 * DTB's reserved-memory map, and the only one big enough that is also clear of
 * everything the bootloader leaves behind. Measured, not guessed:
 *
 *   below  camera_mem_region   0xc0000000 + 0x14000000, ends at 0xd4000000
 *   above  qseecom_ta_region   0xff000000 + 0x01000000, starts at 0xff000000
 *
 * so the window is exactly bounded by two carve-outs and ends at 0xff000000.
 * Clear of:
 *   - the payload: ABL puts the BOOT ramdisk at 0xa2000000
 *     (/chosen/linux,initrd-{start,end}, 0xa2000000..0xa2fd3df0 on hardware,
 *     0xfd3df0 = 15.8 MiB, ending well below 0xd4000000) - the previous window,
 *     0xa2000000 + 0x18000000, *started on* that ramdisk, which is exactly the
 *     collision the loader's overlap check reported and refused to boot,
 *   - the framebuffer   cont_splash_region 0x9d400000 + 0x2400000,
 *   - ramoops           0xa1300000 + 0x100000.
 *
 * The largest gap (0xa1c01000 + 0x1e3ff000, 483 MiB) is NOT usable: the payload
 * sits in the middle of it.
 *
 * Must match tools/mkadt.py's BOARDS['starqltechn'] dram_base/dram_size and
 * tools/starq-boot.sh's RAM_BASE/RAM_SIZE, i.e. the ADT and the payload's
 * phys_base/mem_size. */
#ifndef XNU_RAM_BASE
#define XNU_RAM_BASE		0xd4000000UL
#endif
#ifndef XNU_RAM_SIZE
#define XNU_RAM_SIZE		0x2b000000UL
#endif

/* ABL puts the BOOT ramdisk in the low 4 GB bank; only 0x80000000..4G is
 * DRAM on this board. */
#ifndef LOW_RAM_BASE
#define LOW_RAM_BASE		0x80000000UL
#endif
#ifndef LOW_RAM_END
#define LOW_RAM_END		0x100000000UL
#endif

/* Framebuffer test pattern: ON by default for bring-up. It answers the open
 * question of the port - does this bootloader leave the display scanning out of
 * the buffer at FB_BASE (CPU copies reach the panel, as on sweet), or is the
 * panel in command mode with its own GRAM (nothing we write shows up and MDSS/
 * DSI has to be programmed)? The vendor DTS says both S9 panels are
 * dsi_cmd_mode, so the second answer is the likely one; the DSI_CTRL line the
 * loader prints is the deciding evidence either way. Build with
 * -DSTARQ_FB_TEST=0 or pass starqfb=0 to turn the pattern off. */
#ifndef STARQ_FB_TEST
#define STARQ_FB_TEST		1
#endif

/* Panel ELVSS override, sent over DSI after the mode probe and before the jump
 * to XNU (see dsi.c and STARQ_ELVSS_* in dsi.h). ON by default and panel-aware:
 *
 *   f0 5a 5a              level-1 key: 0xB5 is in the protected bank
 *   b5 8f dc <elvss>      TSET 0x8f (-15 C), MPS 0xdc, ELVSS offset
 *   f0 a5 a5              level-1 key off
 *
 * with <elvss> = 0x16 on AMB577PX01 and 0x0a on AMB577MQ01, from the panel name
 * in the DTB's /chosen/bootargs. An unknown panel gets the PX01 value (see
 * dsi.h for why). starqelvss=<hex list> overrides all of it; starqelvss=off and
 * starqdsi=0 both turn the whole DSI path off before any register is read.
 *
 * STARQ_ELVSS_DEFAULT is only for baking a *specific* sequence into an image
 * instead; leave it empty for the policy above. */
#ifndef STARQ_ELVSS_DEFAULT
#define STARQ_ELVSS_DEFAULT	""
#endif
/* MPS (byte 2): 0xdc above 39 cd, 0xcc below (ss_elvss() in the vendor driver).
 * starqmps=<1-2 hex digits> overrides it. */
#ifndef STARQ_ELVSS_MPS_DEFAULT
#define STARQ_ELVSS_MPS_DEFAULT	STARQ_ELVSS_MPS
#endif

/* /chosen/bootargs is ABL's merged Linux command line: the stock boot.img
 * cmdline plus ABL's own additions (root=, androidboot.serialno=, ... and last
 * msm_drm.dsi_display0=<panel>:). It is LONG: the live DTB on this SC-38 carries
 * 1793 bytes, and the panel token - the one thing the loader needs from it -
 * comes after ABL's additions, i.e. near the end. A 512-byte buffer truncated
 * it mid-token ("... msm_drm.dsi_di") and the panel probe then answered
 * UNKNOWN on real hardware. 2048 covers this phone's full line with margin;
 * fdt_chosen_str() truncates (and terminates) rather than overrunning if a
 * future bootloader line is longer still. */
#define STARQ_BOOTARGS_MAX	2048
#define STARQ_PANEL_TOKEN_MAX	96

/* Cache maintenance and barriers mean nothing on the host, where
 * shim/starqltechn/host_test.c links this file to actually run the validation
 * and copy path: with no cross compiler on a desktop, that is the only way this
 * code gets executed at all. */
#if defined(__aarch64__)
#define cpu_barrier()	__asm__ volatile("dsb sy" ::: "memory")
#else
#define cpu_barrier()	do { } while (0)
#endif

/* ---- persistent log ---- */
struct prz {
	uint32_t sig, start, size;
	uint8_t data[];
};

static volatile struct prz *const prz = (volatile struct prz *)RAMOOPS_CONSOLE;
static volatile struct prz *const prz2 = (volatile struct prz *)RAMOOPS_PMSG;
#define PRZ_DATA_MAX		(RAMOOPS_ZONE_SIZE - 12)
static uint32_t log_len, log2_len;

static void prz_put(volatile struct prz *z, uint32_t *len, char c)
{
	if (*len >= PRZ_DATA_MAX)
		return;
	z->data[(*len)++] = (uint8_t)c;
	z->start = *len;
	z->size = *len;
}

/* Both zones: a forced reboot (Power + Vol Down) can leave a torn header in
 * one of them, and pstore then drops that zone ("found existing invalid
 * buffer"). Two copies mean one usually survives. */
static void log_ch(char c)
{
	prz_put(prz, &log_len, c);
	prz_put(prz2, &log2_len, c);
}

/* ---- screen console ---- */
static volatile uint32_t *const fb = (volatile uint32_t *)FB_BASE;
#define SCALE		3
#define CELL_W		(8 * SCALE)
#define CELL_H		(16 * SCALE + 6)
#define TEXT_TOP	272		/* below the 4 test bands (0..255) */
#define COLS		((FB_W - 48) / CELL_W)
static int row, col;
static uint32_t fg = 0xffffffffU;

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h && y < FB_H; y++) {
		if (y < 0)
			continue;
		volatile uint32_t *line = fb + y * (FB_STRIDE / 4);
		for (int x = x0; x < x0 + w && x < FB_W; x++)
			if (x >= 0)
				line[x] = c;
	}
}

/* Framebuffer test pattern. Four 64-line bands at the top (white, red, green,
 * blue, in that order) plus a large solid block with a nested square below the
 * text: if the bands and the block are on the panel, the bootloader is
 * scanning out of FB_BASE and the CPU-copy display path works; if the panel
 * shows the Samsung logo or black, it is a command-mode panel and MDSS/DSI has
 * to be brought up instead. A wrong stride shears the nested square. */
#define BAND_H		64
#define BLOCK_TOP	(FB_H * 2 / 3)
#define BLOCK_H		(FB_H / 6)
static void fb_pattern_regions(uint32_t white, uint32_t red, uint32_t green,
			       uint32_t blue, uint32_t block, uint32_t inner)
{
	fill(0, 0 * BAND_H, FB_W, BAND_H, white);
	fill(0, 1 * BAND_H, FB_W, BAND_H, red);
	fill(0, 2 * BAND_H, FB_W, BAND_H, green);
	fill(0, 3 * BAND_H, FB_W, BAND_H, blue);
	fill(FB_W / 4, BLOCK_TOP, FB_W / 2, BLOCK_H, block);
	fill(FB_W / 2 - 96, BLOCK_TOP + 96, 192, 192, inner);
	cpu_barrier();
}

static void fb_pattern(void)
{
	fb_pattern_regions(0xffffffffU, 0xffff0000U, 0xff00ff00U, 0xff0000ffU,
			   0xffff00ffU, 0xff00ffffU);
}

/* Undo fb_pattern() when the boot-args say starqfb=0: same regions, black. */
static void fb_pattern_clear(void)
{
	fb_pattern_regions(0xff000000U, 0xff000000U, 0xff000000U, 0xff000000U,
			   0xff000000U, 0xff000000U);
}

/* Keep the text above the block so the pattern stays readable. */
#define ROWS		((BLOCK_TOP - TEXT_TOP) / CELL_H)

static void scr_ch(char c)
{
	if (c == '\n' || col >= COLS) {
		col = 0;
		row = (row + 1) % ROWS;
		fill(0, TEXT_TOP + row * CELL_H, FB_W, CELL_H, 0xff000000U);
		if (c == '\n')
			return;
	}
	if (c < 0x20 || c > 0x7e)
		c = '?';
	const unsigned char *g = font8x16[c - 0x20];
	int x0 = 24 + col * CELL_W, y0 = TEXT_TOP + row * CELL_H;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			fill(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			     (g[gy] & (0x80 >> gx)) ? fg : 0xff000000U);
	col++;
}

static void out(const char *s)
{
	while (*s) {
		log_ch(*s);
		scr_ch(*s++);
	}
}

static void hex(uint64_t v)
{
	static const char digits[] = "0123456789abcdef";
	char b[19] = "0x";
	int n = 2, started = 0;
	for (int i = 60; i >= 0; i -= 4) {
		int d = (v >> i) & 0xf;
		if (d || started || i == 0) {
			b[n++] = digits[d];
			started = 1;
		}
	}
	b[n] = 0;
	out(b);
}

static void kv(const char *k, uint64_t v)
{
	out(k);
	out("=");
	hex(v);
	out(" ");
}

/* Console + ramoops logging for shim/starqltechn/dsi.c: the same stream, so a
 * "STARQ DSI:" line lands on the panel and in both ramoops zones like every
 * other line of this loader. */
void starq_out(const char *s)
{
	out(s);
}

void starq_hex(uint64_t v)
{
	hex(v);
}

void starq_kv(const char *k, uint64_t v)
{
	kv(k, v);
}

/* ---- payload (tools/mkpayload.py v1), layouts in loader.h ---- */
_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args size");

/* Cache maintenance and barriers mean nothing on the host, where host_test.c
 * links this file to exercise the validation/copy path: with no cross compiler
 * on a desktop, that is the only way to run this code at all. */
#if defined(__aarch64__)
#define cpu_barrier()	__asm__ volatile("dsb sy" ::: "memory")
#else
#define cpu_barrier()	do { } while (0)
#endif

/* MMU off: everything is Device memory, keep accesses naturally aligned. */
static void copy(uintptr_t dst, uintptr_t src, uint64_t n)
{
	if (!((dst | src) & 7)) {
		for (; n >= 8; n -= 8, dst += 8, src += 8)
			*(volatile uint64_t *)dst = *(volatile uint64_t *)src;
	}
	for (; n; n--, dst++, src++)
		*(volatile uint8_t *)dst = *(volatile uint8_t *)src;
}

static void zero(uintptr_t dst, uint64_t n)
{
	for (; n && (dst & 7); n--, dst++)
		*(volatile uint8_t *)dst = 0;
	for (; n >= 8; n -= 8, dst += 8)
		*(volatile uint64_t *)dst = 0;
	for (; n; n--, dst++)
		*(volatile uint8_t *)dst = 0;
}

static void clean(uintptr_t start, uintptr_t end)
{
#if defined(__aarch64__)
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
	for (uintptr_t p = start & ~(line - 1); p < end; p += line)
		__asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)start;
	(void)end;
#endif
}

static int magic_ok(const volatile char *m)
{
	const char *want = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static uint64_t fail(const char *why)
{
	fg = 0xffff4040U;
	out("\nFAIL: ");
	out(why);
	out("\nHold POWER+VOL DOWN, then POWER+BIXBY+VOL UP -> TWRP\n");
	return 0;
}

/* Label + two values on one line, so 31 rows of console hold a full log. */
static void kp(const char *k, uint64_t a, const char *k2, uint64_t b)
{
	kv(k, a);
	kv(k2, b);
	out("\n");
}

/* ---- boot-arg overrides ---- */

/* Value after "needle" in the payload cmdline, or 0. needle ends with '=' and
 * must start a token (start of the line or after a space), so "nostarqfb=0"
 * does not match "starqfb=0". */
static const char *arg_val(const volatile char *cmd, const char *needle)
{
	for (int i = 0; i < BOOT_LINE_LENGTH && cmd[i]; i++) {
		int j = 0;
		if (i && cmd[i - 1] != ' ')
			continue;
		while (needle[j] && i + j < BOOT_LINE_LENGTH && cmd[i + j] == needle[j])
			j++;
		if (!needle[j])
			return (const char *)cmd + i + j;
	}
	return 0;
}

/* Copy the value of the switch "needle" into buf (bounded, NUL-terminated) and
 * return 1, or return 0 when the switch is absent. arg_val() hands back the
 * rest of the line, so "starqelvss=off starqdsi=0" would otherwise be one
 * value - and boot-args are documented to be combined freely. */
static int arg_tok(const volatile char *cmd, const char *needle, char *buf,
		   unsigned len)
{
	const char *v = arg_val(cmd, needle);
	unsigned n = 0;

	if (!v || !len)
		return 0;
	while (v[n] && v[n] != ' ' && n + 1 < len) {
		buf[n] = v[n];
		n++;
	}
	buf[n] = 0;
	return 1;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == 0 && *b == 0;
}

/* The whole value is one token: "starqdsisw=10" must not match "1". */
static int arg_is(const volatile char *cmd, const char *needle, const char *want)
{
	char b[16];

	return arg_tok(cmd, needle, b, sizeof(b)) && streq(b, want);
}

static int parse_hex(const char *s, uint64_t *val)
{
	uint64_t v = 0;
	int n = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (; *s; s++, n++) {
		int d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;
		if (n >= 16)
			return 0;
		v = v << 4 | (uint64_t)d;
	}
	if (!n)
		return 0;
	*val = v;
	return 1;
}

/* "starqwin=BASE:SIZE" - the only reason to override is to try a different
 * RAM window without rebuilding; the payload still decides where the copies go,
 * so a mismatch only shows up as a validation failure, never as silent damage. */
static int win_override(const volatile char *cmd, uint64_t *base, uint64_t *size)
{
	const char *v = arg_val(cmd, "starqwin=");
	if (!v || !parse_hex(v, base))
		return 0;
	while (*v && *v != ':')
		v++;
	if (*v != ':' || !parse_hex(v + 1, size))
		return 0;
	return *size != 0;
}

static int windows_overlap(uintptr_t a, uint64_t alen, uint64_t base, uint64_t size)
{
	return size && alen && a < base + size && base < a + alen;
}

uint64_t starq_boot_args;	/* read by start.S */

/* load/len: where ABL actually put this image and how big it is, computed
 * PC-relatively by start.S. The image is linked at 0 and runs wherever it was
 * loaded, so the link-time address of _head (0) cannot be used to tell whether
 * the loader overlaps the RAM window XNU is about to own. */
uint64_t dl_main(uintptr_t dtb, uint64_t el, uint64_t sctlr,
		 uintptr_t load, uint64_t len)
{
	prz->sig = PRZ_SIG;
	prz->start = prz->size = 0;
	prz2->sig = PRZ_SIG;
	prz2->start = prz2->size = 0;
	log_len = log2_len = 0;
	fill(0, 0, FB_W, FB_H, 0xff000000U);
	fg = 0xffffff00U;
	out("S9LOADER BEGIN  (starqltechn ABL XNU loader)\n");
	fg = 0xffffffffU;
	kv("EL", el);
	kv("SCTLR", sctlr);
	kv("DTB", dtb);
	out("\n");

	/* Linux Image header values, for the log. */
	kv("image", load);
	kv("size", len);
	out("\n");
	if (el != 1)
		return fail("not entered at EL1");

	/* Framebuffer test pattern before anything else that can fail: it must be
	 * visible on the panel even when the payload is broken, which is the
	 * likelier first failure. starqfb=0 lives in the payload, so it can only
	 * be honoured now - paint, read the boot-args, undo if asked. */
	int pattern = STARQ_FB_TEST;
	if (pattern)
		fb_pattern();

	/*
	 * Panel probe, straight after the pattern and before the payload is even
	 * validated: it costs one string in the DTB, touches no register, and it
	 * is the answer to "which of the two S9 panels is in this unit" for
	 * every later step and for the log if the boot fails from here on.
	 *
	 * The identity comes from the DTB's /chosen/bootargs - ABL's own merged
	 * command line, not our payload cmdline - because that is where the
	 * vendor display driver reads msm_drm.dsi_display0=<panel node>:config0
	 * (dsi_display.c:7061-7064). The payload cmdline says nothing about the
	 * panel.
	 *
	 * It is kept unconditional even though starqelvss=off / starqdsi=0 turn
	 * the DSI path off, for two reasons: it is pure string parsing with no
	 * MMIO, and it cannot see those switches yet - they live in the payload
	 * cmdline, which is only located below, after the initrd is validated.
	 * So the lines are labelled "STARQ panel:" rather than "STARQ DSI:": no
	 * output of this loader ever claims DSI register access that the switch
	 * has forbidden, and the panel identity stays in the log when the DSI
	 * path is off.
	 */
	char bootargs[STARQ_BOOTARGS_MAX];
	char panel_token[STARQ_PANEL_TOKEN_MAX];
	int panel;

	if (fdt_chosen_str(dtb, "bootargs", bootargs, sizeof(bootargs))) {
		out("STARQ panel: bootloader cmdline: ");
		out(bootargs);
		out("\n");
	} else {
		out("STARQ panel: no /chosen/bootargs in the DTB\n");
		bootargs[0] = 0;
	}
	panel = starq_dsi_panel_from_cmdline(bootargs, panel_token,
					    sizeof(panel_token));
	out("STARQ panel: panel=");
	out(panel == STARQ_PANEL_PX01 ? "PX01" :
	    panel == STARQ_PANEL_MQ01 ? "MQ01" : "UNKNOWN");
	if (panel_token[0]) {
		out(" name=");
		out(panel_token);
	}
	out("\n");

	/* ABL puts the BOOT ramdisk in the low 4 GB bank; only 0x80000000..4G is
	 * DRAM on this board. */
	uint64_t rd_start, rd_end;
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return fail("no initrd in /chosen");
	kp("initrd", rd_start, "end", rd_end);
	if (rd_start < LOW_RAM_BASE || rd_end > LOW_RAM_END || rd_end <= rd_start)
		return fail("initrd outside the low 4G bank");

	const volatile struct payload *p = (const volatile struct payload *)rd_start;
	uint64_t length = rd_end - rd_start;
	if (length < sizeof(struct payload) || !magic_ok(p->magic) || p->version != 1)
		return fail("initrd is not a SWEETXNU v1 payload");

	if (arg_val(p->cmdline, "starqfb=0")) {
		pattern = 0;
		fb_pattern_clear();
	} else if (arg_val(p->cmdline, "starqfb=1")) {
		pattern = 1;
	}
	out(pattern ? "fb pattern ON: bands 0-255, block 1973+\n"
		    : "fb pattern OFF (starqfb=0)\n");

	uint64_t nsegs = p->nsegs;
	kp("segs", nsegs, "virt", p->virt_base);
	kp("entry", p->entry_va, "adt_size", p->adt_size);
	kp("phys", p->phys_base, "mem", p->mem_size);
	if (!nsegs || nsegs > 32 ||
	    __builtin_offsetof(struct payload, segs) + nsegs * sizeof(struct payload_seg) > length ||
	    p->adt_off > length || p->adt_size > length - p->adt_off ||
	    p->adt_size < 32)
		return fail("bad payload header");

	/* The RAM window: what the ADT/mkpayload asked for, checked against what
	 * this board may hand out. */
	uint64_t win_base = XNU_RAM_BASE, win_size = XNU_RAM_SIZE;
	if (win_override(p->cmdline, &win_base, &win_size))
		kp("win override", win_base, "size", win_size);
	if (!p->mem_size ||
	    p->phys_base < win_base || p->phys_base >= win_base + win_size ||
	    p->mem_size > win_base + win_size - p->phys_base ||
	    p->entry_va < p->virt_base || p->kernel_end < p->virt_base)
		return fail("payload RAM outside the starqltechn window");

	uintptr_t mem_end = p->phys_base + p->mem_size;
	uintptr_t kernel_end = p->phys_base + (p->kernel_end - p->virt_base);
	if (kernel_end >= mem_end)
		return fail("kernel larger than the RAM window");

	/* Nothing we are about to zero may be something we still need. */
	if (windows_overlap(rd_start, length, p->phys_base, p->mem_size))
		return fail("payload sits inside the XNU RAM window");
	if (windows_overlap(load, len, p->phys_base, p->mem_size))
		return fail("loader image inside the XNU RAM window");
	if (windows_overlap(FB_BASE, FB_BYTES, p->phys_base, p->mem_size))
		return fail("framebuffer inside the XNU RAM window");
	if (windows_overlap(RAMOOPS_BASE, RAMOOPS_SIZE, p->phys_base, p->mem_size))
		return fail("ramoops inside the XNU RAM window");
	if (dtb >= p->phys_base && dtb < mem_end)
		return fail("DTB inside the XNU RAM window");

	/* ABL hands XNU the previous OS's RAM (TWRP's Linux after `adb
	 * reboot`, partly decayed after a forced reboot); XNU 6 hung in
	 * vm_mem_bootstrap on the S8 until this window was zeroed. */
	out("zeroing XNU RAM...\n");
	zero(p->phys_base, p->mem_size);
	out("zeroed\n");

	for (uint64_t i = 0; i < nsegs; i++) {
		const volatile struct payload_seg *s = &p->segs[i];
		if (s->vmaddr < p->virt_base || s->file_size > s->vmsize ||
		    s->file_off > length || s->file_size > length - s->file_off ||
		    s->vmsize > p->mem_size ||
		    s->vmaddr - p->virt_base > p->mem_size - s->vmsize)
			return fail("bad segment");
		uintptr_t dst = p->phys_base + (s->vmaddr - p->virt_base);
		copy(dst, rd_start + s->file_off, s->file_size);
		zero(dst + s->file_size, s->vmsize - s->file_size);
		clean(dst, dst + s->vmsize);
	}
	kp("copied", nsegs, "kernel_end", kernel_end);
	out("\n");

	uintptr_t adt_pa = kernel_end;
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (ba_pa + PAGE_SIZE > mem_end)
		return fail("no room for ADT/boot_args");
	copy(adt_pa, rd_start + p->adt_off, p->adt_size);
	zero(ba_pa, PAGE_SIZE);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = ba_pa + PAGE_SIZE;
	/* xnu reads the ADT through its own mapping: a virtual address. */
	ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->memSizeActual = p->mem_size;
	clean(adt_pa, ba_pa + PAGE_SIZE);
	kp("adt", adt_pa, "boot_args", ba_pa);
	out("\n");

	uintptr_t entry_pa = p->phys_base + (p->entry_va - p->virt_base);
	if (entry_pa >= kernel_end)
		return fail("entry outside kernel");
	kv("entry_pa", entry_pa);
	out("\n");

	out("cmdline: ");
	for (int i = 0; i < BOOT_LINE_LENGTH && p->cmdline[i]; i++) {
		char c[2] = { p->cmdline[i], 0 };
		out(c);
	}
	out("\n");

	/* Step: the DSI mode probe, then the ELVSS override. ABL leaves the
	 * DSI link up, so the panel's own registers can be written without an
	 * MDSS driver; see dsi.c for the register sequence and what is
	 * deliberately left alone.
	 *
	 * Both kill switches are pure string checks and are made *before* the
	 * first register access: starqelvss=off, and starqdsi=0 (the same name
	 * and meaning as the kernel-side switch). Either one means "no DSI
	 * register access at all", not merely "do not send" - the mode probe
	 * itself reads DSI_CTRL, and a read of an *unclocked* MDSS block can
	 * hang the interconnect on Qualcomm, which would cost an Odin flash.
	 * That is the whole reason these switches exist, so when one is set
	 * starq_dsi_init() is never called: no probe, no read, no write.
	 *
	 * Every step here logs and continues: a display experiment must never
	 * stop XNU from starting. */
	{
		char elvss_buf[BOOT_LINE_LENGTH], dsi_buf[16], mps_buf[16];
		const char *elvss = arg_tok(p->cmdline, "starqelvss=", elvss_buf,
					    sizeof(elvss_buf)) ? elvss_buf : 0;
		const char *mps_arg = arg_tok(p->cmdline, "starqmps=", mps_buf,
					      sizeof(mps_buf)) ? mps_buf : 0;
		const char *spec = elvss ? elvss : STARQ_ELVSS_DEFAULT;
		unsigned mps = STARQ_ELVSS_MPS_DEFAULT;
		int elvss_off = starq_elvss_off(spec);
		int dsi_off = 0, dsi_bad = 0;
		uint64_t v;

		/* starqdsi=0 is the kernel's spelling: a value of zero disables,
		 * a non-zero value leaves the default (on) alone. */
		if (arg_tok(p->cmdline, "starqdsi=", dsi_buf, sizeof(dsi_buf))) {
			if (!parse_hex(dsi_buf, &v))
				dsi_bad = 1;
			else if (v == 0)
				dsi_off = 1;
		}

		if (elvss_off || dsi_off) {
			out("STARQ DSI: ");
			if (elvss_off)
				out("starqelvss=off");
			if (elvss_off && dsi_off)
				out(" and ");
			if (dsi_off)
				out("starqdsi=0");
			out(", no DSI register access at all\n");
		} else {
			/* Only now may a DSI register be touched. */
			if (dsi_bad)
				out("STARQ DSI: bad starqdsi, DSI left on\n");
			if (mps_arg && parse_hex(mps_arg, &v) && v <= 0xff)
				mps = (unsigned)v;
			else if (mps_arg)
				out("STARQ DSI: bad starqmps, using 0xdc\n");

			if (starq_dsi_init(STARQ_DSI_CTRL_PA, STARQ_DSI_PHY_PA) != 0)
				out("STARQ DSI: controller not probed\n");

			if (arg_is(p->cmdline, "starqdsisw=", "1"))
				starq_dsi_set_sw_trigger(1);
			/* The boot-arg wins; otherwise spec is the compiled
			 * default, which is empty by default so the policy in
			 * starq_dsi_elvss() picks the per-panel value. */
			if (starq_dsi_elvss(spec, panel, mps) < 0)
				out("STARQ DSI: override not applied\n");
		}
	}
	out("\n");

	starq_boot_args = ba_pa;
	fg = 0xff40ff40U;
	out("JUMP TO XNU\n");
	/* Both zones are written with the MMU off (Device memory), so push them to
	 * the PoC before XNU turns its caches on: nothing else will. */
	clean(RAMOOPS_CONSOLE, RAMOOPS_CONSOLE + RAMOOPS_ZONE_SIZE);
	clean(RAMOOPS_PMSG, RAMOOPS_PMSG + RAMOOPS_ZONE_SIZE);
	return entry_pa;
}

/* Halt this CPU forever. On the host (host_test.c) there is nothing to halt,
 * so it returns and the test can check that the failure was logged. */
static void halt(void)
{
#if defined(__aarch64__)
	for (;;)
		__asm__ volatile("wfe");
#endif
}

void dl_exception(uint64_t slot)
{
	uint64_t esr = 0, elr = 0, far = 0;
#if defined(__aarch64__)
	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1" : "=r"(far));
#endif
	fg = 0xffff4040U;
	out("\nLOADER EXCEPTION ");
	kv("slot", slot);
	out("\n");
	kp("ESR", esr, "FAR", far);
	kv("ELR", elr);
	out("\n");
	halt();
}

/* gcc may emit calls for struct copies; keep the image self-contained. */
void *memset(void *d, int c, unsigned long n)
{
	unsigned char *q = d;
	while (n--)
		*q++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	unsigned char *q = d;
	const unsigned char *r = s;
	while (n--)
		*q++ = *r++;
	return d;
}
