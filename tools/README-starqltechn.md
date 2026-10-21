# iOS on the Galaxy S9 Snapdragon (`starqltechn`, SM-G9600 / SC-02K)

End-to-end procedure for the Samsung Galaxy S9 with the Qualcomm SDM845 — the
`starqltechn` board in `tools/mkadt.py`. Everything here is untested on
hardware as of 2026-10-21: the pipeline builds and round-trips an image, the
phone side is written but has not been run.

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
`machine_config` for this board). The result is a `kernel.development.*`
Mach-O, which step 2 consumes. The patches for this board are not part of this
directory.

## 2. Build the boot image

Needs `aarch64-linux-gnu-gcc` (+ binutils) and `python3`. **No `mkbootimg`** —
`tools/starq_mkboot.py` packs the image from the stock header instead.

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

Inspect an image without a phone:

```sh
python3 tools/starq_mkboot.py --info starq-boot.img
```

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
zones. It is the only part of this port that has been executed.

## 3. Flash the boot image

```sh
tools/starq-flash.sh boot starq-boot.img      # asks for confirmation; --yes to skip
adb reboot
```

The script prints the node it resolved (`/dev/block/by-name/boot` ->
`/dev/block/sdaN`), shows the parsed header, `dd`s with `conv=fsync`, `sync`s,
and reads the magic back. Never hardcode `sdaN`: the script resolves it every
time, and `boot-name` prints the whole list.

## 4. Boot and read the log

After the reboot the screen shows the shim's lines, then the kernel's. To get
the log after a crash or a hang:

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
  has to be programmed. The loader paints a test pattern to answer this - see
  "The framebuffer test pattern" below.
- `shim/starqltechn/loader.c` has never run on hardware, and nothing in this
  tree has been cross-compiled on the machine that wrote it (no
  `aarch64-linux-gnu-gcc` there). What *was* run is
  `make -C shim/starqltechn` (step 2 above): the same loader.c compiled as host
  C with the board addresses `mmap`ed, driving its validation, copy and logging
  paths. That proves the logic, not the hardware facts.

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

