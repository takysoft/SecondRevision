"""Builds the bark picture the renderer reads (SecondRevision-bark.bin) from a photographed bark.

The grown trees' bark is ambientCG's Bark001 (https://ambientcg.com/a/Bark001, CC0 1.0): its colour map, 512 x 1024,
round the branch along its width and along it along its height. Stored as RGBA8, raw deflate (what RdxArchive::Inflate
reads), after a small header:

    'RDXBARK1', width (int32), height (int32), raw bytes (int32), packed bytes (int32), packed data

Run:  python scripts/make_bark.py <Bark001_1K-JPG_Color.jpg> <out file>
"""
import struct
import sys
import zlib

from PIL import Image


def main():
    src, out = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert('RGBA')
    if im.size != (512, 1024):
        im = im.resize((512, 1024), Image.LANCZOS)
    raw = im.tobytes()
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = packer.compress(raw) + packer.flush()
    with open(out, 'wb') as f:
        f.write(b'RDXBARK1')
        f.write(struct.pack('<iiii', im.width, im.height, len(raw), len(packed)))
        f.write(packed)
    print(f'{out}: {im.width}x{im.height}, {len(raw)} bytes raw, {len(packed)} packed')


if __name__ == '__main__':
    main()
