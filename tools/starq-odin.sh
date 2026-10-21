#!/bin/bash
# Galaxy S9 (starqltechn) Odin4 helpers: the Linux-native flashing path.
#
#   tools/starq-odin.sh build BOOT_IMG [--md5] [--member-name NAME]
#                                      pack boot.img as an Odin archive
#   tools/starq-odin.sh check TAR      validate PIT + archive (writes nothing).
#                                      MANDATORY gate before flash.
#   tools/starq-odin.sh flash TAR [--member NAME] --yes
#                                      check, then write TAR to the boot
#                                      partition and reboot
#   tools/starq-odin.sh list           devices in Download Mode
#   tools/starq-odin.sh restore AP_TAR --yes
#                                      flash a stock AP archive (recovery)
#   tools/starq-odin.sh install-udev   USB permissions for 04e8 download mode
#
# This path needs no TWRP and no adb: odin4 speaks the Thor protocol (the same
# one Android's Odin uses) over libusb. But it is WRITE-ONLY. Reading anything
# back -- backup, pstore, /dev/block/by-name -- still needs TWRP; use
# tools/starq-flash.sh for that.
#
# A bad boot.img needs a stock AP (restore) or a TWRP restore of
# backup/starq/boot.img. Keep both before the first flash.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
tar_tool="$repo/tools/starq_odin_tar.py"
SUDO=${SUDO:-sudo}

usage() {
	# The header comment block, minus the shebang.
	awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

die() { echo "error: $*" >&2; exit 1; }

download_mode_hint() {
	cat <<-EOF

	Put the phone in Download Mode:
	  1. unplug USB
	  2. hold Vol Down + Bixby + Power, release at the warning
	  3. Vol Up to confirm
	  4. plug USB back in
	(Bixby is the separate button on the left edge, below the volume rocker.)
	EOF
}

# odin4 writes ./odin4.log in the directory it is run from; .gitignore has it.
#
# odin4 --check-only still needs the phone: it fetches the PIT from the device
# and matches the archive entries against that. Finding no device is the common
# case here, so `need_device` checks with -l first and prints the button combo
# instead of letting a wall of libusb errors through.
if [ -n "${ODIN4:-}" ]; then
	odin4=$ODIN4
elif command -v odin4 >/dev/null 2>&1; then
	odin4=$(command -v odin4)
elif [ -x "$repo/build/odin4" ]; then
	odin4="$repo/build/odin4"
elif [ -x /tmp/opencode/odin4/build/odin4 ]; then
	odin4=/tmp/opencode/odin4/build/odin4
else
	odin4=
fi

need_odin4() {
	[ -n "$odin4" ] && [ -x "$odin4" ] || die "odin4 not found.
  build it:  git clone https://github.com/Llucs/odin4 && cd odin4
             cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DODIN4_BUILD_GUI=OFF
             cmake --build build --parallel \$(nproc)
  (the GUI needs Qt6 and is not used here)
  then either export ODIN4=/path/to/odin4/build/odin4 or put it in PATH"
}

need_device() {
	local out
	out=$("$odin4" -l 2>&1) || die "odin4 -l failed: $out"
	if printf '%s\n' "$out" | grep -qi 'no .*devices detected'; then
		echo "error: no device in Download Mode (odin4 -l)." >&2
		download_mode_hint >&2
		exit 1
	fi
	printf '== device: %s\n' "$out"
}

# Every tar we hand to odin4 is parsed here first: a broken or truncated
# archive must not reach the write path.
need_archive() {
	local tar=$1 member=${2:-} members
	[ -f "$tar" ] || die "no such file: $tar"
	members=$(python3 "$tar_tool" --list "$tar" 2>&1) || die "$tar is not a valid tar archive"
	members=$(printf '%s\n' "$members" | awk 'NF==2 {print $1}')
	[ -n "$members" ] || die "$tar has no members"
	if [ -n "$member" ]; then
		printf '%s\n' "$members" | grep -qx -- "$member" \
			|| die "$tar has no '$member' member (has: $(printf '%s' "$members" | tr '\n' ' '));
  repack with tools/starq_odin_tar.py, or pass flash --member NAME if the PIT calls it something else"
	fi
}

show_archive() {
	local tar=$1
	echo "== $tar ($(stat -c %s "$tar") bytes)"
	python3 "$tar_tool" --list "$tar" | while read -r name size; do
		printf '  %-24s %12s bytes\n' "$name" "$size"
	done
}

# The gate. Nothing writes unless this returns 0 in the same invocation.
run_check() {
	local tar=$1 allow_unknown=${2:-0}
	local args=(--check-only -a "$tar")
	if [ "$allow_unknown" = 1 ]; then
		echo "warning: --allow-unknown: entries that match no PIT partition are skipped," >&2
		echo "  and any entry that matches only by a guessed name may be written to the" >&2
		echo "  wrong partition. Only use this on an archive you built yourself." >&2
		args+=(--allow-unknown)
	fi
	echo "== odin4 ${args[*]}"
	local logf rc
	logf=$(mktemp "${TMPDIR:-/tmp}/starq-odin-check.XXXXXX")
	set +e
	"$odin4" "${args[@]}" 2>&1 | tee "$logf"
	rc=${PIPESTATUS[0]}
	set -e
	if grep -qi 'no .*devices detected' "$logf"; then
		# The phone dropped off the bus between -l and now (or -l raced a replug).
		rm -f "$logf"
		echo "error: the device is no longer in Download Mode." >&2
		download_mode_hint >&2
		return 1
	fi
	rm -f "$logf"
	if [ "$rc" -ne 0 ]; then
		echo "error: --check-only rejected the archive (exit $rc)." >&2
		echo "  4 = archive/PIT format problem, 5 = an entry matches no partition (or twice)." >&2
		echo "  If the name is the problem, rebuild with --member-name <PIT file_name>." >&2
		return "$rc"
	fi
	echo "== check passed: PIT and archive agree; nothing was written"
}

cmd_build() {
	local img= name=boot.img md5=0
	img=${1:-}
	[ -n "$img" ] || die "usage: starq-odin.sh build BOOT_IMG [--md5] [--member-name NAME]"
	shift
	while [ $# -gt 0 ]; do
		case "$1" in
		--md5) md5=1 ;;
		--member-name)
			shift
			[ $# -gt 0 ] || die "--member-name needs a value"
			name=$1
			;;
		*) die "unknown option $1" ;;
		esac
		shift
	done
	local out=${img%.img}
	out=${out%.IMG}
	if [ "$md5" = 1 ]; then out="$out.tar.md5"; else out="$out.tar"; fi
	local args=("$out" --boot "$img" --member-name "$name")
	[ "$md5" = 1 ] && args+=(--md5)
	python3 "$tar_tool" "${args[@]}"
	echo
	echo "next (phone in Download Mode): tools/starq-odin.sh check $out"
}

cmd_check() {
	local tar= allow_unknown=0
	tar=${1:-}
	[ -n "$tar" ] || die "usage: starq-odin.sh check TAR [--allow-unknown]"
	shift
	while [ $# -gt 0 ]; do
		case "$1" in
		--allow-unknown) allow_unknown=1 ;;
		*) die "unknown option $1" ;;
		esac
		shift
	done
	need_odin4
	need_archive "$tar"
	show_archive "$tar"
	need_device
	run_check "$tar" "$allow_unknown"
}

cmd_flash() {
	local tar= yes=0 member=boot.img
	tar=${1:-}
	[ -n "$tar" ] || die "usage: starq-odin.sh flash TAR [--member NAME] --yes"
	shift
	while [ $# -gt 0 ]; do
		case "$1" in
		--yes|-y) yes=1 ;;
		--member)
			shift
			[ $# -gt 0 ] || die "--member needs a value"
			member=$1
			;;
		*) die "unknown option $1" ;;
		esac
		shift
	done
	need_odin4
	# A boot flash gets the strictest check: the archive must be a valid tar
	# with a boot.img member holding a real boot image, then pass the PIT
	# gate, then be confirmed.
	need_archive "$tar" "$member"
	# tar exits non-zero on the SIGPIPE from dd; the bytes are what matter.
	local magic
	magic=$(tar -xOf "$tar" "$member" 2>/dev/null | dd bs=1 count=8 status=none 2>/dev/null || true)
	[ "$magic" = "ANDROID!" ] \
		|| die "$tar: member '$member' does not start with ANDROID! (got '$magic'); refusing to flash it to boot"
	show_archive "$tar"
	need_device
	run_check "$tar" 0 || exit 1

	echo
	echo "about to overwrite the boot partition through Download Mode"
	echo "  archive: $tar"
	echo "  device:  $("$odin4" -l 2>/dev/null)"
	echo "This replaces the bootloader's only kernel. A bad image needs a stock AP:"
	echo "  tools/starq-odin.sh restore AP_<...>.tar.md5 --yes"
	if [ "$yes" != 1 ]; then
		local answer=
		read -r -p "Type 'yes' to continue: " answer || true
		[ "$answer" = yes ] || { echo "aborted (type 'yes', or pass --yes)" >&2; exit 1; }
	fi

	# No --efs-clear, no --allow-unknown, no repartition: -a only, and --reboot
	# so a failed boot does not leave the phone sitting in Download Mode.
	"$odin4" --reboot -a "$tar"
	echo "== flashed; the phone reboots. If it hangs, hold Power + Vol Down ~10 s"
	echo "   and boot TWRP (Vol Up + Bixby + Power) to pull pstore:"
	echo "   tools/starq-flash.sh pull-log logs/starq-boot1.txt"
}

cmd_restore() {
	local tar= yes=0
	tar=${1:-}
	[ -n "$tar" ] || die "usage: starq-odin.sh restore AP_TAR --yes"
	shift
	while [ $# -gt 0 ]; do
		case "$1" in
		--yes|-y) yes=1 ;;
		*) die "unknown option $1" ;;
		esac
		shift
	done
	need_odin4
	need_archive "$tar"
	show_archive "$tar"
	need_device
	# Stock AP archives carry dozens of partitions; --allow-unknown is not an
	# option here, so a genuine Samsung AP must pass the PIT check as-is.
	run_check "$tar" 0 || exit 1

	echo
	echo "about to flash a full AP (recovery path)"
	echo "  archive: $tar"
	echo "A stock AP rewrites the boot and system partitions from Samsung's own"
	echo "image. Any iOS root or Android install on this phone is replaced or"
	echo "orphaned; keep backup/starq/ and the firmware downloads."
	if [ "$yes" != 1 ]; then
		local answer=
		read -r -p "Type 'yes' to continue: " answer || true
		[ "$answer" = yes ] || { echo "aborted (type 'yes', or pass --yes)" >&2; exit 1; }
	fi
	"$odin4" --reboot -a "$tar"
	echo "== AP flashed; the phone reboots into Android"
}

cmd_list() {
	need_odin4
	"$odin4" -l
}

udev_rule_text() {
	cat <<-EOF
	# Samsung Download Mode devices (04e8), for odin4 (libusb)
	SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="6601", MODE="0660", TAG+="uaccess", GROUP="plugdev"
	SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="685d", MODE="0660", TAG+="uaccess", GROUP="plugdev"
	SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="68c3", MODE="0660", TAG+="uaccess", GROUP="plugdev"
	SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="68ef", MODE="0660", TAG+="uaccess", GROUP="plugdev"
	SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="4eee", MODE="0660", TAG+="uaccess", GROUP="plugdev"
	SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="4eef", MODE="0660", TAG+="uaccess", GROUP="plugdev"
	EOF
}

# Prefer the rule shipped with the checkout we got odin4 from; fall back to
# the same six VID/PID pairs written inline, so this works with a bare binary.
find_udev_rule() {
	local cand
	if [ -n "${ODIN4_UDEV:-}" ] && [ -f "$ODIN4_UDEV" ]; then
		echo "$ODIN4_UDEV"
		return 0
	fi
	if [ -n "$odin4" ]; then
		cand=$(cd "$(dirname "$odin4")/.." 2>/dev/null && pwd)/udev/60-odin4.rules
		[ -f "$cand" ] && { echo "$cand"; return 0; }
	fi
	for cand in "$repo/../odin4/udev/60-odin4.rules" /tmp/opencode/odin4/udev/60-odin4.rules; do
		[ -f "$cand" ] && { echo "$cand"; return 0; }
	done
	return 1
}

cmd_install_udev() {
	local dest=/etc/udev/rules.d/60-odin4.rules src=
	src=$(find_udev_rule) || src=

	if [ "$(id -u)" != 0 ]; then
		echo "not root: run these three commands yourself (needs sudo)"
		if [ -n "$src" ]; then
			echo "  $SUDO install -m 0644 '$src' '$dest'"
		else
			echo "  $SUDO tee '$dest' >/dev/null <<'RULE'"
			udev_rule_text | sed 's/^/  /'
			echo "  RULE"
		fi
		echo "  $SUDO udevadm control --reload-rules && $SUDO udevadm trigger"
		echo
		echo "then unplug and replug the phone in Download Mode."
		echo "Rule content that will be installed:"
		if [ -n "$src" ]; then sed 's/^/  /' "$src"; else udev_rule_text | sed 's/^/  /'; fi
		return 0
	fi

	mkdir -p /etc/udev/rules.d
	if [ -n "$src" ]; then
		install -m 0644 "$src" "$dest"
	else
		udev_rule_text > "$dest"
		chmod 0644 "$dest"
	fi
	udevadm control --reload-rules
	udevadm trigger
	echo "== installed $dest; unplug and replug the phone in Download Mode"
	grep -c 04e8 "$dest" | xargs -I{} echo "   {} rules for VID 04e8"
}

case "${1:-}" in
build) shift; cmd_build "$@" ;;
check) shift; cmd_check "$@" ;;
flash) shift; cmd_flash "$@" ;;
list) cmd_list ;;
restore) shift; cmd_restore "$@" ;;
install-udev) cmd_install_udev ;;
-h|--help|'') usage; exit 0 ;;
*) usage >&2; exit 2 ;;
esac
