#!/usr/bin/env python3
"""Pack a Samsung Galaxy S9 (starqltechn, SM-G9600) boot.img v0.

  starq_mkboot.py STOCK_BOOT.img KERNEL_BIN RAMDISK_BIN OUT.img [--dtb FILE]
                                                             [--dtb-set DIR_OR_FILE]
  starq_mkboot.py --info IMG
  starq_mkboot.py --shape IMG          # dtbh | appended | none

The stock image is the template, not just a reference: the bootloader (Qualcomm
ABL on this board) and the qseecom key-update path compare the boot header
against what Knox/AVB recorded, so we keep every field we do not have to change
-- kernel/ramdisk/second/tags addresses, page size, os_version and
os_patch_level, board name and the 16-byte "name" field. The S8 port learned
this the hard way: a header with os_version = 0 makes qseecom fail and the
image not boot.

Two stock shapes exist, and the packer picks the right one from the template:

1. DTBH template (dt_size > 0). Samsung reuses the header_version slot (offset
   0x28) as dt_size, and the DT section after the second stage is a Qualcomm
   DTBH table (magic "DTBH"), not a bare FDT. We carry the stock table verbatim
   unless --dtb gives another DTBH table, because the bootloader's early code
   and the shim must agree on the same tree. **The kernel stays raw here**: ABL
   locates the DTBs through the header's dt offset (DtbOffset = dt_size), so a
   gzip stream would only add a decompression pass and break the working
   sweet-style flow. This path is deliberately untouched by --no-gzip-kernel.

2. Image.gz-dtb template (dt_size == 0, no DT section at all -- what the
   starqltechn stock/ROM images are). The kernel slot is a gzip stream
   followed by concatenated bare FDTs, one per board revision; the bootloader
   decompresses the kernel and hands the kernel a pointer (x0) to the FDT
   whose qcom,board-id / qcom,msm-id match the hardware. For this shape we
   keep dt_size = 0 and write out_kernel = gzip(kernel) + dtb_set:

     - **the gzip stream is mandatory.** With header v0 ABL never reads
       dt_size to find the DTBs: it decompresses the kernel slot and takes the
       DTB offset from where the gzip member ended
       (QcomModulePkg/Library/BootLib/BootLinux.c: decompress() writes
       *pos = next_in - in_buf + 8 into DtbOffset). A raw Image leaves
       DtbOffset 0, DeviceTreeAppended(0) bails out, the fallback "single DT"
       is the ANDROID! magic and fdt_check_header fails -- BootLinux returns
       before the jump, so the phone lands in recovery with a blank screen.
       That is exactly what a raw kernel slot did on hardware. So the packer
       gzips the kernel argument (unless it already is gzip, which is passed
       through unchanged) with a fixed mtime and level so the same inputs give
       the same bytes; --no-gzip-kernel exists for debugging only;
     - the default set is the exact bytes the stock kernel carried after its
       gzip stream, so the board-id set is unchanged and the bootloader's
       selection keeps working; dt_size == 0 is legal for this shape;
     - --dtb-set DIR_OR_FILE substitutes another set (a file of concatenated
       FDTs, or a directory of one-FDT-per-file).

   Our kernel blob is the loader Image built by shim/starqltechn/, and it is
   gzipped exactly like the stock one: ABL relies on the gzip member's end to
   find the appended DTBs, and the DTBs must therefore start immediately after
   it.

The cmdline is copied from the template by default. An empty cmdline is not a
boot blocker -- ABL synthesises most of what the kernel needs -- but it loses
the stock msm_drm.dsi_display0=... token, and the loader reads that from
/chosen/bootargs to pick the panel (AMB577PX01 vs AMB577MQ01) and the ELVSS
byte. Our own boot-args never go here: they travel in the payload cmdline
(tools/mkpayload.py), which the loader keeps preferring for starq* arguments.
--cmdline STR overrides the field, --empty-cmdline restores the old zeroing.

The trailer is what the other Android parts of the bootloader expect:

  0x240  id[32]  = SHA-1 over kernel|ramdisk|second, each followed by its u32
                   length (the AOSP boot.img v0 signature; the dt section is
                   not part of it, which is why an Image.gz-dtb image gets a
                   valid id from us). When dt_size > 0 the legacy Samsung
                   extension dt|dt_size is appended, as this tool always did.
  end            = the sections padded to page_size, then "SEANDROIDENFORCE"

Same recipe as Floppikrob/ios-galaxy-s8 tools/dreamlte_mkboot.py, plus the
validation that script leaves to the caller.
"""
import argparse
import gzip
import hashlib
import os
import struct
import sys
import zlib

MAGIC = b'ANDROID!'
DTBH_MAGIC = b'DTBH'
FDT_MAGIC = 0xd00dfeed
FDT_MAGIC_BYTES = b'\xd0\x0d\xfe\xed'
GZIP_MAGIC = b'\x1f\x8b'
TRAILER = b'SEANDROIDENFORCE'
NAME_OFF, CMDLINE_OFF = 0x30, 0x40
CMD_LEN, ID_OFF, EXTRA_OFF, EXTRA_LEN = 0x200, 0x240, 0x260, 0x400
# Fixed gzip parameters: the packed image must be reproducible from the same
# inputs, so no timestamp in the member header and no "adaptive" level choice.
GZIP_LEVEL, GZIP_MTIME = 9, 0
SECTIONS = ('kernel', 'ramdisk', 'second', 'dt')
# FDT structure block tokens (Devicetree Specification 5.4.1).
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def die(msg):
    sys.exit(f'error: {msg}')


def warn(msg):
    print(f'warning: {msg}', file=sys.stderr)


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


# --- the small FDT reader -------------------------------------------------
# Only what the boot image needs: header, root-node properties (model,
# qcom,msm-id, qcom,board-id) and the structural walk that proves the tree is
# well formed. No dtc, no external tool.


def fdt_root(blob, base):
    """(root_props, totalsize) of the FDT at blob[base:].

    Walks the whole structure block so a corrupt tree raises instead of
    returning half-parsed properties.
    """
    hdr = struct.unpack_from('>10I', blob, base)
    magic, total, off_struct, off_strings = hdr[0], hdr[1], hdr[2], hdr[3]
    if magic != FDT_MAGIC:
        raise ValueError(f'magic {magic:#010x}, not d00dfeed')
    if total < 40 or base + total > len(blob):
        raise ValueError(f'totalsize {total:#x} runs past the end of the blob')

    end = base + total
    # Offsets are relative to the FDT's own base and 4-byte aligned within it.
    # The appended set is a plain cat of FDT files, so a member may start at an
    # odd address (real starqltechn: #2 at 0xb5c9d); absolute alignment is wrong.
    p = off_struct
    if off_struct < 40 or base + off_struct >= end or base + off_strings >= end:
        raise ValueError('structure/strings block offset outside the blob')

    def u32():
        nonlocal p
        if p + 4 > total:
            raise ValueError('structure block overran totalsize')
        v = struct.unpack_from('>I', blob, base + p)[0]
        p += 4
        return v

    def skip_name():
        nonlocal p
        try:
            e = blob.index(b'\0', base + p, end)
        except ValueError:
            raise ValueError('unterminated node name') from None
        p = (e - base + 4) & ~3

    if u32() != FDT_BEGIN_NODE:
        raise ValueError('structure block does not start with FDT_BEGIN_NODE')
    skip_name()

    props, depth = {}, 1
    while depth:
        tok = u32()
        if tok == FDT_PROP:
            if p + 8 > total:
                raise ValueError('property header overran totalsize')
            ln, nameoff = struct.unpack_from('>II', blob, base + p)
            p += 8
            if p + ln > total:
                raise ValueError('property value overran totalsize')
            if base + off_strings + nameoff >= end:
                raise ValueError('property name offset outside the strings block')
            try:
                ne = blob.index(b'\0', base + off_strings + nameoff, end)
            except ValueError:
                raise ValueError('unterminated property name') from None
            name = blob[base + off_strings + nameoff:ne].decode('ascii', 'replace')
            if depth == 1:          # only the root node's properties matter
                props[name] = blob[base + p:base + p + ln]
            p = (p + ln + 3) & ~3
        elif tok == FDT_BEGIN_NODE:
            skip_name()
            depth += 1
        elif tok == FDT_END_NODE:
            depth -= 1
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise ValueError(f'bad structure token {tok:#010x}')
    return props, total


def cells(v):
    """Property value as big-endian u32 cells (qcom,board-id = <0x08 0x08>)."""
    if not v or len(v) % 4:
        return ()
    return struct.unpack('>%dI' % (len(v) // 4), v)


def fdt_entry(blob, off):
    """(totalsize, model, board-id, msm-id) of the FDT at blob[off:]."""
    props, total = fdt_root(blob, off)
    model = props.get('model', b'').split(b'\0')[0].decode('ascii', 'replace')
    return total, model, cells(props.get('qcom,board-id')), cells(props.get('qcom,msm-id'))


def fdt_chain(blob, off, allow_pad=False):
    """Walk concatenated FDTs from `off`.

    Returns the entry list, or None unless the bytes are a chain of well-formed
    FDTs that covers the blob exactly. With allow_pad, trailing zero padding is
    tolerated (the tail of a padded image); chain_len() then gives the real end.
    """
    entries, p = [], off
    while p < len(blob):
        if p + 8 > len(blob) or struct.unpack_from('>I', blob, p)[0] != FDT_MAGIC:
            if allow_pad and not blob[p:].strip(b'\0'):
                break
            return None
        total = struct.unpack_from('>I', blob, p + 4)[0]
        if total < 40 or p + total > len(blob):
            return None
        try:
            entries.append((p,) + fdt_entry(blob, p))
        except (ValueError, struct.error):
            return None
        p += total
    if not entries or (p != len(blob) and not allow_pad):
        return None
    return entries


def chain_len(entries):
    """Bytes a chain covers, i.e. the end of its last FDT."""
    return entries[-1][0] + entries[-1][1]


def gzip_stream_len(blob):
    """Length of the gzip member at the start of blob, or None if it is not gzip."""
    if blob[:2] != GZIP_MAGIC:
        return None
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        d.decompress(blob)
    except zlib.error as e:
        die(f'gzip stream in the kernel slot is corrupt: {e}')
    if not d.eof:
        die('the kernel slot starts with a gzip header but the stream is truncated')
    return len(blob) - len(d.unused_data)


def gzip_plain_len(blob):
    """Decompressed length of the gzip member at the start of blob."""
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    return len(d.decompress(blob))


def appended_dtbs(blob):
    """(offset, entries, dtb_blob) of the FDTs appended to a kernel blob, else None.

    gzip'd kernel (Image.gz-dtb): the DTBs start where the gzip member ends.
    raw kernel: the DTBs are the tail; the start is the earliest offset whose
    FDT chain covers the blob exactly, so code that happens to contain the
    magic cannot be mistaken for a DTB set.
    """
    start = gzip_stream_len(blob)
    if start is not None:
        tail = blob[start:]
        if not tail:
            return None
        entries = fdt_chain(tail, 0)
        return None if entries is None else (start, entries, tail)
    pos = blob.find(FDT_MAGIC_BYTES)
    while pos >= 0:
        entries = fdt_chain(blob, pos)
        if entries is not None:
            return pos, entries, blob[pos:]
        # Tolerate trailing zero padding after a raw Image + DTBs kernel.
        entries = fdt_chain(blob, pos, allow_pad=True)
        if entries is not None:
            return pos, entries, blob[pos:chain_len(entries)]
        pos = blob.find(FDT_MAGIC_BYTES, pos + 1)
    return None


def describe(entries, blob_name=''):
    """One line per appended FDT plus the board-id set."""
    for i, (off, total, model, board, msm) in enumerate(entries, 1):
        bd = '(' + ' '.join(f'{c:#x}' for c in board) + ')'
        mi = '(' + ' '.join(f'{c:#x}' for c in msm) + ')'
        print(f'  appended DTB #{i} off={off:#x} size={total:#x} '
              f'model="{model}" board-id={bd} msm-id={mi}')
    ids = sorted({b for _, _, _, b, _ in entries})
    print(f'  appended DTB set: {len(entries)} FDTs, {len(ids)} distinct board-ids '
          + ' '.join('(' + ' '.join(f'{c:#x}' for c in b) + ')' for b in ids)
          + (f' ({blob_name})' if blob_name else ''))
    off, total, model, board, msm = entries[0]
    bd = '(' + ' '.join(f'{c:#x}' for c in board) + ')'
    print('  selection    ABL hands the kernel (x0) the entry whose qcom,board-id /'
          '\n               qcom,msm-id match this phone; the image does not record which'
          f'\n               one that is. First entry: "{model}" board-id={bd}')


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
                '       ABL parses that area as a Qualcomm DTBH table. If this is a\n'
                '       plain FDT, it is not a boot.img dt section (Samsung header_version 0\n'
                '       has no dtb slot; a --dtb argument must point at a whole DTBH table).'
                + hint)
        self._appended = None
        self._appended_done = False

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

    def appended(self):
        """(offset, entries, dtb_blob) of the DTBs appended to the kernel, or None."""
        if not self._appended_done:
            self._appended = appended_dtbs(self.parts['kernel'])
            self._appended_done = True
        return self._appended

    def shape(self):
        if self.dt_size:
            return 'dtbh'
        return 'appended' if self.appended() else 'none'

    def digest(self):
        return trailer_id(self.parts)

    def id_state(self):
        if self.id == b'\0' * 32:
            return 'zeroed (the bootloader will not verify it)'
        if self.id != self.digest():
            return 'STALE: does not match the sections'
        hashed = 'kernel+ramdisk+second' + ('+dt' if self.dt_size else '')
        return f'matches {hashed}'

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
        app = self.appended()
        print(f'{self.path}: {len(self.buf)} bytes, boot.img v0')
        print(f'  page_size    {self.page_size}')
        for name, addr in (('kernel', self.kernel_addr), ('ramdisk', self.ramdisk_addr),
                           ('second', self.second_addr)):
            print(f'  {name:<12} size={len(p[name]):#x} addr={addr:#x} off={self.section_off(name):#x}')
        if app:
            gz = gzip_stream_len(p['kernel'])
            if gz is None:
                print(f'  kernel slot  RAW: no gzip member; appended DTB set at '
                      f'kernel+{app[0]:#x} ({len(app[2]):#x} bytes, {len(app[1])} FDTs)')
                print('               ABL takes the DTB offset from the end of a gzip '
                      'member and leaves it 0\n'
                      '               for a raw Image, so this layout does not boot on '
                      'this board')
            else:
                print(f'  kernel slot  GZIP: member {gz:#x} bytes at kernel+0x0 '
                      '(1f 8b), decompresses to '
                      f'{gzip_plain_len(p["kernel"]):#x} bytes')
                print(f'               appended DTB set starts at kernel+{app[0]:#x} '
                      f'(= the gzip member end), {len(app[2]):#x} bytes, '
                      f'{len(app[1])} FDTs -- what ABL uses as DtbOffset')
        print(f'  tags_addr    {self.tags_addr:#x}')
        dt = self.dtbh()
        if not self.dt_size:
            print('  dt           size=0x0 (no dt section%s)'
                  % (': the kernel carries the DTBs, Image.gz-dtb' if app else ''))
        elif dt:
            print(f'  dt           size={self.dt_size:#x} off={self.section_off("dt"):#x} '
                  f'DTBH v{dt[0]} entries={dt[1]} entry_size={dt[2]} total={dt[3]:#x}')
        else:
            print(f'  dt           size={self.dt_size:#x} off={self.section_off("dt"):#x} (short)')
        if app:
            describe(app[1])
        print(f'  name         {self.name!r}')
        print(f'  os_version   {self.os_version:#010x} = {osver_str(self.os_version)}')
        print(f'  cmdline      {self.cmdline!r}')
        if self.extra:
            print(f'  extra_cmdline {self.extra!r}')
        print(f'  id           {self.id[:20].hex()} ... {self.id_state()}')
        tail = self.tail().rstrip(b'\0')
        print(f'  payload ends {self.end:#x}; after it: {tail[:32]!r}'
              f'{" +" + str(len(tail) - 32) + " bytes" if len(tail) > 32 else ""}')
        return self


def trailer_id(parts):
    """The 0x240 id: SHA-1 over kernel|ramdisk|second (+dt when there is one)."""
    sha = hashlib.sha1()
    for name in ('kernel', 'ramdisk', 'second'):
        blob = parts[name]
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    if parts['dt']:
        sha.update(parts['dt'])
        sha.update(struct.pack('<I', len(parts['dt'])))
    return sha.digest() + b'\0' * 12


def read_dt(path):
    blob = open(path, 'rb').read()
    if blob[:4] == DTBH_MAGIC:
        return blob
    if len(blob) >= 4 and struct.unpack_from('>I', blob, 0)[0] == FDT_MAGIC:
        die(f'{path}: a bare FDT (magic d00dfeed), not a DTBH table.\n'
            '       ABL does not look at bare FDTs in the boot.img dt slot; it parses\n'
            '       the Qualcomm DTBH table. A stock image with no dt section at all\n'
            '       (starqltechn: Image.gz-dtb) takes --dtb-set FILE_OR_DIR instead.')
    die(f'{path}: neither DTBH nor an FDT (magic {blob[:4]!r})')


def read_dtb_set(path):
    """Concatenated FDTs from a file, or from every file of a directory."""
    if os.path.isdir(path):
        names = sorted(n for n in os.listdir(path) if os.path.isfile(os.path.join(path, n)))
        if not names:
            die(f'{path}: directory has no files to use as DTBs')
        out = bytearray()
        for n in names:
            blob = open(os.path.join(path, n), 'rb').read()
            # A member is one FDT; a file of several is fine too. No padding
            # between members: the stock set is an unpadded cat (its second
            # FDT starts at 0xb5c9d, not on a 4-byte boundary).
            if fdt_chain(blob, 0) is None:
                die(f'{os.path.join(path, n)}: not a well-formed FDT (magic {blob[:4]!r})')
            out += blob
        return bytes(out)
    blob = open(path, 'rb').read()
    if blob[:2] == GZIP_MAGIC:
        die(f'{path}: this is a gzip stream; --dtb-set wants the appended DTB bytes '
            '(bare FDTs) or a directory of them, or omit it to reuse the stock set')
    entries = fdt_chain(blob, 0)
    if entries is None:
        entries = fdt_chain(blob, 0, allow_pad=True)
        if entries is None:
            die(f'{path}: no well-formed chain of FDTs (magic {blob[:4]!r})')
        blob = blob[:chain_len(entries)]
    return blob


def appended_set_check(new_blob, stock_entries, what):
    """Refuse a set with no FDTs; warn when its board-id set differs from stock.

    stock_entries None means the template carried no set to compare against.
    """
    entries = fdt_chain(new_blob, 0)
    if entries is None:
        die(f'{what} contains no parseable FDT; the bootloader would have no tree '
            'to hand over')
    if stock_entries is None:
        return entries

    def ids(ents):
        seen = sorted({b for _, _, _, b, _ in ents})
        return ' '.join('(' + ' '.join(f'{c:#x}' for c in b) + ')' for b in seen) or '(none)'

    want, have = ids(stock_entries), ids(entries)
    if want != have:
        warn(f'{what} has a different qcom,board-id set than the stock image.\n'
             f'       stock {want}\n'
             f'       new   {have}\n'
             '       The bootloader selects the DTB by board-id: a set that does not '
             'match this phone can\n'
             '       leave it handing the kernel a tree that does not describe the hardware.')
    return entries


def gzip_kernel(kernel):
    """The kernel slot's gzip member, reproducible from the same input bytes.

    Fixed level and mtime because the member header carries the timestamp: with
    time.time() in it, packing twice would give two different images
    (mtime bytes 4..8 of the header, plus the CRC/OS byte).
    """
    return gzip.compress(kernel, GZIP_LEVEL, mtime=GZIP_MTIME)


def kernel_slot(kernel, dtb_set, gzip_it):
    """(slot, gz_len) for the appended-DTBs shape: gzip member + bare FDTs.

    ABL finds the DTBs immediately after the gzip member (it never reads
    dt_size for header v0), so the gzip stream is a hard requirement, not a
    size optimisation. A caller-supplied file that is already gzip is passed
    through unchanged so an Image.gz from CI packs as-is.
    """
    if kernel[:2] == GZIP_MAGIC:
        gz_len = gzip_stream_len(kernel)
        print(f'kernel       {len(kernel):,} B already a gzip stream at offset 0 '
              f'(1f 8b), passed through unchanged')
    elif not gzip_it:
        print('warning: --no-gzip-kernel: the kernel slot stays a raw Image.\n'
              '         ABL derives the DTB offset from the end of the gzip member '
              'and leaves it 0 for a\n'
              '         raw Image, so it will not find the appended DTBs and will '
              'not jump to the kernel\n'
              '         (the phone falls through to recovery). Debugging only.',
              file=sys.stderr)
        gz_len = None
    else:
        raw_len = len(kernel)
        kernel = gzip_kernel(kernel)
        gz_len = len(kernel)
        print(f'kernel       {raw_len:,} B raw -> gzipped {gz_len:,} B '
              f'(mtime={GZIP_MTIME}, level {GZIP_LEVEL})')

    slot = kernel + dtb_set
    if gz_len is not None:
        # The one property ABL relies on: the FDT set starts exactly where the
        # single gzip member ends. If it does not, DtbOffset would point into
        # the middle of the DTB set and fdt_check_header would fail.
        if slot[gz_len:gz_len + 4] != FDT_MAGIC_BYTES:
            die(f'the appended DTB set does not start at the end of the gzip member '
                f'({gz_len:#x}): found {slot[gz_len:gz_len + 4]!r}, want '
                f'd0 0d fe ed.\n'
                '       The DTB set is missing, empty or padded; the bootloader would '
                'compute a DtbOffset\n'
                '       that does not point at an FDT and refuse to boot.')
        print(f'kernel slot  gzip {gz_len:,} B + DTB set {len(dtb_set):,} B = '
              f'{len(slot):,} B; d00dfeed at {gz_len:#x} (the gzip member end)')
    else:
        print(f'kernel slot  raw Image {len(kernel):,} B + DTB set '
              f'{len(dtb_set):,} B = {len(slot):,} B; d00dfeed at {len(kernel):#x}')
    return slot, gz_len


def cmdline_field(stock, text, empty):
    """(bytes, label) for the 0x40 cmdline field."""
    if empty:
        return b'', 'empty (--empty-cmdline: the stock msm_drm.dsi_display0 token is gone)'
    if text is None:
        raw = stock.buf[CMDLINE_OFF:CMDLINE_OFF + CMD_LEN].split(b'\0')[0]
        return raw, f'stock, copied verbatim ({len(raw)} B)'
    raw = text.encode()
    if len(raw) >= CMD_LEN:
        die(f'--cmdline is {len(raw)} bytes; the v0 field holds at most {CMD_LEN - 1} '
            '(it must stay NUL-terminated)')
    return raw, f'--cmdline ({len(raw)} B)'


def pack(stock_path, kernel_path, ramdisk_path, out_path, dtb_path=None,
         dtb_set_path=None, gzip_it=True, cmdline=None, empty_cmdline=False):
    stock = Boot(stock_path)
    kernel = open(kernel_path, 'rb').read()
    ramdisk = open(ramdisk_path, 'rb').read()
    page = stock.page_size
    space = pages(stock.kernel_size, page)
    app = stock.appended()
    if not kernel:
        die(f'{kernel_path}: empty kernel')
    if not ramdisk:
        die(f'{ramdisk_path}: empty ramdisk')
    if len(kernel) > space:
        die(f'kernel is {len(kernel)} bytes, larger than the {space} bytes the stock '
            f'image implied for it.\n'
            '       the bootloader loads the kernel at the header address and the ramdisk\n'
            '       after it; growing past that means the image no longer matches what the\n'
            '       stock header describes. Shrink the kernel, or re-derive the header.')

    dtb_set, gz_len = None, None
    if stock.dt_size:
        if dtb_set_path:
            die('--dtb-set is for a stock image whose DTBs are appended to the kernel; '
                f'{stock_path} has a DTBH dt section, use --dtb')
        if not gzip_it:
            die('--no-gzip-kernel is for the appended-DTB shape only; a DTBH template '
                'keeps its kernel raw\n'
                '       because the bootloader finds the DTBs through the header dt '
                'offset, not the gzip end')
        dt = read_dt(dtb_path) if dtb_path else stock.parts['dt']
        print(f'dt shape     DTBH table in the dt section ({len(dt)} bytes, '
              f'{stock.dtbh()[1] if stock.dtbh() else "?"} entries); '
              'the kernel stays raw')
    else:
        if dtb_path:
            die('--dtb is a DTBH table; '
                + (f'{stock_path} has no dt section (dt_size = 0) but its kernel carries '
                   'appended FDTs, so pass --dtb-set (or nothing, to reuse the stock set)'
                   if app else f'{stock_path} has no dt section (dt_size = 0); pass --dtb-set'))
        dt = b''
        if dtb_set_path:
            dtb_set = read_dtb_set(dtb_set_path)
            entries = appended_set_check(
                dtb_set, app[1] if app else None, f'--dtb-set {dtb_set_path}')
            print(f'dt shape     no dt section (dt_size stays 0); gzip kernel + DTBs, '
                  f'{len(entries)} FDTs from {dtb_set_path}')
            if not app:
                print(f'             the stock kernel carried no DTB set, so there is '
                      'nothing to compare board-ids\n'
                      '             against: make sure this set describes this phone.')
        elif app:
            dtb_set = app[2]
            entries = app[1]
            print(f'dt shape     no dt section (dt_size stays 0); stock kernel is '
                  f'Image.gz-dtb, reusing its {len(entries)} appended FDTs verbatim '
                  f'({len(dtb_set)} bytes) so the board-id set is unchanged')
        else:
            die('the stock image has neither a dt section nor DTBs appended to its '
                'kernel, and no --dtb-set was given;\n'
                '       the bootloader needs a tree to hand the kernel. A starqltechn '
                'stock image has dt_size = 0 with Image.gz-dtb, pass that one.')
        kernel, gz_len = kernel_slot(kernel, dtb_set, gzip_it)

    print(f'kernel  {len(kernel)} bytes'
          + (f' = {"gzip " if gz_len else "raw "}{len(kernel) - len(dtb_set)} + DTBs '
             f'{len(dtb_set)}' if dtb_set
             else f' of the {space} the stock image reserves (stock kernel {stock.kernel_size})')
          + ('' if not dtb_set else f'; the stock kernel slot was {space} '
             f'(stock kernel {stock.kernel_size})'))
    print(f'ramdisk {len(ramdisk)} bytes (stock {stock.ramdisk_size})')
    if dtb_set and len(kernel) > space:
        # The stock Image.gz-dtb kernel slot is gzip + DTBs, so the DTBs already
        # lived outside the "kernel size" the header counted for the compressed
        # part. Same shape here; only the partition limit is real.
        print(f'note: the kernel blob is {len(kernel) - space} bytes past the slot the '
              'stock header implied, because the\n'
              f'      {len(dtb_set)}-byte DTB set rides at its tail. That is the '
              'Image.gz-dtb shape:\n'
              '      the header counts only the compressed part in kernel_size. The '
              'ceiling that matters\n'
              '      is the partition, checked below.')
    if stock.second_size:
        print(f'warning: the stock image has a {stock.second_size}-byte second stage; '
              'the new image has none', file=sys.stderr)

    cmd_raw, cmd_label = cmdline_field(stock, cmdline, empty_cmdline)
    print(f'cmdline      {cmd_label}: {cmd_raw[:96]!r}'
          + (f' +{len(cmd_raw) - 96} B' if len(cmd_raw) > 96 else ''))

    hdr = bytearray(stock.buf[:page])
    struct.pack_into('<10I', hdr, 8, len(kernel), stock.kernel_addr, len(ramdisk),
                     stock.ramdisk_addr, 0, stock.second_addr, stock.tags_addr, page,
                     len(dt), stock.os_version)
    # The cmdline is the stock one unless the caller says otherwise. Our own
    # boot-args stay in the payload cmdline: the loader prefers that for starq*
    # tokens, and this field is what ABL merges into /chosen/bootargs.
    hdr[CMDLINE_OFF:CMDLINE_OFF + CMD_LEN] = b'\0' * CMD_LEN
    hdr[CMDLINE_OFF:CMDLINE_OFF + len(cmd_raw)] = cmd_raw
    hdr[EXTRA_OFF:EXTRA_OFF + EXTRA_LEN] = b'\0' * EXTRA_LEN

    parts = dict(kernel=kernel, ramdisk=ramdisk, second=b'', dt=dt)
    hdr[ID_OFF:ID_OFF + 32] = trailer_id(parts)

    out = bytearray(hdr)
    for name in SECTIONS:
        out += parts[name]
        out += b'\0' * (pages(len(parts[name]), page) - len(parts[name]))
    out += TRAILER
    out += b'\0' * (pages(len(out), page) - len(out))
    open(out_path, 'wb').write(out)
    print(f'{out_path}: {len(out)} bytes (page {page}, dt_size {len(dt)}, '
          f'kernel {len(kernel)}'
          + (f' = {"gzip" if gz_len else "raw"} {len(kernel) - len(dtb_set)} + DTBs'
             if dtb_set else '')
          + f', id {hdr[ID_OFF:ID_OFF + 20].hex()})')
    if len(out) <= len(stock.buf):
        print(f'fits: {len(stock.buf) - len(out)} bytes of slack against the '
              f'{len(stock.buf)}-byte stock image (the whole boot partition on this '
              'phone)')
    else:
        warn(f'OVER THE STOCK IMAGE SIZE: the packed image is {len(out)} bytes, '
             f'{len(out) - len(stock.buf)} bytes more than\n'
             f'       the {len(stock.buf)}-byte stock image. On this phone the stock '
             'boot.img is the whole\n'
             '       boot partition (64 MiB for starqltechn), so an image this size will '
             'not flash:\n'
             '       shrink the kernel or the ramdisk. (If your stock template is a file '
             'smaller than\n'
             '       the partition, compare against the real partition size before '
             'believing this.)')
    return 0


def main():
    ap = argparse.ArgumentParser(
        description='Pack or inspect a Samsung boot.img v0 (starqltechn).',
        epilog='The stock image selects the shape. If it has a dt section (DTBH\n'
               'table) that table is carried verbatim, or replaced with --dtb; the\n'
               'kernel stays raw there, because ABL finds the DTBs through the\n'
               'header dt offset.\n'
               'If dt_size is 0 and the kernel is Image.gz-dtb (a gzip stream\n'
               'followed by bare FDTs, one per board revision -- what the\n'
               'starqltechn stock and ROM images are), the kernel is gzipped and\n'
               'the DTBs are appended after the gzip member, dt_size stays 0. The\n'
               'gzip is mandatory: ABL takes the DTB offset from the end of the\n'
               'gzip member and never reads dt_size for header v0, so a raw Image\n'
               'leaves it with no DTBs and no jump. A kernel that already starts\n'
               'with 1f 8b is passed through instead of gzipped twice.\n'
               '--dtb-set substitutes another DTB set; without it the stock bytes\n'
               'are reused so the qcom,board-id set stays what ABL expects.\n'
               'The cmdline is copied from the template (ABL merges it into\n'
               '/chosen/bootargs, where the loader reads msm_drm.dsi_display0 to\n'
               'pick the panel); --cmdline STR replaces it, --empty-cmdline zeroes\n'
               'it. Our own boot-args go into the shim payload, not here; see\n'
               'tools/starq-boot.sh.',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='+', metavar='PATH',
                    help='STOCK_BOOT KERNEL RAMDISK OUT, or just IMG with --info/--shape')
    ap.add_argument('--info', action='store_true', help='print the header of PATH')
    ap.add_argument('--shape', action='store_true',
                    help='print dtbh | appended | none for PATH and exit')
    ap.add_argument('--dtb', metavar='FILE',
                    help='DTBH table to use instead of the stock dt section')
    ap.add_argument('--dtb-set', metavar='DIR_OR_FILE',
                    help='FDT set to append to the kernel when the stock image has no dt '
                         'section (a file of concatenated FDTs, or a directory of FDTs); '
                         'default: reuse the stock appended set')
    ap.add_argument('--no-gzip-kernel', action='store_true',
                    help='debugging only: leave the kernel slot as a raw Image. ABL then '
                         'cannot find the appended DTBs (it takes that offset from the end '
                         'of the gzip member) and does not boot')
    ap.add_argument('--cmdline', metavar='STR',
                    help='cmdline to write into the header instead of the stock one; this '
                         'is not where our boot-args go (they travel in the payload)')
    ap.add_argument('--empty-cmdline', action='store_true',
                    help='zero the cmdline field (the old behaviour). Loses the stock '
                         'msm_drm.dsi_display0 token the panel detection reads')
    a = ap.parse_args()
    if a.info or a.shape:
        if len(a.paths) != 1:
            ap.error('--info/--shape take exactly one image')
        if a.shape:
            print(Boot(a.paths[0]).shape())
        else:
            Boot(a.paths[0]).info()
        return 0
    if a.cmdline is not None and a.empty_cmdline:
        ap.error('--cmdline and --empty-cmdline are mutually exclusive')
    if len(a.paths) != 4:
        ap.error('usage: starq_mkboot.py STOCK_BOOT.img KERNEL_BIN RAMDISK_BIN OUT.img '
                 '[--dtb FILE | --dtb-set DIR_OR_FILE] [--cmdline STR | --empty-cmdline] '
                 '[--no-gzip-kernel]')
    return pack(*a.paths, dtb_path=a.dtb, dtb_set_path=a.dtb_set,
                gzip_it=not a.no_gzip_kernel, cmdline=a.cmdline,
                empty_cmdline=a.empty_cmdline)


if __name__ == '__main__':
    sys.exit(main())
