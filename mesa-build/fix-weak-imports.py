#!/usr/bin/env python3
"""fix-weak-imports.py DYLIB: bind the weak flat-namespace imports of a Mach-O dylib to the image itself (in place, and signs it again).

PanVK's entrypoint tables name every Vulkan function it may not implement as a weak reference (ELF resolves it to NULL at link time).
Mach-O cannot do that at link time, and the Mesa build lets the link through with -undefined dynamic_lookup: each one becomes a flat-namespace
weak import (6645 of them in libvulkan_panfrost.dylib), and dyld resolves each by walking the export trie of EVERY image of the process:
0.8 s in backboardd and 1.4-2.6 s in a poster extension, on the main thread of an extension that has a few seconds to check in (gpu45).
A weak import of the image itself (library ordinal 0, "self") finds nothing in a small trie and ends as NULL: the same table, no walk.

(An absolute symbol of value 0 for each of them does not do: lld makes a rebase of the pointer, which then points at the Mach header, gpu46.)

The patch changes the file, so the ad hoc code signature (SHA-256 of every page) is made again: only the page hashes of the CodeDirectory(ies).
"""
import hashlib
import struct
import sys

LC_CODE_SIGNATURE = 0x1d
LC_DYLD_CHAINED_FIXUPS = 0x80000034
CS_MAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0
CS_MAGIC_CODEDIRECTORY = 0xfade0c02
HASHES = {1: hashlib.sha1, 2: hashlib.sha256, 3: hashlib.sha256, 4: hashlib.sha384}


def main():
    path = sys.argv[1]
    data = bytearray(open(path, 'rb').read())
    magic, = struct.unpack_from('<I', data, 0)
    if magic != 0xfeedfacf:
        raise SystemExit('%s: not a thin 64-bit Mach-O' % path)
    ncmds, = struct.unpack_from('<I', data, 16)
    pos = 32
    fixups = signature = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, pos)
        if cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from('<II', data, pos + 8)
        elif cmd == LC_CODE_SIGNATURE:
            signature = struct.unpack_from('<II', data, pos + 8)
        pos += size
    if not fixups:
        print('%s: no chained fixups, nothing to do' % path)
        return
    base = fixups[0]
    version, starts, imports, symbols, count, fmt, sfmt = struct.unpack_from('<7I', data, base)
    if fmt != 1:
        raise SystemExit('%s: import format %d is not handled' % (path, fmt))
    changed = 0
    for i in range(count):
        at = base + imports + 4 * i
        word, = struct.unpack_from('<I', data, at)
        ordinal, weak = word & 0xff, (word >> 8) & 1
        if ordinal == 0xfe and weak:   # flat namespace, weak
            struct.pack_into('<I', data, at, word & ~0xff)   # library ordinal 0: this image
            changed += 1
    print('%s: %d of %d imports were weak flat-namespace lookups, now lookups in the image itself' % (path, changed, count))
    if not changed:
        return
    if signature:
        sig_off, sig_size = signature
        sb_magic, sb_len, sb_count = struct.unpack_from('>III', data, sig_off)
        if sb_magic != CS_MAGIC_EMBEDDED_SIGNATURE:
            raise SystemExit('%s: unexpected signature blob %#x' % (path, sb_magic))
        for k in range(sb_count):
            typ, off = struct.unpack_from('>II', data, sig_off + 12 + 8 * k)
            cd = sig_off + off
            if struct.unpack_from('>I', data, cd)[0] != CS_MAGIC_CODEDIRECTORY:
                continue
            hash_off, = struct.unpack_from('>I', data, cd + 16)
            slots_special, slots_code, limit = struct.unpack_from('>III', data, cd + 24)
            hash_size, hash_type, _plat, page_log2 = struct.unpack_from('BBBB', data, cd + 36)
            page = 1 << page_log2 if page_log2 else limit
            algo = HASHES[hash_type]
            for s in range(slots_code):
                chunk = bytes(data[s * page:min((s + 1) * page, limit)])
                digest = algo(chunk).digest()[:hash_size]
                data[cd + hash_off + s * hash_size:cd + hash_off + (s + 1) * hash_size] = digest
        print('%s: code directory page hashes made again' % path)
    open(path, 'wb').write(data)


main()
