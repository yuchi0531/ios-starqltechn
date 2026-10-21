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

**This step needs TWRP.** odin4 writes partitions but cannot read them, so the
backup is still `starq-flash.sh`; there is no Odin equivalent. Phone in TWRP,
USB attached:

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
python3 tools/starq_mkboot.py --info data/boot.emmc.win.img   # a real stock image
python3 tools/starq_mkboot.py --shape data/boot.emmc.win.img  # dtbh | appended | none
```

### The two stock boot.img shapes

`tools/starq_mkboot.py` probes the template and packs the matching shape; no
flag selects it.

| Stock shape | Header | DTBs live in | Packer |
| --- | --- | --- | --- |
| DTBH | `dt_size > 0`, dt section after the second stage | the dt section (Qualcomm `DTBH` table) | carried verbatim, or `--dtb FILE` |
| Image.gz-dtb | `dt_size == 0`, no dt section | appended to the kernel blob, after the gzip stream | appended to our `Image`, or `--dtb-set DIR_OR_FILE` |

The real starqltechn images (`data/boot.emmc.win.img`, and the
KernelSU-patched one) are the second shape: page 4096, `kernel_addr=0x8000`,
`ramdisk_addr=0x1000000`, `tags_addr=0x100`, `os_version=0x1a0001a2`, no
`second`, and a kernel slot of gzip + **15 concatenated FDTs** (0xaa5e86 bytes)
for 13 distinct `qcom,board-id` values Rev02..Rev14.

For that shape the packer keeps `dt_size = 0` and writes
`out_kernel = Image + dtb_set`, reusing the stock DTB bytes verbatim by default,
so S-Boot's board-id selection is unchanged. The `Image` is our own loader and
is **not** gzipped: S-Boot has no decompression to do, so raw `Image` + DTBs is
the exact analogue of `Image.gz-dtb`.

The trailer `id` is the AOSP v0 signature, SHA-1 over `kernel|ramdisk|second`,
each followed by its u32 length; the dt section is not part of it. (The old
README of this tool said dt was included — the real images disprove that; the
dt form is only used when a dt section exists, as the legacy Samsung extension.)

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

There are two paths and they are not interchangeable. Both write the same
partition; only TWRP can read anything back.

| | **A. odin4 (Linux-native)** | **B. TWRP + `adb dd`** |
| --- | --- | --- |
| Host tools | `odin4` (libusb) + `python3` | `adb` + TWRP on the phone |
| Phone mode | Download Mode (`Vol Down + Bixby + Power`, then `Vol Up`) | Recovery (`Vol Up + Bixby + Power`) |
| Write boot | `starq-odin.sh flash` -> Odin/Thor protocol, one archive | `starq-flash.sh boot` -> `dd` to `/dev/block/by-name/boot` |
| Pre-flight | **mandatory** `odin4 --check-only`: archive checked against the phone's own PIT | magic + header parse only |
| Read back | **impossible** — write-only | `backup`, `pull-log`, `boot-name` |
| Recovery from a bad image | `restore AP_TAR` (stock AP, **replaces the Windows-only Odin route**) | `starq-flash.sh boot backup/starq/boot.img` |
| Needs root on host once | yes, for the udev rule | no |

### A. Odin4: no TWRP, no adb, no Windows

odin4 speaks the **Thor USB protocol — the same one Android's Odin uses** — so
the boot image can be written straight from Linux. No TWRP, no `adb push`, no
Wine. Build it once (the GUI needs Qt6 and is not needed here):

```sh
git clone https://github.com/Llucs/odin4 && cd odin4
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DODIN4_BUILD_GUI=OFF
cmake --build build --parallel $(nproc)
```

```sh
tools/starq-odin.sh install-udev                            # sudo: 04e8 download-mode USB access
tools/starq-odin.sh build starq-boot.img                    # -> starq-boot.tar
tools/starq-odin.sh check starq-boot.tar                    # GATE: needs the phone, writes nothing
tools/starq-odin.sh flash starq-boot.tar --yes              # re-runs check, then writes + reboots
```

`check` is the gate, and it is not optional: `flash` runs it itself and refuses
to continue if it fails. It is `odin4 --check-only -a TAR`, which downloads the
PIT from the phone and matches every archive entry against it; nothing is
written. `odin4` is looked up in `$ODIN4`, then `PATH`, then `./build/odin4`,
then `/tmp/opencode/odin4/build/odin4`. Add `--md5` to `build` for a `.tar.md5`
with a verified trailer (the name must end in `.tar.md5`).

**The member name must match this phone's PIT.** odin4 takes each tar entry's
basename, lowercases it, strips `.img`/`.bin`/`.ext4`/`.lz4`, and compares it to
the PIT `file_name` and `partition_name`; an entry that matches nothing is a
hard error, and two entries for one partition is also an error. `boot.img` is
the default because that is the name Samsung's AP archives use — this repo has
no dump of the phone's own PIT, so it is a convention, not a verified fact.
`--member-name` (and `--entry NAME=PATH` for other partitions) overrides it,
and `check` is how you find out before writing anything.

A bad boot image still needs a **stock AP**, and odin4 can do that too:

```sh
tools/starq-odin.sh restore AP_G9600_....tar.md5 --yes      # stock AP, --reboot
```

### B. TWRP + `adb dd` (unchanged)

Still the only way to read the phone: pull logs and pstore, back up
partitions, list `/dev/block/by-name/`. **odin4 cannot replace
`starq-flash.sh backup` or `pull-log` — it is write-only.**

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
- **Does the panel scan out of `0x9d400000` at all?** Likely **no**: both S9
  panels are declared `dsi_cmd_mode` in the vendor DTS
  (`dtsi_px01.dtsi:70`, `dtsi_mq01.dtsi:66`), i.e. command mode with their own
  GRAM, in which case nothing written to that buffer reaches the panel and MDSS
  `write_memory_start` plus a DSI bring-up has to be programmed. This is the
  single most important open question for the display. The loader answers it
  two ways: it paints a test pattern (`starqfb=1`, described in "The
  framebuffer test pattern" below) *and* it prints DSI_CTRL's enable/video/cmd
  bits, which is the deciding evidence regardless of what is on the panel — see
  "Read this before trusting the framebuffer path".
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
| `dsi.c` | DSI0 command-DMA transmit (`starq_dsi_dcs_write`), the panel probe from `/chosen/bootargs`, the `starqelvss=`/`starqmps=` parsers, the panel-aware default sequence and the read-only `DSI_CTRL` mode probe. See "Panel ELVSS override over DSI" above. |
| `build.sh` | Compile + optional pack. `build.sh STOCK_BOOT.img PAYLOAD.bin OUT.img [DTBH.bin|FDT_DIR]`, packed by `tools/starq_mkboot.py`, so `mkbootimg` is not needed. |
| `Makefile` | `make -C shim/starqltechn` builds and runs the host test. |

### What it does, in order

1. Validates the entry: EL1, and `x0` must point at something `shim/fdt.c` can
   parse (`fdt_chosen_u64` / `fdt_chosen_str`, both bounds-checked against the
   header's `totalsize` because the tree is bootloader input).
2. Reads `/chosen/linux,initrd-start` / `linux,initrd-end` - the same two
   properties the sweet shim uses, which is why the payload format and
   `tools/mkpayload.py` are unchanged. Requires the initrd in the low 4 GB bank
   (`0x80000000..0x100000000`).
3. Optionally paints the framebuffer test pattern (below).
4. Reads `/chosen/bootargs` (the **DTB**, not our payload cmdline), logs it, and
   probes the panel name out of `msm_drm.dsi_display0=...`
   (`panel=PX01|MQ01|UNKNOWN` plus the raw token). No register is touched, and
   it runs before the payload is validated so the answer is in the log even if
   the boot fails later. Its lines are prefixed `STARQ panel:`, not `STARQ
   DSI:`, because it is string parsing and stays on even when the DSI path is
   switched off (step 10).
5. Validates the SWEETXNU v1 payload: magic, version, `nsegs` (1..32), the
   segment table inside the ramdisk, `adt_off`/`adt_size` inside the ramdisk,
   `phys_base`+`mem_size` inside `0xa2000000 + 0x18000000`, `entry_va` and
   `kernel_end` at or above `virt_base`, and every segment inside the RAM
   window.
6. Refuses to zero RAM it still needs: the payload, the loader image itself,
   the framebuffer, the ramoops region and the DTB must all be outside the XNU
   window.
7. **Zeroes `phys_base..phys_base+mem_size`.** This is the S8's hard-won step:
   S-Boot leaves the previous OS's RAM in place, and XNU hung in
   `vm_mem_bootstrap` on the S8 depending on how the phone had been rebooted.
8. Copies each segment, zeroes the `vmsize - file_size` tail, cleans to PoC.
9. Writes the ADT at `kernel_end`, `boot_args` on the next page boundary,
   `deviceTreeP` as a **virtual** address, `topOfKernelData`,
   `memSizeActual`, and the framebuffer geometry from `FB_*`.
10. **Unless `starqelvss=off` or `starqdsi=0` is on the payload cmdline**, probes
   DSI0 read-only and logs `DSI_CTRL` with its enable/video/cmd bits,
   `DSI_ACK_ERR_STATUS` and `DSI_TIMEOUT_STATUS`, then the plain-words mode
   verdict; a down link turns every later send into a no-op. Either switch is
   checked *before* the first register access and skips the whole DSI path —
   see "The two DSI kill switches" below.
11. Sends the ELVSS sequence: the `starqelvss=` list if given, nothing for
   `starqelvss=off` or `starqdsi=0`, otherwise the panel-aware default
   (`f0 5a 5a` / `b5 8f dc <elvss>` / `f0 a5 a5`). A failure logs and continues.
12. Jumps with MMU, D-cache and I-cache off and `x0` = `boot_args` PA.

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
gamma or a framebuffer one, and the fix is one 4-byte DCS write.

**What the Android Magisk module actually changes.** Not the whole ELVSS table:
it forces the panel's ELVSS *temperature* byte onto the extreme-cold row,
`TSET 0x8F` = `BIT(7) | 15`, i.e. **-15 °C**. At normal brightness that is the
module's only real effect — MPS stays `0xDC` above 39 cd — so the write is

```
f0 5a 5a              level-1 key on   (0xB5 is in the protected bank)
b5 8f dc <elvss>      TSET 0x8f, MPS 0xdc, ELVSS offset
f0 a5 a5              level-1 key off
```

**Why 4 bytes and not the whole page.** `0xB5` is *not* readable as a payload:
both S9 panels expose only a **2-byte** read at a `0xB0`-selected offset
(`samsung,elvss_rx_cmds_revA = [06 01 00 00 00 00 01 B5 02 16]`, `rx_len=2`,
read offset `0x16`; `0x17` on MQ01), so there is no read-modify-write here and
no DCS read at all. `AMB577MQ01` writes `0xB5` natively as 4 bytes
(`b5 19 dc 0a`, 26 brightness entries). `AMB577PX01` uses a 24-byte form but its
driver only ever *changes* bytes 1, 2, 3 and 23 — and **byte 23 is a per-panel
OTP black-level trim that must be preserved, not synthesised**: writing `0x00`
there throws the panel's calibration away. A 4-byte write leaves PX01's bytes
4..23, including that trim, exactly as they are.

**The two panels differ in byte 3.** Index-0 (brightest) values from the vendor
DTS:

| Panel | Vendor command | TSET | MPS | ELVSS | Ours |
| --- | --- | --- | --- | --- | --- |
| `AMB577PX01` (starqlte) | `b5 19 dc 16 …` | 0x19 | 0xdc | **0x16** | `b5 8f dc 16` |
| `AMB577MQ01` | `b5 19 dc 0a` | 0x19 | 0xdc | **0x0a** | `b5 8f dc 0a` |

So it cannot be a constant. **Detection is free of risk and needs no register
access**: S-Boot's own command line lives in the DTB's `/chosen/bootargs` and the
vendor display driver names the panel in it,

```
msm_drm.dsi_display0=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0
```

(`dsi_display.c:7061-7064` parses the property, `ss_dsi_panel_common.c:4862-4876`
matches it against `panel->name`). The loader prints the token it found, then
`panel=PX01`, `panel=MQ01` or `panel=UNKNOWN`. It reads the **DTB's** bootargs,
not the payload cmdline — the payload cmdline is ours and says nothing about the
panel. These lines are prefixed `STARQ panel:` rather than `STARQ DSI:`: no MMIO
is involved, and they stay in the log when the DSI path is switched off.

**The override is ON by default.** `starqelvss=` is now a *tuning* knob, not the
only way to send anything: with no boot-arg the loader and the kernel both send
the panel-appropriate three-command sequence above. To turn it off:

```sh
tools/starq-boot.sh --cmdline "starqelvss=off" ...
```

With `off`, **nothing in the DSI path runs at all**: the loader checks the switch
before the first register access, so the read-only `DSI_CTRL` mode probe is
skipped too and not one DSI register is read or written. That is the point of
the switch — a read of an **unclocked** MDSS block can hang Qualcomm's
interconnect, which costs an Odin flash to recover — so it must not depend on
S-Boot having left MDSS clocked. The log says which switch fired and what it
guarantees:

```
STARQ DSI: starqelvss=off, no DSI register access at all
```

The panel name is still logged (it is pure string parsing from
`/chosen/bootargs`, no MMIO), under its own `STARQ panel:` prefix rather than
`STARQ DSI:`, so no output line claims DSI activity the switch has forbidden:

```
STARQ panel: panel=PX01 name=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0
```

### The two DSI kill switches

`starqdsi=0` is the **second, independent** switch, with the same name and
meaning as the one the kernel side already uses (patch 0083). Either one is
enough, in any order and alongside other boot-args:

| Boot-arg | Loader (this port) | Kernel (patch 0083) |
| --- | --- | --- |
| `starqelvss=off` | no DSI register access at all | nothing sent |
| `starqdsi=0` | no DSI register access at all | nothing sent |
| both | same, and the log names both | same |

```sh
# the same kill, for when the payload cmdline has to keep starqelvss= values
tools/starq-boot.sh --cmdline "starqdsi=0" ...
# both set: the log reads "starqelvss=off and starqdsi=0, ..."
tools/starq-boot.sh --cmdline "starqelvss=off starqdsi=0" ...
```

A *non-zero* `starqdsi=` (or a value that is not hex) leaves DSI on and is
logged (`bad starqdsi, DSI left on`), so the default is never turned off by a
typo. The values are read as whole tokens: `starqelvss=off starqfb=0` is still
the `off` switch, and `nostarqdsi=0` / `starqdsioff=1` are not switches at all.

To tune, pass your own hex list; it replaces the whole default sequence:

```sh
# the vendor's unmodified value for comparison (PX01)
tools/starq-boot.sh --cmdline "starqelvss=f0,5a,5a;b5,19,dc,16;f0,a5,a5" ...
# a different MPS: 0xcc is the vendor's value below 39 cd
tools/starq-boot.sh --cmdline "starqmps=cc" ...
# ...or say it explicitly in the list
--cmdline "starqelvss=f0,5a,5a;b5,8f,cc,16;f0,a5,a5"
```

Syntax of a list: bytes are one or two hex digits, `,` separates the bytes of one
command, `;` separates commands, up to 8 commands of 64 bytes. Nothing else is
accepted: an empty byte (`b5,,dc`), a trailing `,` or `;`, a three-digit field or
a space makes the whole string invalid and **nothing at all is sent** (the log
says `bad hex list ... nothing sent`). There is no partial application.
`starqmps=<1-2 hex digits>` only changes byte 2 of the default; an invalid value
is logged and `0xdc` is kept.

| Switch | Where | Effect |
| --- | --- | --- |
| *(none)* | loader + kernel | the default: `f0 5a 5a` / `b5 8f dc <elvss>` / `f0 a5 a5`, `<elvss>` from the panel name |
| `starqelvss=off` | loader + kernel | **loader: no DSI register access at all**; kernel: nothing is sent |
| `starqdsi=0` | loader + kernel | same as `starqelvss=off`, independently; either switch works |
| `starqelvss=<hex list>` | loader + kernel | replaces the default sequence entirely |
| `starqmps=cc` | loader only | byte 2 of the default sequence |
| `starqdsisw=1` | loader only | skip the scan-line scheduling, trigger the DMA immediately (mainline's behaviour) |

Two places send it, for one reason:

| Where | When | Source |
| --- | --- | --- |
| loader (`shim/starqltechn/dsi.c`) | every boot, right before the jump to XNU | `/chosen/bootargs` panel probe + the payload cmdline |
| kernel (patch 0083, `bsd/dev/starqltechn_hw.c`) | after `starqltechn_hw_init()`, i.e. once the scheduler runs | `/chosen/bootargs` panel probe + the kernel boot-args |

The kernel copy exists because XNU takes minutes to reach the GUI and the panel
can be power-cycled in between; the loader copy exists because it works even if
XNU panics before that point. Both compute the same three commands.

`STARQ_ELVSS_DEFAULT` (build-time, `shim/starqltechn/loader.c`) still exists, but
it is empty by design: it is only for baking one *specific* sequence into an
image, and a non-empty value there would override the per-panel default.

**Caveat — this is a per-unit judgement.** Forcing `-15 °C` compensation at room
temperature is not a calibration, it is a bias: it lowers ELVSS until the tint
goes away, which can also **crush blacks and clip near-black detail**, and how
much is acceptable differs panel to panel and unit to unit (the vendor driver
also drops MPS to `0xcc` below 39 cd precisely to compensate differently at low
brightness). Nothing here is permanent: the panel resets on the next power
cycle, so a plain reboot restores the previous state and reflashing is not
needed. Change one field at a time and keep the previous working string in
`NOTES.md`.

**No DCS read.** The read path would need the BTA/rx-buffer handling of the
command DMA plus three paginated passes (only 2 bytes are exposed at a time),
which is not worth the risk on an unproven link. A DCS `0xD6` read after a
`0xB0` page select is the vendor's own mechanism and the future path if reading
the current state ever becomes necessary.

Both paths log every byte and every DMA poll as `STARQ DSI:` lines, on the panel
and in both ramoops zones, so a failed attempt is readable afterwards with
`tools/starq-flash.sh pull-log` even when the panel looks black:

```
STARQ panel: panel=PX01 name=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0
STARQ DSI: ctrl=0xae94000 phy=0xae94400 buf=0x9f7ff000
STARQ DSI: DSI_CTRL=0x000001b3 en=1 video=1 cmd=0 hwver=0x00000001 status=0x0 lanes=0xb
STARQ DSI: ack_err=0x00000000 timeout=0x00000000
STARQ DSI: panel in VIDEO mode: the bootloader framebuffer is being scanned out
STARQ DSI: default policy, panel=PX01 mps=0xdc elvss=0x16
STARQ DSI: sched line=3164
STARQ DSI: dcs b5 8f dc 16 pkt=0x8
STARQ DSI: command sent
```

### **Read this before trusting the framebuffer path: both S9 panels are command mode**

The vendor DTS declares *both* S9 panels `dsi_cmd_mode`:
`dtsi_px01.dtsi:70` and `dtsi_mq01.dtsi:66` (`qcom,mdss-dsi-panel-type =
"dsi_cmd_mode"`). An earlier version of this document and of `NOTES.md`
assumed video mode. If the panel really is in command mode it has its own GRAM,
and **writing pixels into the bootloader framebuffer at `0x9d400000` will not
appear on screen at all** — MDSS `write_memory_start` plus a full DSI bring-up
becomes mandatory rather than optional.

The runtime register decides, not the DT, so the loader reports it. The line to
look for in `tools/starq-flash.sh pull-log` is

```
STARQ DSI: DSI_CTRL=0x000001b3 en=1 video=1 cmd=0 ...
STARQ DSI: panel in VIDEO mode: the bootloader framebuffer is being scanned out
```

`video=1` means the bootloader framebuffer path works and the CPU-copy
display plan applies. `cmd=1` with `video=0` means it does not:

```
STARQ DSI: panel in COMMAND mode (vendor DT agrees, dsi_cmd_mode): FB_BASE is NOT on screen unless MDSS push/write_memory_start is programmed
```

`en=0` means the link is down entirely (DSI_CTRL.ENABLE clear); the loader then
sends nothing, logs `LINK DOWN (DSI_CTRL.ENABLE=0)` and XNU still starts. The
same line also prints `DSI_ACK_ERR_STATUS` and `DSI_TIMEOUT_STATUS`, so a link
that is up but erroring shows it.

None of this runs at all with `starqelvss=off` or `starqdsi=0`: there is no
probe line to read, because the switch forbids the register access the probe
needs. If the log shows `no DSI register access at all` and you still want the
mode verdict, drop the switch on one boot — accepting the risk that a hung
interconnect costs an Odin flash.

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

