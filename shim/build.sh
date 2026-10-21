#!/bin/bash
# Build the shim and wrap it in a boot image.
#   BOARD=sweet (default)  -> boot.img v2 with the stock Xiaomi header
#   BOARD=starqltechn      -> PROVISIONAL Samsung S9 SM-G9600 image (see below)
# FB_* defaults are sweet's; a board overrides them through CFLAGS.
set -euo pipefail
cd "$(dirname "$0")"
BOARD=${BOARD:-sweet}
# Stock DTB the boot image carries (tools/unpack_bootv2.py -> dtb/).
DTB=${DTB:-../dtb/boot_unpacked/dtb}
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
BOARD_CFLAGS=""
IMG=out/shim-boot.img
case "$BOARD" in
sweet) ;;
starqltechn)
	# Splash framebuffer from the mainline dts: chosen/framebuffer with
	# reserved-memory framebuffer_mem: memory@9d400000, 1440x2960 a8r8g8b8.
	BOARD_CFLAGS="-DFB_BASE=0x9d400000 -DFB_W=1440 -DFB_H=2960"
	IMG=out/shim-boot-starqltechn.img
	;;
*)
	echo "error: unknown BOARD=$BOARD (sweet|starqltechn)" >&2
	exit 1
	;;
esac
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS $BOARD_CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS $BOARD_CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
if [ "$BOARD" = sweet ]; then
	mkbootimg --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
		--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
		--os_version 17.0.0 --os_patch_level 2035-06 \
		--cmdline "console=null" -o out/shim-boot.img
else
	# PROVISIONAL Samsung S9 layout: header v0, no qcdt, with the offsets the
	# postmarketOS starqltechn port uses.
	# TODO: where SBOOT takes the DTB is still open - the dtbo partition
	# (Samsung qcdt) or the kernel blob. For now do what the postmarketOS u-boot
	# payload does (deviceinfo_bootimg_override_payload_append_dtb): append the
	# stock dtb to the kernel and pass no --dtb.
	cp out/Image out/Image-dtb
	if [ -f "$DTB" ]; then
		cat "$DTB" >> out/Image-dtb
	else
		echo "warning: $DTB missing, the image gets no DTB" >&2
	fi
	mkbootimg --header_version 0 --kernel out/Image-dtb --ramdisk out/ramdisk.bin \
		--base 0 --kernel_offset 0x00008000 --ramdisk_offset 0x02000000 \
		--tags_offset 0x01e00000 --second_offset 0x00f00000 --pagesize 4096 \
		--os_version 17.0.0 --os_patch_level 2035-06 \
		--cmdline "console=null" -o "$IMG"
fi
# Host render of the console layout.
gcc -O1 -Wall $BOARD_CFLAGS -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image "$IMG"
