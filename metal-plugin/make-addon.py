#!/usr/bin/env python3
"""Pack out/NativeMaliMetal.bundle into a GPU addon (the NGPUADD2 format of UniXNU's scripts/make-gpu-addon.py), which `unixnu build payload
--gpu-addon FILE` hands to the kernel as /chosen/memory-map/GpuAddon; the kernel (UniXNU-evb10 patches/xnu/evb10-userland-mounts.patch, boot-arg evb10-gpu-addon=1)
writes the files to /private/var/ngpu/addons/NativeMaliMetal.bundle/* on the Data volume and union-mounts that over /System/Library/Extensions.

  make-addon.py [OUTPUT]      default out/NativeMaliMetal.addon (overwritten)

Layout, little endian: "NGPUADD2", u32 count, u32 0, u64 total bytes; per file u32 path bytes (with NUL, at most 160), u32 mode, u64 size,
u64 compressed size, the path padded to 8 bytes, the zlib stream padded to 8 bytes."""
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUNDLE = HERE / 'out/NativeMaliMetal.bundle'
EXECUTABLE = ('NativeMaliMetal', 'libvulkan_panfrost.dylib')


def pad(data):
    return data + bytes(-len(data) % 8)


def main():
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / 'out/NativeMaliMetal.addon'
    if not (BUNDLE / 'NativeMaliMetal').is_file():
        sys.exit('Build the plugin first: metal-plugin/build-plugin.sh')
    files = []
    for path in sorted(p for p in BUNDLE.rglob('*') if p.is_file()):
        mode = 0o755 if path.name in EXECUTABLE else 0o644
        files.append(('addons/NativeMaliMetal.bundle/' + path.relative_to(BUNDLE).as_posix(), mode, path.read_bytes()))
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            sys.exit('Bad addon path ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
    blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    output.write_bytes(blob)
    for name, mode, data in files:
        print(f'{mode:o} {len(data):>10} {name}')
    print(f'{output}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
