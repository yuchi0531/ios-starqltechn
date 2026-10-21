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
| shim compile | `shim/build.sh` with `BOARD=starqltechn`, `PACK=0` |
| boot.img v0 pack | `tools/starq_mkboot.py` |

The default boot-args keep the console **on** (`sweetfb_noconsole=0`): with no
serial port, text on the panel is the only early output.

Inspect an image without a phone:

```sh
python3 tools/starq_mkboot.py --info starq-boot.img
```

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
  confirm against the live FDT before trusting a crash log.
- `tools/mkadt.py` still marks `starqltechn` provisional; passing
  `--ram-base/--ram-size` (which `starq-boot.sh` does) silences its warning.
- Framebuffer address and panel geometry (`0x9d400000`, 1440×2960) come from
  mainline's `framebuffer_mem` node, not from a live dump.
- The shim does not yet validate the payload's `phys_base` window or zero XNU
  RAM. The S8 port does both (`shim/dreamlte/loader.c`): S-Boot leaves the
  previous OS's data in RAM, and XNU hung in `vm_mem_bootstrap` on the S8 until
  it was zeroed. Expect to need the same here.
