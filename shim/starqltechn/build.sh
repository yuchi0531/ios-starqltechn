#!/bin/bash
# Build the Galaxy S9 (starqltechn, SM-G9600) S-Boot XNU loader: the "kernel"
# section of a Samsung boot.img v0.
#
#   shim/starqltechn/build.sh                              compile only
#   shim/starqltechn/build.sh STOCK_BOOT.img PAYLOAD.bin OUT.img [DTBH.bin]
#
# STOCK_BOOT.img is the phone's own boot image (backup/starq/boot.img): it
# supplies the header S-Boot/AVB expect and the Qualcomm DTBH dt section.
# PAYLOAD.bin is tools/mkpayload.py's SWEETXNU v1 container. The packer is
# tools/starq_mkboot.py, so no mkbootimg is needed (Samsung's header is not an
# mkbootimg layout).
#
# Environment:
#   OUT_DIR=dir   object/output directory (default <here>/out). shim/build.sh
#                 BOARD=starqltechn passes ../out so that tools/starq-boot.sh
#                 still finds shim/out/Image.
#   CFLAGS_EXTRA= extra compiler flags, e.g. -DSTARQ_FB_TEST=0.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "${OUT_DIR:-$here/out}")
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
# Same flag set as the S8 port (ios-galaxy-s8 shim/dreamlte/build.sh):
# -march=armv8-a keeps the image runnable on whichever core S-Boot used,
# -fno-tree-loop-distribute-patterns stops gcc turning the copy/zero loops into
# calls that expect a libc, and -Werror is on because a warning in a
# freestanding loader is a bug.
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align \
-fno-stack-protector -fno-tree-loop-distribute-patterns -march=armv8-a -Wall -Wextra -Werror"
# Board constants (framebuffer, RAM window, ramoops, test pattern) are the
# defaults in loader.c; a caller may override them, e.g.
#   CFLAGS_EXTRA="-DSTARQ_FB_TEST=0"
CFLAGS="$CFLAGS ${CFLAGS_EXTRA:-}"

mkdir -p "$OUT"
objs=""
for f in start.S loader.c dsi.c ../fdt.c; do
	o="$OUT/starq-$(basename "${f%.*}").o"
	$CC $CFLAGS -c "$here/$f" -o "$o"
	objs="$objs $o"
done

# The image runs wherever S-Boot placed it, with no relocation pass. The Linux
# Image header at the start of .text.head is the one place a link-time address
# may appear (image_size = _end, linked at 0), exactly as in the sweet shim; a
# relocation anywhere else means the image would only work at its link address.
abs=$($READELF -r $objs | awk '
	/^Relocation section/ { sec = $3; gsub(/["'"'"']/, "", sec) }
	/ABS64/ && sec !~ /\.text\.head$/ { print }')
[ -z "$abs" ] || { echo "error: absolute relocation(s) outside the Image header:" >&2
	echo "$abs" >&2; exit 1; }

$CC $CFLAGS -static -T "$here/loader.ld" -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$objs -o "$OUT/starq-loader.elf"
$OBJCOPY -O binary "$OUT/starq-loader.elf" "$OUT/Image"
# Pad to _end so .bss and the stack exist as zeroed bytes in the file: nothing
# else writes them, and S-Boot copies only image_size bytes.
size=$($NM "$OUT/starq-loader.elf" | awk '$3=="_end"{print strtonum("0x"$1)}')
[ -n "$size" ] || { echo "error: no _end symbol in the loader elf" >&2; exit 1; }
truncate -s "$size" "$OUT/Image"
echo "$OUT/Image: $size bytes"

if [ $# -eq 3 ]; then
	python3 "$here/../../tools/starq_mkboot.py" "$1" "$OUT/Image" "$2" "$3"
	sha256sum "$3"
elif [ $# -eq 4 ]; then
	# Substitute the stock DTBs: a DTBH table when the stock image has a dt
	# section, an FDT set when it is Image.gz-dtb. The packer probes the shape
	# and rejects the wrong flag with an explanation.
	if [ "$(python3 "$here/../../tools/starq_mkboot.py" --shape "$1")" = appended ]; then
		flag=--dtb-set
	else
		flag=--dtb
	fi
	python3 "$here/../../tools/starq_mkboot.py" "$1" "$OUT/Image" "$2" "$3" \
		"$flag" "$4"
	sha256sum "$3"
elif [ $# -ne 0 ]; then
	echo "usage: $0 [STOCK_BOOT.img PAYLOAD.bin OUT.img [DTBH.bin|FDT_DIR]]" >&2
	exit 2
fi
