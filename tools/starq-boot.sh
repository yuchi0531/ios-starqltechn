#!/bin/bash
# XNU kernel Mach-O -> Galaxy S9 (starqltechn, SM-G9600) BOOT image.
#
#   tools/starq-boot.sh KERNEL STOCK_BOOT.img OUT.img [boot-args...]
#
# Pipeline: mkadt.py -> mkpayload.py -> shim/build.sh (BOARD=starqltechn,
# compiles the shim and calls tools/starq_mkboot.py) -> header check.
#
# STOCK_BOOT.img is the phone's own boot image (data/boot.emmc.win.img or
# backup/starq/boot.img, see tools/starq-flash.sh). It supplies the header
# ABL expects (addresses, page size, os_version/patch level, name, cmdline) and
# the DTBs. The packer probes its shape, so either stock shape works:
#   - a dt section holding a Qualcomm DTBH table (older stock/port images), or
#   - Image.gz-dtb: dt_size 0, the DTBs appended after a gzip stream (what the
#     starqltechn stock and ROM images are). Then our Image is gzipped, the
#     stock DTB set is appended right after the gzip member, and dt_size stays 0.
#
# The gzip step is not optional on this board: with header v0 ABL takes the DTB
# offset from the end of the gzip member and never reads dt_size, so a raw
# Image leaves it with DtbOffset 0, no tree, and no jump to the kernel (the
# phone just lands in recovery with a blank screen -- seen on hardware).
#
# There is no fastboot on this phone: the result is written to /dev/block/
# by-name/boot from TWRP with tools/starq-flash.sh.
set -euo pipefail

usage() {
	# The header comment block, minus the shebang.
	awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
	echo
	echo "Defaults for the boot-args:"
	echo "  $DEFAULT_ARGS"
}

DEFAULT_ARGS="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=0 sweetfb_hz=30"

if [ $# -lt 3 ]; then
	usage >&2
	exit 2
fi
case "$1" in -h|--help) usage; exit 0 ;; esac

repo=$(cd "$(dirname "$0")/.." && pwd)
kernel=$(realpath "$1")
stock=$(realpath "$2")
out=$(realpath -m "$3")
shift 3
# Console ON by default: this board has no serial port, so text on the panel is
# the only way to see how far boot got.
args=${*:-$DEFAULT_ARGS}
# These args go into the payload cmdline (tools/mkpayload.py), NOT into the
# boot.img header. The header cmdline stays the stock one, because ABL merges it
# into /chosen/bootargs and the loader reads msm_drm.dsi_display0 from there to
# pick the panel (AMB577PX01 vs AMB577MQ01) and the ELVSS byte. Override it with
# tools/starq_mkboot.py --cmdline/--empty-cmdline if that ever has to change.

[ -f "$kernel" ] || { echo "error: kernel not found: $kernel" >&2; exit 1; }
[ -f "$stock" ] || { echo "error: stock boot image not found: $stock" >&2; exit 1; }

# The RAM window is Samsung's largest free gap in the 4 GB low bank
# (0xa1c01000..0xba000000 per the vendor reserved-memory map); 0xa2000000 is
# where the postmarketOS initramfs is loaded on this exact device. Passed
# explicitly so mkadt.py does not print its PROVISIONAL warning.
RAM_BASE=0xa2000000
RAM_SIZE=0x18000000

# Fail on the toolchain before doing any work: the cross compiler is the one
# that is missing on a plain desktop, and its absence used to surface as a
# "command not found" halfway through the shim build.
missing=()
for t in python3 aarch64-linux-gnu-gcc aarch64-linux-gnu-objcopy \
	 aarch64-linux-gnu-readelf aarch64-linux-gnu-nm; do
	command -v "$t" >/dev/null 2>&1 || missing+=("$t")
done
if [ ${#missing[@]} -gt 0 ]; then
	echo "error: missing build tools: ${missing[*]}" >&2
	echo "       install the aarch64 cross toolchain, e.g. on Debian/Ubuntu:" >&2
	echo "         sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu" >&2
	echo "       (the Samsung path does not use mkbootimg: tools/starq_mkboot.py" >&2
	echo "        packs the image from the stock header instead)" >&2
	exit 1
fi

work=$repo/shim/out
mkdir -p "$work" "$(dirname "$out")"

echo "== ADT (board starqltechn, RAM $RAM_BASE+$RAM_SIZE)"
python3 "$repo/tools/mkadt.py" "$work/starq.adt" --board starqltechn \
	--ram-base "$RAM_BASE" --ram-size "$RAM_SIZE"

echo "== payload"
python3 "$repo/tools/mkpayload.py" "$kernel" "$work/starq.adt" "$work/payload.bin" \
	--cmdline "$args"

echo "== shim"
# PACK=0: compile only. The packer runs below so OUT.img is exactly the path
# the caller asked for, and no mkbootimg is involved.
BOARD=starqltechn PACK=0 bash "$repo/shim/build.sh" "$work/payload.bin"

echo "== boot.img (stock header + DTBs from $stock)"
# The packer probes the stock image and takes the right path itself:
#   dt_size > 0        -> dt section is a DTBH table, carried verbatim; the
#                         kernel stays raw (ABL finds the DTBs via dt_size)
#   dt_size == 0       -> kernel is Image.gz-dtb, so the packer gzips our Image
#                         and appends the DTBs right after the gzip member,
#                         keeping dt_size 0. Nothing extra is needed here.
# DTB_FILE=... substitutes another table/set: a DTBH table for the first shape,
# a file or directory of bare FDTs for the second (the packer picks --dtb or
# --dtb-set and rejects the wrong one).
shape=$(python3 "$repo/tools/starq_mkboot.py" --shape "$stock")
case "$shape" in
dtbh)     echo "   stock shape: DTBH dt section" ;;
appended) echo "   stock shape: Image.gz-dtb (dt_size 0, appended FDTs); the packer" \
               "gzips our Image and appends the stock DTB set, dt_size stays 0" ;;
*)        echo "error: $stock is a boot.img v0 with neither a DTBH dt section nor DTBs" >&2
          echo "       appended to its kernel; the bootloader would have no tree to" >&2
          echo "       boot with. Point STOCK_BOOT at the phone's own boot.img (see" >&2
          echo "       NOTES.md)." >&2
          exit 1 ;;
esac
if [ -n "${DTB_FILE:-}" ]; then
	# Same flag for both shapes, different meaning: a DTBH table vs an FDT set.
	flag=--dtb
	[ "$shape" = appended ] && flag=--dtb-set
	echo "   substituting $DTB_FILE ($flag)"
	python3 "$repo/tools/starq_mkboot.py" "$stock" "$work/Image" \
		"$work/payload.bin" "$out" "$flag" "$DTB_FILE"
else
	python3 "$repo/tools/starq_mkboot.py" "$stock" "$work/Image" \
		"$work/payload.bin" "$out"
fi

echo "== header"
python3 "$repo/tools/starq_mkboot.py" --info "$out"
echo "flash it from TWRP: tools/starq-flash.sh boot $out"
