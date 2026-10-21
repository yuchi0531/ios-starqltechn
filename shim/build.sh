#!/bin/bash
# Build the shim and wrap it in a boot image.
#   BOARD=sweet (default)  -> boot.img v2 with the stock Xiaomi header
#   BOARD=starqltechn      -> Samsung S9 SM-G9600 boot.img v0 that reuses the
#                             stock Samsung header and DTBH dt section
# FB_* defaults are sweet's; a board overrides them through CFLAGS.
#
# Knobs for an external pipeline (tools/starq-boot.sh) on starqltechn:
#   STOCK_BOOT=STOCK.img  pack with tools/starq_mkboot.py (header + dt source)
#   IMG=OUT.img           where the packed image goes
#   PACK=0                compile only; the caller packs
#   DTB_FILE=DTBH.bin     use this DTBH table instead of the stock dt section
set -euo pipefail
cd "$(dirname "$0")"
BOARD=${BOARD:-sweet}
# Stock DTB the sweet boot image carries (tools/unpack_bootv2.py -> dtb/).
# starqltechn gets its DTB from the stock boot.img's DTBH section instead.
DTB=${DTB:-../dtb/boot_unpacked/dtb}
# Stock Samsung boot image: header and DTBH dt section for BOARD=starqltechn.
STOCK_BOOT=${STOCK_BOOT:-}
PACK=${PACK:-1}
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
BOARD_CFLAGS=""
case "$BOARD" in
sweet) IMG=${IMG:-out/shim-boot.img} ;;
starqltechn)
	# Splash framebuffer from the mainline dts: chosen/framebuffer with
	# reserved-memory framebuffer_mem: memory@9d400000, 1440x2960 a8r8g8b8.
	BOARD_CFLAGS="-DFB_BASE=0x9d400000 -DFB_W=1440 -DFB_H=2960"
	# Samsung boot.img v0: no mkbootimg, the packer reuses the stock header.
	IMG=${IMG:-out/shim-boot-starqltechn.img}
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
if [ "$PACK" = 0 ]; then
	# Compile only; the caller packs (tools/starq-boot.sh runs starq_mkboot.py
	# itself, which is what lets OUT.img be an arbitrary path).
	echo "PACK=0: compiled out/Image and out/ramdisk.bin, no boot image written" >&2
elif [ "$BOARD" = sweet ]; then
	mkbootimg --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
		--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
		--os_version 17.0.0 --os_patch_level 2035-06 \
		--cmdline "console=null" -o out/shim-boot.img
elif [ -z "$STOCK_BOOT" ]; then
	# Samsung header v0 with a Qualcomm DTBH dt section. There is no mkbootimg
	# layout for it that matches the phone: use the stock image as the template.
	echo "error: BOARD=starqltechn needs STOCK_BOOT=STOCK_BOOT.img" >&2
	echo "       (the phone's own boot image, e.g. backup/starq/boot.img;" >&2
	echo "        tools/starq-boot.sh passes it and tools/starq-flash.sh backup pulls it)" >&2
	exit 1
else
	# DTB_FILE: a whole DTBH table to carry instead of the stock one.
	if [ -n "${DTB_FILE:-}" ]; then
		python3 ../tools/starq_mkboot.py "$STOCK_BOOT" out/Image \
			out/ramdisk.bin "$IMG" --dtb "$DTB_FILE"
	else
		python3 ../tools/starq_mkboot.py "$STOCK_BOOT" out/Image \
			out/ramdisk.bin "$IMG"
	fi
fi
# Host render of the console layout.
gcc -O1 -Wall $BOARD_CFLAGS -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image
[ "$PACK" = 0 ] || ls -l "$IMG"
