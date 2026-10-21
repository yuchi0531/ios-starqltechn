#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the xnu shim boots.

Only what the early boot path actually reads (see NOTES.md). Every MMIO address
is board specific and lives in BOARDS below (see site/porting.html for where to
read each one out of that board's live FDT).

Usage:
  mkadt.py OUT.adt [--board sweet|starqltechn] [--dump] [--qemu] [--highmem]
                   [--ram-base HEX] [--ram-size HEX]
"""
import argparse, hashlib, os, struct, sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27


@dataclass(frozen=True)
class Board:
    """One phone's MMIO layout. Every number must come from that board's live
    FDT (`adb shell cat /sys/firmware/fdt > live.dtb`, then dtc), never from a
    guess - see site/porting.html for the node to look at."""
    dram_base: int          # kernel-managed RAM: no carve-out may fall inside
    dram_size: int
    high_dram: tuple        # (base, size) of a usable bank >= 4GiB, or None
    soc_base: int           # /arm-io children are relative to this; not 0
    gicd: int
    gicd_size: int
    gicr: int
    gicr_size: int
    fb: int                 # bootloader splash framebuffer (cont_splash_region)
    fb_size: int
    fb_w: int
    fb_h: int
    fb_stride: int          # bytes per line, a8r8g8b8 -> width * 4
    ufs: int                # internal UFS host, ufshc@1d84000 reg "std"
    ufs_size: int
    timebase: int
    vtimer_intid: int
    ncpus: int
    cpu_compatible: tuple
    soc_compatible: str     # SoC string in the root compatible
    ram_provisional: bool = False  # dram_base/dram_size are a placeholder


BOARDS = {
    'sweet': Board(
        # Above every Qualcomm XPU carve-out (see NOTES.md)
        dram_base=0xb0000000,
        dram_size=0x4d000000,
        # --highmem: the whole upper DDR bank. The live FDT memory node is
        # <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
        # carve-out is in the low bank and the dynamic pools are limited below 4G.
        high_dram=(0x100000000, 0xfcc00000),
        soc_base=0x01000000,
        gicd=0x17a00000, gicd_size=0x10000,
        gicr=0x17a60000, gicr_size=0x100000,
        fb=0x9c000000, fb_size=0x1700000,   # reserved-memory cont_splash_region
        fb_w=1080, fb_h=2400, fb_stride=1080 * 4,
        ufs=0x01d84000, ufs_size=0x3000,    # sweet's ufshc "ufs_mem" reg size
        timebase=19200000,
        vtimer_intid=19,
        ncpus=8,                            # 0-5 Cortex-A55, 6-7 Cortex-A76
        cpu_compatible=('qcom,kryo470', 'ARM,v8'),
        soc_compatible='qcom,sm7150',
    ),
    # Samsung Galaxy S9 SM-G9600 / SC-02K (SDM845). Values read from mainline
    # arch/arm64/boot/dts/qcom/sdm845-samsung-starqltechn.dts + sdm845.dtsi.
    'starqltechn': Board(
        # PROVISIONAL - the 4 GB bank starts at 0x80000000 but is full of
        # carve-outs; we only know the ones in mainline Linux, and only up to the
        # first one (removed-region@88f00000). The real range, including the
        # hyp/tz/modem/pil regions that only the live tree knows, is UNKNOWN
        # until an FDT is pulled from the phone:
        #     adb shell cat /sys/firmware/fdt > live.dtb
        #     dtc -I dtb -O dts -o live.dts live.dtb   # memory + reserved-memory
        # Then set dram_base/dram_size here (or pass --ram-base/--ram-size).
        dram_base=0x80000000,
        dram_size=0x08f00000,               # up to removed-region@88f00000
        # No usable bank above 4 GiB: --highmem does not apply, and the S9 has
        # only 4 GB total, so there is nothing to fall back to.
        high_dram=None,
        soc_base=0x01000000,
        gicd=0x17a00000, gicd_size=0x10000,   # interrupt-controller@17a00000
        gicr=0x17a60000, gicr_size=0x100000,
        # chosen/framebuffer (simple-framebuffer) + reserved-memory
        # framebuffer_mem: memory@9d400000, 1440x2960 a8r8g8b8
        fb=0x9d400000, fb_size=0x2400000,
        fb_w=1440, fb_h=2960, fb_stride=1440 * 4,
        # ufshc@1d84000 reg "std" is 0x2500 here (sweet's is 0x3000); the "ice"
        # window at 0x01d90000 is not mapped by the kernel driver.
        ufs=0x01d84000, ufs_size=0x2500,
        timebase=19200000,                  # arch timer, sdm845 boards
        vtimer_intid=19,                    # GIC_PPI 3 -> 16 + 3
        ncpus=8,                            # MPIDR 0x000..0x700, kryo385
        cpu_compatible=('qcom,kryo385', 'ARM,v8'),
        soc_compatible='qcom,sdm845',
        ram_provisional=True,
    ),
}

SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic; not the phone's

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, board='sweet', ram=None):
    """board: key of BOARDS. qemu=True: same tree, but with QEMU virt's GIC
    addresses and CNTFRQ (tools/qemu-sweet.py). ram=(base, size) overrides the
    kernel-managed RAM range (--ram-base/--ram-size)."""
    bd = BOARDS[board] if isinstance(board, str) else board
    if highmem:
        if bd.high_dram is None:
            raise SystemExit(f'{board}: --highmem is not usable on this board '
                             '(no bank above 4 GiB); set the RAM range from the live FDT')
        phys_base, mem_size = bd.high_dram
    else:
        phys_base, mem_size = bd.dram_base, bd.dram_size
    if ram is not None:
        phys_base, mem_size = ram
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (bd.gicd, bd.gicr, bd.timebase)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SMP: all cores in physical-core order, one homogeneous cluster (sweet:
    # 0-5 Cortex-A55, 6-7 Cortex-A76; starqltechn: 0-3 A55, 4-7 A75). The kernel
    # defaults to one core and uses cpus=N to select an initial prefix (N <= ncpus).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from the phones.
    # clock-frequency is descriptive only and still sweet's numbers on every board.
    cpu_nodes = []
    for core in range(1 if qemu else bd.ncpus):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', b''.join(cstr(c) for c in bd.cpu_compatible)),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2304000000 if core >= 6 else 1804800000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else bd.vtimer_intid)),
        ('reg', u64(gicd - bd.soc_base, bd.gicd_size, gicr - bd.soc_base, bd.gicr_size)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, bd.soc_base, 0x100000000 - bd.soc_base)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(bd.fb, bd.fb_size)),
        ('width', u32(bd.fb_w)),
        ('height', u32(bd.fb_h)),
        ('stride', u32(bd.fb_stride)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(bd.ufs, bd.ufs_size)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching on
    # every board: the kernel patches match that one string, so a board port
    # keeps the node shape and only swaps the SoC entry.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr(bd.soc_compatible) + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]))


def warn_provisional(board, ram):
    bd = BOARDS[board]
    if not bd.ram_provisional or ram:
        return
    print('=' * 72, file=sys.stderr)
    print(f'WARNING: board {board} is PROVISIONAL.', file=sys.stderr)
    print(f'The RAM range 0x{bd.dram_base:08x}+0x{bd.dram_size:x} is a placeholder:', file=sys.stderr)
    print('it stops at the first carve-out known to mainline Linux, not at the real',
          file=sys.stderr)
    print('usable range. Pull the live FDT from the phone and use it:', file=sys.stderr)
    print('    adb shell cat /sys/firmware/fdt > live.dtb', file=sys.stderr)
    print('    dtc -I dtb -O dts -o live.dts live.dtb   # memory + reserved-memory', file=sys.stderr)
    print('then pass --ram-base/--ram-size (or fix BOARDS). Do not boot this on hardware.',
          file=sys.stderr)
    print('=' * 72, file=sys.stderr)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Build the shim/ADT device tree.')
    ap.add_argument('out', metavar='OUT.adt')
    ap.add_argument('--board', choices=sorted(BOARDS), default='sweet',
                    help='target board (default: sweet)')
    ap.add_argument('--qemu', action='store_true', help='QEMU virt GIC/CNTFRQ')
    ap.add_argument('--highmem', action='store_true',
                    help='use the RAM bank above 4 GiB (sweet only)')
    ap.add_argument('--dump', action='store_true', help='print the tree')
    ap.add_argument('--ram-base', type=lambda v: int(v, 0), metavar='HEX',
                    help='override dram-base (e.g. 0x80000000)')
    ap.add_argument('--ram-size', type=lambda v: int(v, 0), metavar='HEX',
                    help='override dram-size (e.g. 0x80000000)')
    args = ap.parse_args()
    if (args.ram_base is None) != (args.ram_size is None):
        ap.error('--ram-base and --ram-size go together')
    ram = None if args.ram_base is None else (args.ram_base, args.ram_size)

    warn_provisional(args.board, ram)
    root = build(qemu=args.qemu, highmem=args.highmem, board=args.board, ram=ram)
    blob = adt.serialize(root)
    open(args.out, 'wb').write(blob)
    if args.dump:
        adt.dump(adt.parse(blob)[0])
    print(f'{args.out}: {len(blob)} bytes', file=sys.stderr)

