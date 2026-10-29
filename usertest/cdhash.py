#!/usr/bin/env python3
"""Print the cdhash (first 20 bytes of the SHA-256 of the CodeDirectory) of a signed Mach-O and what is in its signature.

    cdhash.py FILE...
"""
import hashlib
import struct
import sys

LC_CODE_SIGNATURE = 0x1d
CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0
CSMAGIC_CODEDIRECTORY = 0xfade0c02


def codedirectory(path):
    d = open(path, 'rb').read()
    magic, ncmds = struct.unpack_from('<II', d, 0)
    if magic != 0xfeedfacf:
        raise SystemExit('%s: not a 64-bit little-endian Mach-O' % path)
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == LC_CODE_SIGNATURE:
            start, length = struct.unpack_from('<II', d, off + 8)
            sb = d[start:start + length]
            sig_magic, _, count = struct.unpack_from('>III', sb, 0)
            if sig_magic != CSMAGIC_EMBEDDED_SIGNATURE:
                raise SystemExit('%s: the signature is not an embedded superblob' % path)
            cds = []
            for i in range(count):
                kind, at = struct.unpack_from('>II', sb, 12 + 8 * i)
                m, l = struct.unpack_from('>II', sb, at)
                if m == CSMAGIC_CODEDIRECTORY:
                    cds.append((kind, sb[at:at + l]))
            if not cds:
                raise SystemExit('%s: no CodeDirectory' % path)
            return d, start, cds
        off += size
    raise SystemExit('%s: no LC_CODE_SIGNATURE' % path)


def describe(path):
    d, start, cds = codedirectory(path)
    out = []
    for kind, cd in cds:
        (version, flags, hash_off, ident_off, nspecial, ncode, code_limit,
         hash_size, hash_type, platform, page_shift) = struct.unpack_from('>IIIIIIIBBBB', cd, 8)
        ident = cd[ident_off:cd.index(b'\0', ident_off)].decode()
        digest = hashlib.sha256(cd).digest() if hash_type == 2 else hashlib.sha1(cd).digest()
        out.append(dict(slot=kind, version=version, flags=flags, ident=ident, nspecial=nspecial, ncode=ncode, code_limit=code_limit,
                        page_shift=page_shift, hash_type=hash_type, cdhash=digest[:20].hex(), signature_at=start, file_size=len(d)))
    return out


def main():
    for path in sys.argv[1:]:
        for e in describe(path):
            print('%s cdhash %s slot %d version %#x flags %#x ident %s code slots %d (page shift %d, hash type %d, limit %d)' %
                  (path, e['cdhash'], e['slot'], e['version'], e['flags'], e['ident'], e['ncode'], e['page_shift'], e['hash_type'], e['code_limit']))


if __name__ == '__main__':
    main()
