/*
 * starqltechn loader: linked at 0, runs wherever S-Boot placed the kernel
 * section. Same layout as the S8 port's loader.ld, minus the secondary-stack
 * area (this loader starts no other cores).
 */
ENTRY(_head)
SECTIONS
{
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x8000;
	. = ALIGN(16);
	_stack_top = .;
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }

	/* start.S reaches _stack_top, _bss_end and _end with adr, which has a
	 * +/-1MB range. The image is ~40KB; this makes a future growth past the
	 * range a link error instead of a silently truncated displacement. */
	ASSERT(_end <= 0x100000, "image outgrew the adr range in start.S")
}
