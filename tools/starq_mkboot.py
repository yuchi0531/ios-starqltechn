#!/usr/bin/env python3
"""Pack a Samsung Galaxy S9 (starqltechn, SM-G9600) boot.img v0.

  starq_mkboot.py STOCK_BOOT.img KERNEL_BIN RAMDISK_BIN OUT.img [--dtb FILE]
  starq_mkboot.py --info IMG

The stock image is the template, not just a reference: Samsung's S-Boot (their
Qualcomm ABL) and the qseecom key-update path compare the boot header against
what Knox/AVB recorded, so we keep every field we do not have to change --
kernel/ramdisk/second/tags addresses, page size, os_version and os_patch_level,
board name and the 16-byte "name" field. The S8 port learned this the hard
way: a header with os_version = 0 makes qseecom fail and the image not boot.

Samsung reuses the header_version slot (offset 0x28) as dt_size, and the DT
section after the second stage is a Qualcomm DTBH table (magic "DTBH"), not a
bare FDT. We carry the stock table verbatim unless --dtb gives another DTBH
table, because S-Boot's early code and the shim must agree on the same tree.

The trailer is what the other Android parts of S-Boot expect:

  0x240  id[32]  = SHA-1 over kernel|ramdisk|second|dt, each followed by its
                   u32 length, zero padded to 32 bytes
  end            = the sections padded to page_size, then "SEANDROIDENFORCE"

Same recipe as Floppikrob/ios-galaxy-s8 tools/dreamlte_mkboot.py, plus the
validation that script leaves to the caller.
"""
import argparse
import hashlib
import struct
import sys

MAGIC = b'ANDROID!'
DTBH_MAGIC = b'DTBH'
FDT_MAGIC = 0xd00dfeed
TRAILER = b'SEANDROIDENFORCE'
NAME_OFF, CMDLINE_OFF = 0x30, 0x40
CMD_LEN, ID_OFF, EXTRA_OFF, EXTRA_LEN = 0x200, 0x240, 0x260, 0x400
SECTIONS = ('kernel', 'ramdisk', 'second', 'dt')


def die(msg):
    sys.exit(f'error: {msg}')


def pages(n, page):
    return (n + page - 1) // page * page


def osver_str(v):
    """os_version u32 -> "17.0.0 patch 2035-06" (mkbootimg encoding)."""
    if not v:
        return '0 (unset: qseecom rejects this image)'
    major, minor, patch = (v >> 25) & 0x7f, (v >> 18) & 0x7f, (v >> 11) & 0x7f
    year, month = 2000 + ((v >> 4) & 0x7f), v & 0xf
    return f'{major}.{minor}.{patch} patch {year:04d}-{month:02d}'


def cstr(b):
    return b.split(b'\0')[0].decode('ascii', 'replace')


class Boot:
    """The v0 header plus the four sections of one image."""

    def __init__(self, path):
        self.path = path
        self.buf = open(path, 'rb').read()
        if self.buf[:8] != MAGIC:
            die(f'{path}: not an Android boot image (magic {self.buf[:8]!r}, '
                f'want ANDROID!)')
        (self.kernel_size, self.kernel_addr, self.ramdisk_size, self.ramdisk_addr,
         self.second_size, self.second_addr, self.tags_addr, self.page_size,
         self.dt_size, self.os_version) = struct.unpack_from('<10I', self.buf, 8)
        if self.page_size < 512 or self.page_size > 0x10000 or self.page_size & (self.page_size - 1):
            die(f'{path}: implausible page_size {self.page_size}')
        self.name = self.buf[NAME_OFF:NAME_OFF + 16]
        self.cmdline = cstr(self.buf[CMDLINE_OFF:CMDLINE_OFF + CMD_LEN])
        self.extra = cstr(self.buf[EXTRA_OFF:EXTRA_OFF + EXTRA_LEN])
        self.id = self.buf[ID_OFF:ID_OFF + 32]
        self.parts, self.end = self.sections()
        if self.dt_size and self.parts['dt'][:4] != DTBH_MAGIC:
            hint = ''
            if self.dt_size <= 2:
                hint = ("\n       (dt_size is "
                        f"{self.dt_size}: this looks like an Android boot.img v1/v2 with a "
                        "header_version\n       in that slot, not a Samsung v0 image. Point "
                        "this at the phone's own stock boot.img.)")
            die(f'{path}: the dt section at {self.section_off("dt"):#x} starts with '
                f'{self.parts["dt"][:4]!r}, not DTBH.\n'
                '       S-Boot/ABL parses that area as a Qualcomm DTBH table. If this is a\n'
                '       plain FDT, it is not a boot.img dt section (Samsung header_version 0\n'
                '       has no dtb slot; a --dtb argument must point at a whole DTBH table).'
                + hint)

    def section_off(self, key):
        off = self.page_size
        for name in SECTIONS:
            if name == key:
                return off
            off += pages(getattr(self, name + '_size'), self.page_size)
        raise KeyError(key)

    def sections(self):
        off = self.page_size
        parts = {}
        for name in SECTIONS:
            size = getattr(self, name + '_size')
            parts[name] = self.buf[off:off + size]
            if len(parts[name]) != size:
                die(f'{self.path}: truncated {name} section ({len(parts[name])} of '
                    f'{size} bytes)')
            off += pages(size, self.page_size)
        return parts, off

    def digest(self):
        sha = hashlib.sha1()
        for name in SECTIONS:
            blob = self.parts[name]
            sha.update(blob)
            sha.update(struct.pack('<I', len(blob)))
        return sha.digest() + b'\0' * 12

    def dtbh(self):
        """(version, num_entries, entry_size, total_size) of the dt section."""
        dt = self.parts['dt']
        if len(dt) < 20:
            return None
        return struct.unpack_from('<4I', dt, 4)

    def tail(self):
        rest = self.buf[self.end:]
        return rest

    def info(self):
        p = self.parts
        print(f'{self.path}: {len(self.buf)} bytes, boot.img v0')
        print(f'  page_size    {self.page_size}')
        for name, addr in (('kernel', self.kernel_addr), ('ramdisk', self.ramdisk_addr),
                           ('second', self.second_addr)):
            print(f'  {name:<12} size={len(p[name]):#x} addr={addr:#x} off={self.section_off(name):#x}')
        print(f'  tags_addr    {self.tags_addr:#x}')
        dt = self.dtbh()
        if not self.dt_size:
            print('  dt           (none)')
        elif dt:
            print(f'  dt           size={self.dt_size:#x} off={self.section_off("dt"):#x} '
                  f'DTBH v{dt[0]} entries={dt[1]} entry_size={dt[2]} total={dt[3]:#x}')
        else:
            print(f'  dt           size={self.dt_size:#x} off={self.section_off("dt"):#x} (short)')
        print(f'  name         {self.name!r}')
        print(f'  os_version   {self.os_version:#010x} = {osver_str(self.os_version)}')
        print(f'  cmdline      {self.cmdline!r}')
        if self.extra:
            print(f'  extra_cmdline {self.extra!r}')
        if self.id == b'\0' * 32:
            state = 'zeroed (S-Boot will not verify it)'
        elif self.id == self.digest():
            state = 'matches kernel+ramdisk+second+dt'
        else:
            state = 'STALE: does not match the sections'
        print(f'  id           {self.id[:20].hex()} ... {state}')
        tail = self.tail().rstrip(b'\0')
        print(f'  payload ends {self.end:#x}; after it: {tail[:32]!r}'
              f'{" +" + str(len(tail) - 32) + " bytes" if len(tail) > 32 else ""}')
        return self


def read_dt(path):
    blob = open(path, 'rb').read()
    if blob[:4] == DTBH_MAGIC:
        return blob
    if len(blob) >= 4 and struct.unpack_from('>I', blob, 0)[0] == FDT_MAGIC:
        die(f'{path}: a bare FDT (magic d00dfeed), not a DTBH table.\n'
            '       S-Boot does not look at bare FDTs in the boot.img; it parses the\n'
            '       Qualcomm DTBH table that carries the per-platform DTBs. Pass the whole\n'
            '       dt section of a stock S9 boot.img (tools/starq_mkboot.py --info shows it).')
    die(f'{path}: neither DTBH nor an FDT (magic {blob[:4]!r})')


def pack(stock_path, kernel_path, ramdisk_path, out_path, dtb_path=None):
    stock = Boot(stock_path)
    kernel = open(kernel_path, 'rb').read()
    ramdisk = open(ramdisk_path, 'rb').read()
    page = stock.page_size
    space = pages(stock.kernel_size, page)
    dt = read_dt(dtb_path) if dtb_path else stock.parts['dt']
    if not dt:
        die('the stock image has no dt section and --dtb was not given; S-Boot needs '
            'a DTBH table')
    print(f'kernel  {len(kernel)} bytes of the {space} the stock image reserves '
          f'(stock kernel {stock.kernel_size})')
    print(f'ramdisk {len(ramdisk)} bytes (stock {stock.ramdisk_size})')
    if not kernel:
        die(f'{kernel_path}: empty kernel')
    if not ramdisk:
        die(f'{ramdisk_path}: empty ramdisk')
    if len(kernel) > space:
        die(f'kernel is {len(kernel)} bytes, larger than the {space} bytes the stock '
            f'image implied for it.\n'
            '       S-Boot loads the kernel at the header address and the ramdisk after\n'
            '       it; growing past that means the image no longer matches what the stock\n'
            '       header describes. Shrink the kernel, or re-derive the header.')
    if stock.second_size:
        print(f'warning: the stock image has a {stock.second_size}-byte second stage; '
              'the new image has none', file=sys.stderr)

    hdr = bytearray(stock.buf[:page])
    struct.pack_into('<10I', hdr, 8, len(kernel), stock.kernel_addr, len(ramdisk),
                     stock.ramdisk_addr, 0, stock.second_addr, stock.tags_addr, page,
                     len(dt), stock.os_version)
    hdr[CMDLINE_OFF:CMDLINE_OFF + CMD_LEN] = b'\0' * CMD_LEN   # empty, as stock
    hdr[EXTRA_OFF:EXTRA_OFF + EXTRA_LEN] = b'\0' * EXTRA_LEN

    parts = dict(kernel=kernel, ramdisk=ramdisk, second=b'', dt=dt)
    sha = hashlib.sha1()
    for name in SECTIONS:
        sha.update(parts[name])
        sha.update(struct.pack('<I', len(parts[name])))
    hdr[ID_OFF:ID_OFF + 32] = sha.digest() + b'\0' * 12

    out = bytearray(hdr)
    for name in SECTIONS:
        out += parts[name]
        out += b'\0' * (pages(len(parts[name]), page) - len(parts[name]))
    out += TRAILER
    out += b'\0' * (pages(len(out), page) - len(out))
    open(out_path, 'wb').write(out)
    print(f'{out_path}: {len(out)} bytes (page {page}, dt {len(dt)}, '
          f'id {hdr[ID_OFF:ID_OFF + 20].hex()})')
    return 0


def main():
    ap = argparse.ArgumentParser(
        description='Pack or inspect a Samsung boot.img v0 (starqltechn).',
        epilog='Boot-args go into the shim payload, not into the boot.img cmdline '
               '(the stock image has none); see tools/starq-boot.sh.')
    ap.add_argument('paths', nargs='+', metavar='PATH',
                    help='STOCK_BOOT KERNEL RAMDISK OUT, or just IMG with --info')
    ap.add_argument('--info', action='store_true', help='print the header of PATH')
    ap.add_argument('--dtb', metavar='FILE', help='DTBH table to use instead of the stock one')
    a = ap.parse_args()
    if a.info:
        if len(a.paths) != 1:
            ap.error('--info takes exactly one image')
        Boot(a.paths[0]).info()
        return 0
    if len(a.paths) != 4:
        ap.error('usage: starq_mkboot.py STOCK_BOOT.img KERNEL_BIN RAMDISK_BIN OUT.img')
    return pack(*a.paths, dtb_path=a.dtb)


if __name__ == '__main__':
    sys.exit(main())
