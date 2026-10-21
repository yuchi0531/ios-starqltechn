#!/bin/bash
# Galaxy S9 (starqltechn) helpers, run on the PC with the phone in TWRP.
#
#   tools/starq-flash.sh boot IMG      write IMG to the boot partition
#   tools/starq-flash.sh boot-name     list /dev/block/by-name/ (find the nodes)
#   tools/starq-flash.sh backup        pull boot, recovery, efs, persist, modem
#   tools/starq-flash.sh pull-log OUT  read pstore after a crash
#
# There is no fastboot on this phone. Every kernel iteration is an adb push of
# a boot image into TWRP, then a dd of the boot partition; a bad boot.img is
# recovered by flashing a stock AP in Odin (see tools/README-starqltechn.md).
set -euo pipefail

ADB=${ADB:-adb}
repo=$(cd "$(dirname "$0")/.." && pwd)
usage() {
	# The header comment block, minus the shebang.
	awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

die() { echo "error: $*" >&2; exit 1; }

need_adb() {
	command -v "$ADB" >/dev/null 2>&1 || die "adb not found in PATH"
	local state
	state=$($ADB get-state 2>/dev/null || true)
	[ "$state" = device ] || die "no adb device (state '${state:-none}'); boot TWRP and check 'adb devices'"
}

# The boot node name is not guaranteed across firmware: Samsung TWRP exposes
# /dev/block/by-name/<lowercase> symlinks to raw sdaN nodes. Never assume the
# number.
by_name() {
	$ADB shell "for p in /dev/block/by-name/$1 /dev/block/bootdevice/by-name/$1 \
/dev/block/platform/soc/*/by-name/$1; do [ -e \$p ] && readlink -f \$p && exit 0; done; exit 1" \
		2>/dev/null | tr -d '\r'
}

cmd_boot_name() {
	need_adb
	echo "== /dev/block/by-name/ (this is the authoritative list for this phone)"
	$ADB shell ls -l /dev/block/by-name/ | tr -d '\r'
}

cmd_backup() {
	need_adb
	# Into the repo's gitignored backup/ dir, wherever the script was called from.
	local out="$repo/backup/starq"
	mkdir -p "$out"
	echo "== pulling partitions to $out/ (keep these off git; .gitignore has backup/)"
	for part in boot recovery efs persist modem; do
		local node
		if ! node=$(by_name "$part"); then
			echo "warning: $part not found under /dev/block/by-name, skipped" >&2
			continue
		fi
		# Read it straight off the block device: a filesystem copy of efs/persist
		# would lose the raw per-device calibration data we need.
		$ADB exec-out "dd if=$node bs=1048576 2>/dev/null" > "$out/$part.img"
		printf '  %-9s %-40s %s bytes\n' "$part" "$node" "$(stat -c %s "$out/$part.img")"
	done
	echo "== sha256"
	(cd "$out" && sha256sum ./*.img > SHA256SUMS && cat SHA256SUMS)
}

cmd_boot() {
	local img=${1:-}
	[ -n "$img" ] || die "usage: starq-flash.sh boot IMG [--yes]"
	shift || true
	local yes=0
	for a in "$@"; do
		case "$a" in
		--yes|-y) yes=1 ;;
		*) die "unknown option $a" ;;
		esac
	done
	[ -f "$img" ] || die "no such file: $img"
	need_adb
	local node
	node=$(by_name boot) || die "boot partition not found; run 'starq-flash.sh boot-name'"

	# Refuse anything that is not a boot image before touching the phone: this is
	# a dd into the one partition the phone boots from.
	head -c 8 "$img" | grep -q 'ANDROID!' || die "$img does not start with ANDROID!"
	python3 "$(dirname "$0")/starq_mkboot.py" --info "$img" || die "$img is not a usable boot image"

	echo
	echo "about to overwrite the boot partition"
	echo "  image: $img ($(stat -c %s "$img") bytes)"
	echo "  node:  $node"
	echo "This replaces the bootloader's only kernel. A bad image needs a stock AP"
	echo "flashed with Odin to recover. The stock boot.img must be in backup/starq/."
	if [ "$yes" != 1 ]; then
		answer=
		read -r -p "Type 'yes' to continue: " answer || true
		[ "$answer" = yes ] || { echo "aborted (type 'yes', or pass --yes)" >&2; exit 1; }
	fi

	$ADB push "$img" /tmp/starq-boot.img
	# bs=1M: the TWRP shell's dd is busybox, and a smaller bs is needlessly slow.
	$ADB shell "dd if=/tmp/starq-boot.img of=$node bs=1048576 conv=fsync; sync; rm -f /tmp/starq-boot.img" \
		|| die "dd failed; the boot partition may be partially written. Reflash backup/starq/boot.img or a stock AP with Odin."
	echo "== read back"
	$ADB shell "dd if=$node bs=1048576 count=4 2>/dev/null" | head -c 8 | grep -q 'ANDROID!' \
		&& echo "ok: $node starts with ANDROID!" \
		|| echo "warning: could not read the magic back from $node; verify with 'starq-flash.sh boot-name'" >&2
	echo "now: adb reboot   (recovery = Vol Up + Bixby + Power)"
}

cmd_pull_log() {
	local out=${1:-}
	[ -n "$out" ] || die "usage: starq-flash.sh pull-log OUT"
	need_adb
	# pstore survives the reboot TWRP did after the crash: the bootloader's
	# ramoops region is what the previous boot left behind. console-ramoops-0 is
	# the console (shim text + XNU kprintf), pmsg-ramoops-0 is the loader's own
	# messages (S-Boot does not write here, our shim does).
	local ts; ts=$(date +%Y%m%d-%H%M%S)
	mkdir -p "$(dirname "$out")"
	local base=${out%.txt}
	local got=0 f
	for f in console-ramoops-0 pmsg-ramoops-0; do
		$ADB exec-out "cat /sys/fs/pstore/$f 2>/dev/null" > "$base-$f-$ts.txt" || true
		if [ -s "$base-$f-$ts.txt" ]; then
			echo "  $base-$f-$ts.txt: $(stat -c %s "$base-$f-$ts.txt") bytes"
			got=1
		else
			rm -f "$base-$f-$ts.txt"
		fi
	done
	if [ "$got" = 0 ]; then
		echo "warning: both ramoops files are empty." >&2
		echo "  Either the phone did not crash into ramoops (a clean reboot clears it)," >&2
		echo "  or it was reset by another boot, or /sys/fs/pstore is not mounted." >&2
		echo "  Filmed screen text, and the kernel's persistent log ring in userdata," >&2
		echo "  are the fallbacks; TWRP cannot read pstore across a second reboot." >&2
		return 1
	fi
}

case "${1:-}" in
boot) shift; cmd_boot "$@" ;;
boot-name) cmd_boot_name ;;
backup) cmd_backup ;;
pull-log) shift; cmd_pull_log "$@" ;;
-h|--help|'') usage; exit 0 ;;
*) usage >&2; exit 2 ;;
esac
