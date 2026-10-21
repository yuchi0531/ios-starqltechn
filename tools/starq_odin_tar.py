#!/usr/bin/env python3
"""Build an Odin-flashable archive (plain .tar, or .tar.md5) for starqltechn.

  starq_odin_tar.py OUT.tar --boot boot.img [--entry NAME=PATH ...] [--md5]
  starq_odin_tar.py --list ARCHIVE

Why the member name matters
---------------------------
odin4 maps every tar entry to a PIT partition: it takes the entry's basename,
lowercases it, strips image suffixes (.img/.bin/.ext4/.lz4), and compares the
result against each PIT entry's `file_name` and `partition_name`
(src/firmware/firmware_package.cpp). An entry that matches nothing is a hard
error unless --allow-unknown is passed, and two entries may not resolve to one
partition. Samsung's own AP archives name the boot member `boot.img`, so that
is the default here (this repo has no dump of the phone's own PIT, only the
convention). `--member-name` overrides it. Extra partitions are the same
problem: `--entry recovery=...` will not match a PIT whose file_name is
`recovery.img`, so pass the PIT name, e.g. `--entry recovery.img=...`.

Nothing in this tool touches the phone. `starq-odin.sh check` is the way to
confirm the names against the real PIT (odin4 --check-only) before any write.

Output is byte-reproducible: mtime 0, uid/gid 0, mode 0644, empty uname/gname,
members in the order given, plain POSIX ustar headers, no PAX/GNU extensions.
A .tar.md5 also embeds the archive name in its trailer, exactly like md5sum, so
re-running the same command to the same path yields an identical file.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

MAGIC = b'ANDROID!'
BLOCK = 512
HERE = os.path.dirname(os.path.abspath(__file__))
IMAGE_SUFFIXES = ('.img', '.bin', '.ext4', '.lz4')


def die(msg):
    sys.exit(f'error: {msg}')


def warn(msg):
    print(f'warning: {msg}', file=sys.stderr)


# ---------------------------------------------------------------- tar writing

def _octal(value, width):
    """`width` bytes of right-aligned octal digits, NUL terminated."""
    digits = format(value, 'o')
    if len(digits) > width - 1:
        die(f'value {value} does not fit in a {width}-byte tar header field')
    return digits.rjust(width - 1, '0').encode('ascii') + b'\0'


def ustar_header(name, size):
    raw = name.encode('utf-8')
    if not raw or len(raw) > 100:
        die(f'member name must be 1..100 bytes: {name!r}')
    h = bytearray(BLOCK)

    def put(off, data):
        h[off:off + len(data)] = data

    put(0, raw)                    # name
    put(100, _octal(0o644, 8))      # mode
    put(108, _octal(0, 8))          # uid
    put(116, _octal(0, 8))          # gid
    put(124, _octal(size, 12))      # size
    put(136, _octal(0, 12))         # mtime 0 -> reproducible
    put(148, b' ' * 8)              # checksum is computed with this as spaces
    put(156, b'0')                  # typeflag: regular file
    put(257, b'ustar\0')            # magic + version: plain POSIX ustar
    put(263, b'00')
    # uname/gname stay NUL (no host or user name leaks in), devmajor/devminor
    # stay NUL (odin4 reads them as 0), prefix stays NUL (names are short).
    put(148, format(sum(h), 'o').rjust(6, '0').encode('ascii') + b'\0 ')
    return bytes(h)


def write_tar(out, members, md5, trailer_name):
    with open(out, 'wb') as f:
        for name, path in members:
            size = os.path.getsize(path)
            f.write(ustar_header(name, size))
            with open(path, 'rb') as src:
                shutil.copyfileobj(src, f, 1 << 20)
            pad = (-size) % BLOCK
            if pad:
                f.write(b'\0' * pad)
        # Two zero blocks end the archive. This also means a .tar.md5 trailer
        # appended below sits after a 512-aligned boundary, which is what
        # odin4's tar_md5::locate_trailer() looks for, and it cannot corrupt
        # the last member: that member is already padded and terminated.
        f.write(b'\0' * (2 * BLOCK))
        content_end = f.tell()
    assert content_end % BLOCK == 0

    if md5:
        # Hash exactly the bytes odin4 hashes: [0, content_end), i.e. everything
        # before the trailer line, not the file size.
        digest = hashlib.md5()
        with open(out, 'rb') as r:
            remaining = content_end
            while remaining:
                chunk = r.read(min(1 << 20, remaining))
                if not chunk:
                    die('short read while hashing the archive')
                digest.update(chunk)
                remaining -= len(chunk)
        with open(out, 'ab') as a:
            # "<hex>  <name>\n" at offset content_end == md5sum style; odin4
            # finds the 32 hex digits at a 512-aligned offset and takes the
            # trailer start as content_end.
            a.write(f'{digest.hexdigest()}  {trailer_name}\n'.encode('ascii'))
        return content_end, digest.hexdigest()
    return content_end, None


# ------------------------------------------------------------- tar inspection

def _tar_octal(field):
    text = field.split(b'\0')[0].strip()
    if not text:
        return 0
    try:
        return int(text, 8)
    except ValueError:
        pass
    # GNU base-256 encoding, used by stock Samsung AP archives for >8 GiB members.
    if field[0] & 0x80:
        return int.from_bytes(bytes([field[0] & 0x7f]) + field[1:], 'big')
    die(f'invalid octal field in tar header: {field!r}')


def read_members(path):
    """Walk the ustar headers and return [(name, size)] up to the end marker.

    Deliberately small: plain ustar as written above, plus tolerance for the
    GNU/PAX metadata entries a stock Samsung AP contains. It is a sanity check
    for "is this an archive, and what is in it", not a full tar implementation.
    """
    if not os.path.isfile(path):
        die(f'not a regular file: {path}')
    total = os.path.getsize(path)
    members = []
    pos = 0
    with open(path, 'rb') as f:
        while pos + BLOCK <= total:
            f.seek(pos)
            h = f.read(BLOCK)
            if len(h) < BLOCK:
                die(f'{path}: truncated tar header at offset {pos}')
            if h == b'\0' * BLOCK:
                break
            stored = _tar_octal(h[148:156])
            unsigned = sum(h[:148]) + 8 * 0x20 + sum(h[156:])
            signed = sum(b - 256 if b > 127 else b for b in h[:148]) + 8 * 0x20 \
                + sum(b - 256 if b > 127 else b for b in h[156:])
            if stored not in (unsigned, signed):
                die(f'{path}: bad tar header checksum at offset {pos} '
                    f'(stored {stored:o}, computed {unsigned:o})')
            name = h[0:100].split(b'\0')[0].decode('utf-8', 'replace')
            if h[257:262] == b'ustar':
                prefix = h[345:500].split(b'\0')[0].decode('utf-8', 'replace')
                if prefix:
                    name = f'{prefix}/{name}'
            size = _tar_octal(h[124:136])
            if h[156:157] in (b'0', b'\0'):
                members.append((name, size))
            pos += BLOCK + size + (-size) % BLOCK
    if not members:
        die(f'{path}: no regular-file members (not a usable Odin archive)')
    return members


# ------------------------------------------------------------------- packing

def check_boot_image(path):
    if not os.path.isfile(path):
        die(f'no such file: {path}')
    with open(path, 'rb') as f:
        magic = f.read(8)
    if magic != MAGIC:
        die(f'{path}: not an Android boot image (magic {magic!r}, want ANDROID!)')
    # starq_mkboot.py is the authority on the v0 header; it also catches
    # truncated section tables and leaves the parse printed for the log.
    mkboot = os.path.join(HERE, 'starq_mkboot.py')
    proc = subprocess.run([sys.executable, mkboot, '--info', path],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sys.stderr.write(proc.stdout)
    if proc.returncode != 0:
        die(f'{path}: starq_mkboot.py --info rejected this image')


def parse_entry(spec):
    name, sep, path = spec.partition('=')
    if not sep or not name or not path:
        die(f'--entry wants NAME=PATH, got {spec!r}')
    if not os.path.isfile(path):
        die(f'--entry {name}: no such file: {path}')
    if os.path.getsize(path) == 0:
        die(f'--entry {name}: {path} is empty')
    return name, path


def normalized_key(name):
    """The comparison key odin4 derives from a tar entry name.

    odin4 takes the basename, lowercases it and strips .lz4/.ext4/.img/.bin,
    then compares that against the same transform of each PIT entry's
    `file_name` and `partition_name`. Two members with the same key would
    therefore resolve to the same partition, which odin4 rejects.
    """
    base = name.rsplit('/', 1)[-1].lower()
    while True:
        for suffix in IMAGE_SUFFIXES:
            if base.endswith(suffix):
                base = base[:-len(suffix)]
                break
        else:
            return base


def cmd_build(args):
    members = []
    by_key = {}

    def add(name, path):
        key = normalized_key(name)
        if key in by_key:
            other = by_key[key]
            if other == name:
                die(f'member name {name!r} given twice')
            # e.g. boot.img + BOOT: different names, one PIT partition.
            die(f'members {other!r} and {name!r} would both map to PIT partition '
                f'{key!r} (odin4 rejects that); keep one')
        by_key[key] = name
        members.append((name, path))

    if args.boot:
        check_boot_image(args.boot)
        add(args.member_name, args.boot)
    for spec in args.entry:
        add(*parse_entry(spec))
    if not members:
        die('nothing to pack: pass --boot IMG and/or --entry NAME=PATH')

    # Write next to the target so a full /tmp cannot break the build, then
    # rename: a half-written archive must never be left under the final name.
    tmp = f'{args.out}.tmp'
    content_end, digest = write_tar(tmp, members, args.md5, os.path.basename(args.out))
    os.replace(tmp, args.out)
    if args.md5 and not args.out.lower().endswith('.tar.md5'):
        warn('--md5 was requested but the name does not end in .tar.md5: '
             'odin4 only verifies a trailer for .tar.md5 files')

    print(f'== {args.out}')
    for name, size in read_members(args.out):
        print(f'  {name:<24} {size:>12} bytes')
    print(f'  {"tar content":<24} {content_end:>12} bytes')
    print(f'  {"file":<24} {os.path.getsize(args.out):>12} bytes')
    if digest:
        print(f'  {"md5":<24} {digest}')
        print(f'  trailer covers bytes [0, {content_end}), then "<md5>  '
              f'{os.path.basename(args.out)}\\n"')
    print('next: tools/starq-odin.sh check ' + args.out)


def cmd_list(args):
    members = read_members(args.archive)
    for name, size in members:
        print(f'{name} {size}')
    return 0


def main():
    ap = argparse.ArgumentParser(
        description='Build an Odin archive for starqltechn, or list one.',
        epilog='The member name is matched against the phone\'s own PIT, so run '
               '`tools/starq-odin.sh check OUT.tar` (odin4 --check-only) before '
               'flashing: that is the only way to confirm the match, and it '
               'writes nothing. If the PIT names boot differently, override '
               'with --member-name.',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?', metavar='OUT.tar', help='archive to write')
    ap.add_argument('--boot', metavar='IMG', help='boot image, packed as member boot.img')
    ap.add_argument('--member-name', default='boot.img',
                    help='member name for --boot (default: boot.img, as in Samsung AP archives)')
    ap.add_argument('--entry', action='append', default=[], metavar='NAME=PATH',
                    help='extra raw image as member NAME (e.g. recovery=recovery.img)')
    ap.add_argument('--md5', action='store_true',
                    help='append a .tar.md5 trailer (name the output *.tar.md5)')
    ap.add_argument('--list', metavar='ARCHIVE', dest='archive',
                    help='print the members of an archive and exit')
    args = ap.parse_args()

    if args.archive:
        if args.out or args.boot or args.entry or args.md5:
            ap.error('--list takes only an archive path')
        sys.exit(cmd_list(args))
    if not args.out:
        ap.error('OUT.tar is required')
    cmd_build(args)


if __name__ == '__main__':
    main()
