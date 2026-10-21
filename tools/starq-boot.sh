#!/bin/bash
# XNU kernel Mach-O -> Galaxy S9 (starqltechn, SM-G9600) BOOT image.
#
#   tools/starq-boot.sh KERNEL STOCK_BOOT.img OUT.img [boot-args...]
#
# Pipeline: mkadt.py -> mkpayload.py -> shim/build.sh (BOARD=starqltechn,
# compiles the shim and calls tools/starq_mkboot.py) -> header check.
#
# STOCK_BOOT.img is the phone's own boot image (backup/starq/boot.img, see
# tools/starq-flash.sh). It supplies the header S-Boot expects (addresses,
# page size, os_version/patch level, name) and the DTBH dt section.
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

echo "== boot.img (stock header + DTBH from $stock)"
# DTB_FILE=table.dtbh substitutes a different DTBH table for the stock one.
# (No "${arr[@]}" on an empty array: that is an unbound variable in bash < 4.4
# under `set -u`.)
if [ -n "${DTB_FILE:-}" ]; then
	python3 "$repo/tools/starq_mkboot.py" "$stock" "$work/Image" \
		"$work/payload.bin" "$out" --dtb "$DTB_FILE"
else
	python3 "$repo/tools/starq_mkboot.py" "$stock" "$work/Image" \
		"$work/payload.bin" "$out"
fi

echo "== header"
python3 "$repo/tools/starq_mkboot.py" --info "$out"
echo "flash it from TWRP: tools/starq-flash.sh boot $out"
