"""Builds the bark picture the renderer reads (SecondRevision-bark.bin) from a photographed bark - and, given its height
map, the bark's relief (SecondRevision-barkheight.bin).

The grown trees' bark is ambientCG's Bark001 (https://ambientcg.com/a/Bark001, CC0 1.0): its colour map, 512 x 1024,
round the branch along its width and along it along its height. Stored as RGBA8, raw deflate (what RdxArchive::Inflate
reads), after a small header:

    'RDXBARK1', width (int32), height (int32), raw bytes (int32), packed bytes (int32), packed data

Its height map is the same material's displacement map, the same size and laid the same way: stored the same way, under
'RDXBHGT1', the height in the red (and green and blue), 255 the top, alpha 255.

Run:  python scripts/make_bark.py <Bark001_1K-JPG_Color.jpg> <out file> [<Bark001_1K-JPG_Displacement.jpg> <height out>]
"""
import struct
import sys
import zlib

from PIL import Image


def write_picture(im, magic, out):
    raw = im.tobytes()
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = packer.compress(raw) + packer.flush()
    with open(out, 'wb') as f:
        f.write(magic)
        f.write(struct.pack('<iiii', im.width, im.height, len(raw), len(packed)))
        f.write(packed)
    print(f'{out}: {im.width}x{im.height}, {len(raw)} bytes raw, {len(packed)} packed')


def main():
    src, out = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert('RGBA')
    if im.size != (512, 1024):
        im = im.resize((512, 1024), Image.LANCZOS)
    write_picture(im, b'RDXBARK1', out)
    if len(sys.argv) >= 5:
        h = Image.open(sys.argv[3]).convert('L')
        if h.size != (512, 1024):
            h = h.resize((512, 1024), Image.LANCZOS)
        write_picture(Image.merge('RGBA', (h, h, h, Image.new('L', h.size, 255))), b'RDXBHGT1', sys.argv[4])


if __name__ == '__main__':
    main()
