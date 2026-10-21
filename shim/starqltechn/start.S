/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy S9 (starqltechn, Qualcomm SDM845) S-Boot XNU loader entry.
 *
 * S-Boot boots the kernel section of a Samsung boot.img the way a Linux
 * bootloader does, not the way ABL boots its own image, but it still expects
 * the arm64 Linux Image header at the start of the kernel: it reads
 * text_offset/image_size to know what it loaded (the S8 port shipped the
 * header for the same reason). S-Boot enters at EL1 with the MMU and caches
 * off, CNTFRQ_EL0 possibly 0, and x0 = DTB.
 *
 * Position independent: nothing here refers to a link-time address except
 * through PC-relative arithmetic (adr), so the image may be linked at 0 and
 * run wherever S-Boot put it. shim/starqltechn/build.sh fails the build if an
 * ABS64 relocation sneaks in.
 *
 * text_offset is 0x80000 as in the stock Samsung kernel and the S8 port; that
 * port boots with it, i.e. S-Boot enters at the start of the kernel section it
 * loaded and does not add text_offset to the entry address. (If that ever
 * turns out wrong the phone shows no text at all: the first thing to try is
 * text_offset 0.)
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset, as the stock Samsung kernel */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB */
	mrs	x20, CurrentEL
	mrs	x21, sctlr_el1
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	lsr	x1, x20, #2		/* CurrentEL -> 1 at EL1 */
	mov	x2, x21
	/* x3 = where S-Boot actually put us, x4 = image length: both are only
	 * known at run time (the image is linked at 0 and is position
	 * independent), so the loader can tell whether zeroing the RAM window
	 * would erase itself. */
	adr	x3, _head
	adr	x4, _end
	sub	x4, x4, x3
	bl	dl_main			/* returns the XNU entry PA, or 0 */
	cbz	x0, 4f
	mov	x16, x0
	adr	x1, starq_boot_args
	ldr	x0, [x1]		/* boot_args PA in x0, as XNU expects */
	/* XNU's entry expects the MMU and caches off. Everything was written
	 * with them off (Device memory) and the copies were cleaned to PoC. */
	mrs	x14, sctlr_el1
	bic	x14, x14, #1		/* M */
	bic	x14, x14, #4		/* C */
	bic	x14, x14, #0x1000	/* I */
	msr	sctlr_el1, x14
	isb				/* the SCTLR write must be visible ... */
	ic	iallu			/* ... before the I-cache is invalidated */
	dsb	sy
	isb
	br	x16
4:	wfe
	b	4b

/*
 * Exception vectors. Everything except an IRQ ends in dl_exception, which
 * paints the failure on the console and the ramoops zones: a loader fault
 * must be readable after the forced reboot, not just visible.
 */
.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	mov	x1, sp
	b	exc_common
.endm

	.balign	0x800
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	bl	dl_exception		/* never returns */
5:	wfe
	b	5b
