# iOS on the Galaxy S9 Snapdragon (`starqltechn`, SM-G9600 / SC-02K)

End-to-end procedure for the Samsung Galaxy S9 with the Qualcomm SDM845 — the
`starqltechn` board in `tools/mkadt.py`. Nothing here has run on hardware as of
2026-10-21: the kernel builds in CI (run 39590143218, `xnu-kernel` artifact,
17,172,536-byte `kernel.development.starqltechn`) and the whole chain from Mach-O
to a Samsung `boot.img` runs on a Linux host against a synthetic stock image
(46,816-byte loader Image, 16,658,432-byte `starq-ios-boot.img` with the stock
header and a recomputed kernel SHA-1), but the phone side is written and unrun.
Everything that needs the phone is in "What is still open".

## Reference ports

| Port | SoC | What it gave this port |
| --- | --- | --- |
| `https://github.com/Floppikrob/ios-galaxy-s8` | Exynos 8895 Galaxy S8, reaches the iOS lock screen | the Samsung `boot.img` packing scheme, the BOOT-partition/TWRP workflow (no fastboot), the S6SY761 touch protocol (same IC as the S9), the ramoops logging scheme and the XNU-RAM-zeroing lesson |
| `MakrSas/ios-topaz` | sibling port | not used here |
| `orhunnx/ios-a5xelte` | sibling port | not used here |

The S8 is the closest Samsung reference, but it is Exynos: it says nothing about
the SDM845 side (memory map, RPMh, UFS, GENI, PMIC). Those addresses come from
mainline's `sdm845-samsung-starqltechn.dts` and have to be confirmed against the
live FDT (see "What is still open").

## Read this first: there is no fastboot

Unlike the Redmi Note 10 Pro in the main README, **this phone has no
`fastboot`**. Samsung's bootloader speaks Odin download mode only, and it
refuses to boot an arbitrary kernel from a host: nothing like `fastboot boot`
exists here.

So **every kernel iteration is a write to the boot partition**:

1. boot TWRP, `adb push` the image, `dd` it onto `/dev/block/by-name/boot`;
2. reboot and watch the screen — the shim draws its text on the panel,
   because the debug UART is not wired out on this board;
3. TWRP again (`Vol Up + Bixby + Power`) to read pstore and repeat.

A bad `boot.img` is not recoverable from the phone: it needs a **stock AP
flashed with Odin** (CSC/AP from the matching firmware, phone in download
mode). Keep `backup/starq/boot.img` and the full stock AP on the PC before the
first flash.

## Button combos

| Mode | Combo |
| --- | --- |
| Download (Odin) | `Vol Down + Bixby + Power`, release at the warning, then `Vol Up` |
| Recovery (TWRP) | `Vol Up + Bixby + Power` |
| Forced reboot | hold `Power + Vol Down` ~10 s |

Bixby is the separate button on the left edge, below the volume rocker.

## 0. Back up

Phone in TWRP, USB attached:

```sh
tools/starq-flash.sh boot-name     # confirm the real /dev/block/by-name/ nodes
tools/starq-flash.sh backup        # boot, recovery, efs, persist, modem -> backup/starq/
```

`backup/starq/boot.img` is not optional: it is the template every later image
is packed from (header addresses, page size, `os_version`, board name and the
Qualcomm DTBH `dt` section). `efs` and `persist` hold this phone's own
calibration data.

Keep the matching stock AP (Odin package) too — it is the only way back from a
broken boot partition.

## 1. Build the kernel

Same CI as the main README (`xnu/patches/`, `gh workflow run xnu.yml`, with
`machine_config=STARQLTECHN` for this board). The result is a
`kernel.development.starqltechn` Mach-O, which step 2 consumes.

```sh
gh workflow run xnu.yml -f machine_config=STARQLTECHN
gh run download RUN_ID -n xnu-kernel -D xnu/out-starqltechn
```

The patches for this board are not part of this directory (they are
`xnu/patches/0082-starqltechn-board.patch`). The workflow's `concurrency` group is
`xnu-${{ github.ref }}` with `cancel-in-progress: true`, so **run one machine
config at a time**: a second dispatch within seconds silently cancels the first.

| Run | `machine_config` | Result |
| --- | --- | --- |
| 39590143218 | STARQLTECHN | success, 17,172,536-byte `kernel.development.starqltechn` |
| 39591208114 | SWEET | success (the sweet port still builds) |

## 2. Build the boot image

Host prerequisites: `python3` and the aarch64 cross toolchain; `mkbootimg` too if
this host also builds the `sweet` board (the Samsung path packs the image itself,
but `shim/build.sh`'s default board still calls `mkbootimg`). On Debian/Ubuntu:

```sh
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu mkbootimg
```

`tools/starq-boot.sh` checks for `python3`, `aarch64-linux-gnu-gcc`,
`-objcopy`, `-readelf` and `-nm` before doing any work, and prints that first apt
line if one is missing (it used to surface as a "command not found" halfway
through the shim build).

```sh
tools/starq-boot.sh xnu/out/kernel.development.starqltechn \
    backup/starq/boot.img starq-boot.img
```

That runs, in order:

| Step | Tool |
| --- | --- |
| ADT for this board, RAM `0xa2000000 + 0x18000000` | `tools/mkadt.py --board starqltechn` |
| payload (kernel segments + ADT + boot-args) | `tools/mkpayload.py` |
| loader compile (`shim/starqltechn/`) | `shim/build.sh` with `BOARD=starqltechn`, `PACK=0` |
| boot.img v0 pack | `tools/starq_mkboot.py` |

The kernel section is `shim/starqltechn/`'s S-Boot loader, not the Qualcomm ABL
shim: see "The loader" below.

The default boot-args keep the console **on** (`sweetfb_noconsole=0`): with no
serial port, text on the panel is the only early output.

### The ADT needs the iPhone X device tree

`mkadt.py` copies `/product` from the real D22AP tree, which is what MobileGestalt
reads to answer iPhone10,3 instead of an iPod. Without it the ADT is a valid but
anonymous tree:

| `research/ipsw-x/d22ap.adt` | ADT size | `/product` |
| --- | --- | --- |
| present | 12,588 bytes | yes |
| missing | 9,904 bytes + `warning: ... missing, no /product node` | no |

The extraction that works, straight from the IPSW with no full download (16.7.16 /
20H392, `DeviceTree.d22ap.im4p` is not encrypted so no KBAG to key):

```sh
python3 tools/remotezip.py IPSW_URL Firmware/all_flash/DeviceTree.d22ap.im4p research/ipsw-x
# DER-parse the IM4P, take the bvx2 (LZFSE) payload, decode it
python3 tools/adt.py unwrap research/ipsw-x/Firmware/all_flash/DeviceTree.d22ap.im4p \
    research/ipsw-x/d22ap.adt
```

`adt.py unwrap` does both steps: it walks the IM4P as a DER sequence and pipes a
`bvx2` payload through `lzfse -decode`, so build `lzfse` first (`cmake` in
apple/lzfse, then put the binary in `PATH`). The result is 186,084 bytes and stays
out of git — it is an Apple artifact.

Inspect an image without a phone:

```sh
python3 tools/starq_mkboot.py --info starq-boot.img
```

Measured artifacts on a Linux host (real kernel from CI, synthetic stock image):

| Artifact | Size |
| --- | --- |
| `kernel.development.starqltechn` (CI run 39590143218) | 17,172,536 bytes |
| payload (`mkpayload.py`) | 16,596,320 bytes |
| `shim/out/Image` (loader, arm64 Linux header, magic `ARMd` at `0x38`) | 46,816 bytes |
| `starq-ios-boot.img` | 16,658,432 bytes |

The loader can be built and packed on its own, without the kernel pipeline:

```sh
# compile only -> shim/out/Image
BOARD=starqltechn PACK=0 shim/build.sh
# compile + pack -> starq-boot.img (no mkbootimg involved)
shim/starqltechn/build.sh backup/starq/boot.img shim/out/payload.bin starq-boot.img
# the loader's logic, on the host (no cross compiler needed)
make -C shim/starqltechn
```

`make -C shim/starqltechn` runs `shim/starqltechn/host_test.c`, which
`mmap`s the real board addresses (framebuffer, ramoops, low RAM, the XNU
window), builds a synthetic `SWEETXNU` payload and a real flattened device
tree, and calls the loader's entry point exactly as `start.S` does. It covers
the validation failures, the RAM zeroing, the segment copies, the
`boot_args`/`deviceTreeP` fields, the framebuffer pattern and both ramoops
zones. Together with the cross build and the pack command above, that is the
whole of what has been executed for this port.

## 3. Flash the boot image

```sh
tools/starq-flash.sh boot starq-boot.img      # asks for confirmation; --yes to skip
adb reboot
```

The script prints the node it resolved (`/dev/block/by-name/boot` ->
`/dev/block/sdaN`), shows the parsed header, `dd`s with `conv=fsync`, `sync`s,
and reads the magic back. Never hardcode `sdaN`: the script resolves it every
time, and `boot-name` prints the whole list.

## 4. First boot: read the framebuffer test pattern first

The loader paints the pattern before it validates the payload, so what the panel
shows in the first second is the answer to the one question that decides the rest
of the display work. Stand at the phone, do not film — the pattern is static:

1. Flash, `adb reboot`, watch the panel. Four 64-line bands (white, red, green,
   blue) at the top, then the loader's console text, then a magenta block with a
   cyan square.
2. **Bands and block visible** -> S-Boot left MDSS scanning out of `0x9d400000`.
   Record it and stop worrying about display: the sweet CPU-copy path transfers.
3. **Samsung logo, black or garbage** -> command-mode panel with its own GRAM.
   Nothing the loader writes reaches it; MDSS/DSI has to be programmed before any
   of the iOS display work can be reused. Note whether the cyan square is sheared
   (wrong stride) or absent (nothing scanning out) — different failures.
4. Only after that, read the log. To get it after a crash or a hang:

```sh
# TWRP again (Vol Up + Bixby + Power, after Power + Vol Down to leave the hang)
tools/starq-flash.sh pull-log logs/starq-boot1.txt
```

That saves `logs/starq-boot1-console-ramoops-0-<timestamp>.txt` (the kernel
console from `/sys/fs/pstore/console-ramoops-0`) and the matching
`pmsg-ramoops-0` file (the shim's messages). It warns if both are empty —
pstore only survives until the next boot, so pull it in the *same* TWRP session
that follows the crash.

## 5. Back to Android

`dd` `backup/starq/boot.img` back onto the boot partition
(`tools/starq-flash.sh boot backup/starq/boot.img`), or flash the stock AP with
Odin. If `userdata` was overwritten with the iOS root, format it in TWRP.

## What is still open

- The RAM window `0xa2000000 + 0x18000000` is the largest free gap in the low
  4 GB bank per Samsung's `reserved-memory` map, and `0xa2000000` is where
  postmarketOS loads its initramfs on this device. It is a best guess for XNU:
  confirm against the live FDT before trusting a crash log. The loader and
  `tools/starq-boot.sh` both hardcode it; `starqwin=BASE:SIZE` on the boot-args
  overrides it for the loader's check without a rebuild.
- `tools/mkadt.py` still marks `starqltechn` provisional; passing
  `--ram-base/--ram-size` (which `starq-boot.sh` does) silences its warning.
- Framebuffer address and panel geometry (`0x9d400000`, 1440×2960) come from
  mainline's `framebuffer_mem` node, not from a live dump.
- **Does the panel scan out of `0x9d400000` at all?** Unknown, and it decides
  the display work: if the bootloader leaves the display scanning out of that
  buffer, our CPU-copy path works exactly as on sweet; if the panel is in
  command mode with its own GRAM, nothing written there reaches it and MDSS/DSI
  has to be programmed. The loader paints a test pattern to answer this - it is
  step 1 of the first boot (step 4 above) and is described in "The framebuffer
  test pattern" below.
- The `by-name` node names are unconfirmed. `tools/starq-flash.sh` resolves
  `/dev/block/by-name/<part>`, then `bootdevice/by-name`, then
  `platform/soc/*/by-name`, and never hardcodes `sdaN`, but the names
  themselves (`boot`? `BOOT`? `efs`? `persist`?) have not been read off this
  phone. `boot-name` prints the authoritative list.
- `shim/starqltechn/loader.c` has never run on hardware. What *was* run is
  `make -C shim/starqltechn` (step 2 above): the same loader.c compiled as host
  C with the board addresses `mmap`ed, driving its validation, copy and logging
  paths. The cross build does now succeed on the dev host (`gcc-aarch64-linux-gnu`
  from apt, 46,816-byte Image), but a successful link only proves the code
  compiles and fits; every MMIO address in it is still unproven.
- The stock `boot.img` used for packing so far is a **synthetic but realistic**
  stub (20 MiB kernel slot, page 4096). The real one may reserve a different
  kernel size, `os_version`, board name or DTBH table — pull it
  (`tools/starq-flash.sh backup`) and repack before the first flash.

## The loader: `shim/starqltechn/`

`shim/build.sh BOARD=starqltechn` no longer builds the Qualcomm ABL shim
(`shim.c`/`mmu.c`/`ufs.c`): S-Boot behaves like a Linux bootloader, not like
ABL, so the board has its own entry point, link script and loader. `BOARD=sweet`
is untouched and still produces the same `mkbootimg` command line.

| File | What it is |
| --- | --- |
| `start.S` | arm64 Linux Image header + entry. EL1, MMU and caches off, `x0` = DTB (S-Boot's contract). Position independent; passes its own run-time load address to C so the loader can tell whether zeroing XNU's RAM would erase itself. |
| `loader.ld` | Linked at 0, 32 KiB stack after `.bss`, no secondary stacks. |
| `loader.c` | The S-Boot-specific loader (see below). |
| `dsi.c` | DSI0 command-DMA transmit (`starq_dsi_dcs_write`), the `starqelvss=` hex-list parser and the read-only register snapshot. See "Panel ELVSS override over DSI" above. |
| `build.sh` | Compile + optional pack. `build.sh STOCK_BOOT.img PAYLOAD.bin OUT.img [DTBH.bin]`, packed by `tools/starq_mkboot.py`, so `mkbootimg` is not needed. |
| `Makefile` | `make -C shim/starqltechn` builds and runs the host test. |

### What it does, in order

1. Validates the entry: EL1, and `x0` must point at something `shim/fdt.c` can
   parse.
2. Reads `/chosen/linux,initrd-start` / `linux,initrd-end` - the same two
   properties the sweet shim uses, which is why the payload format and
   `tools/mkpayload.py` are unchanged. Requires the initrd in the low 4 GB bank
   (`0x80000000..0x100000000`).
3. Optionally paints the framebuffer test pattern (below).
4. Validates the SWEETXNU v1 payload: magic, version, `nsegs` (1..32), the
   segment table inside the ramdisk, `adt_off`/`adt_size` inside the ramdisk,
   `phys_base`+`mem_size` inside `0xa2000000 + 0x18000000`, `entry_va` and
   `kernel_end` at or above `virt_base`, and every segment inside the RAM
   window.
5. Refuses to zero RAM it still needs: the payload, the loader image itself,
   the framebuffer, the ramoops region and the DTB must all be outside the XNU
   window.
6. **Zeroes `phys_base..phys_base+mem_size`.** This is the S8's hard-won step:
   S-Boot leaves the previous OS's RAM in place, and XNU hung in
   `vm_mem_bootstrap` on the S8 depending on how the phone had been rebooted.
7. Copies each segment, zeroes the `vmsize - file_size` tail, cleans to PoC.
8. Writes the ADT at `kernel_end`, `boot_args` on the next page boundary,
   `deviceTreeP` as a **virtual** address, `topOfKernelData`,
   `memSizeActual`, and the framebuffer geometry from `FB_*`.
9. Jumps with MMU, D-cache and I-cache off and `x0` = `boot_args` PA.

### The framebuffer test pattern

On by default. Before the payload is validated (so it appears even when the
boot fails), the loader fills `0x9d400000` (1440×2960, stride 5760,
XRGB8888) with:

| Lines | Colour |
| --- | --- |
| 0-63 | white |
| 64-127 | red |
| 128-191 | green |
| 192-255 | blue |
| 2/3 height, middle half of the width | magenta block with a cyan square inside |

then `dsb sy`. On the panel:

- **bands and block visible** -> the bootloader is scanning out of that buffer;
  the CPU-copy display path of sweet applies and MDSS/DSI can wait.
- **Samsung logo, black or garbage instead** -> command-mode panel with its own
  GRAM; writing RAM shows nothing and MDSS/DSI has to be programmed. A sheared
  cyan square means the stride is wrong, not the panel mode.

Turn it off with `STARQ_FB_TEST=0` at build time
(`CFLAGS_EXTRA=-DSTARQ_FB_TEST=0 BOARD=starqltechn shim/build.sh ...`) or with
`starqfb=0` on the boot-args (which `tools/starq-boot.sh` passes to
`mkpayload.py --cmdline`). `starqfb=1` forces it back on. Either way the loader
logs `fb pattern ON`/`OFF`.

### Panel ELVSS override over DSI (`starqelvss=`)

This panel's green tint is an ELVSS (black-level) calibration problem, not a
gamma or a framebuffer one. On Android a Magisk module fixes it by rewriting the
panel's ELVSS temperature compensation to the extreme-cold table; the vendor
driver's own command list is `samsung,elvss_tx_cmds_revA` in
`ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD` (`sdm845-sec-starqlte` DTSI):

```
29 01 00 00 00 00 04 b5 19 dc 0a   ->   DCS 0xB5 with TSET=0x19 MPS=0xdc ELVSS=0x0a
```

i.e. **one DSI long write of four bytes: `0xB5`, TSET (temperature), MPS,
ELVSS offset.** S-Boot leaves MDSS + DSI0 + the panel powered and streaming, so
the loader can send that packet over the DSI command DMA without any display
driver, and a wrong value is undone by the next reboot (the panel resets).

**The bytes are yours to choose** — the port computes nothing. Pass them on the
boot-args, which `tools/starq-boot.sh` already forwards to XNU:

```sh
# one command
tools/starq-boot.sh --cmdline "starqelvss=b5,8f,dc,0a" ...
# level-1 key unlock first, then the ELVSS write
tools/starq-boot.sh --cmdline "starqelvss=f0,5a,5a;b5,8f,dc,0a" ...
# several writes, applied left to right
--cmdline "starqelvss=f0,5a,5a;b5,8f,dc,0a;b5,8f,cc,0c"
```

Syntax: bytes are one or two hex digits, `,` separates the bytes of one command,
`;` separates commands, up to 8 commands of 64 bytes. Nothing else is accepted:
an empty byte (`b5,,dc`), a trailing `,` or `;`, a three-digit field or a space
makes the whole string invalid and **nothing at all is sent** (the log says
`bad hex list ... nothing sent`). There is no partial application.

Default: **nothing is sent, and DSI0 is not even read.** No boot-arg means the
loader does not touch the display block at all: a register read of an unclocked
MDSS can hang Qualcomm's interconnect, and that would cost an Odin flash, so the
read-only snapshot only happens when a transfer is actually about to be
attempted. A compiled-in default exists but is empty (`STARQ_ELVSS_DEFAULT` in
`shim/starqltechn/loader.c`); to bake values into an image:

```sh
CFLAGS_EXTRA='-DSTARQ_ELVSS_DEFAULT="b5,8f,dc,0a"' BOARD=starqltechn shim/build.sh ...
```

Two places send it, for one reason:

| Where | When | Source |
| --- | --- | --- |
| loader (`shim/starqltechn/dsi.c`) | every boot, right before the jump to XNU | `starqelvss=` in the payload cmdline, or `STARQ_ELVSS_DEFAULT` |
| kernel (patch 0083, `bsd/dev/starqltechn_hw.c`) | after `starqltechn_hw_init()`, i.e. once the scheduler runs | `starqelvss=` in the boot-args |

The kernel copy exists because XNU takes minutes to reach the GUI and the panel
can be power-cycled in between; the loader copy exists because it works even if
XNU panics before that point. Both do exactly the same write.

Other switches: `starqdsisw=1` (loader only) skips the scan-line scheduling and
software-triggers the DMA immediately, which is mainline's behaviour; `starqdsi=0`
(kernel only) disables the kernel-side send.

**Finding the panel's current ELVSS state.** Read DCS `0xB5` back: the vendor
driver does it with `samsung,elvss_rx_cmds_revA = [06 01 00 00 00 00 01 b5 02 16]`,
a DCS short read of two parameters. Our code implements **writes only** — the
read path needs the BTA/rx-buffer handling of the command DMA, which is a
different register sequence — so use one of:

- on Android, the Magisk module's own log or `/sys/class/lcd/panel/` registers,
  or `dmesg | grep -i elvss` (the Samsung panel driver logs the tables it picks);
- once the panel is on the correct table and looks right, the value you sent is
  by definition the state you want to keep.

For a starting point, the vendor table's neighbours around one entry are
`b5 19 dc 0a` .. `b5 19 cc 07` (the temperature sweep) — the third and fourth
bytes are what change. Three bytes in the payload are the panel's business; the
loader only logs `STARQ DSI: dcs b5 8f dc 0a` and what the DMA engine answered.

**Warning: a wrong `0xB5` is visible.** ELVSS is the panel's black level, so a
bad TSET/MPS/offset pair leaves the display washed out, too dark, or with a
coloured black — often worse than the tint you were fixing. It is **not**
permanent: the panel resets on the next power cycle, so a plain reboot restores
the previous state, and reflashing is not needed. Change one field at a time and
keep the previous working string in `NOTES.md`.

Both paths log every byte and every DMA poll as `STARQ DSI:` lines, on the panel
and in both ramoops zones, so a failed attempt is readable afterwards with
`tools/starq-flash.sh pull-log` even when the panel looks black:

```
STARQ DSI: ctrl=0xae94000 phy=0xae94400 buf=0x9f7ff000
STARQ DSI: hwver=... ctrl=0x... status=0x0
STARQ DSI: mode=video cmd_en=0 ena=0x1 lanes=0xf
STARQ DSI: vid=0x... active_v=0xc5c0080 total=0x...
STARQ DSI: sched line=3164
STARQ DSI: dcs b5 8f dc 0a pkt=0x8
STARQ DSI: command sent
```

### pstore logging

The kernel console ring in `userdata` (patch 0047) needs UFS, which this loader
does not drive, so the loader logs to **ramoops** instead, in both zones, in the
`fs/pstore` format (`{u32 sig, u32 start, u32 size}` with sig `"DBGC"`,
text at +12):

| Address | Size | File in TWRP | Content |
| --- | --- | --- | --- |
| `0xa1300000` | `0x40000` | `dmesg-ramoops-0` | untouched |
| `0xa1340000` | `0x40000` | `console-ramoops-0` | the loader log |
| `0xa1380000` | `0x40000` | `pmsg-ramoops-0` | the same log, second copy |
| `0xa13c0000` | `0x40000` | `ftrace-ramoops-0` | untouched |

(from `ramoops@0xa1300000`, size `0x100000`, in both the mainline and the
vendor DT; `fs/pstore/ram.c` lays the zones out in that order regardless of the
property order.) Two copies because a forced reboot (`Power + Vol Down`) can
tear one zone header, and pstore then drops that zone - with two, one usually
survives. `tools/starq-flash.sh pull-log` already reads both files.

