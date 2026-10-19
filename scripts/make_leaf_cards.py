"""Builds the EZ-Tree leaf cards sheet the renderer reads (SecondRevision-leafcards.bin).

EZ-Tree (https://github.com/dgreenheck/ez-tree, MIT, Daniel Greenheck) draws its leaves as cards: a twig of leaves in
one picture. Its three green-able pictures - oak, ash, aspen - go side by side into one sheet, 1024 x 1024 each, with
the aspen's autumn yellow turned to spring green and the oak once more, mirrored, as a fourth. The sheet is stored as
RGBA8, raw deflate (what RdxArchive::Inflate reads), after a small header:

    'RDXLEAF1', width (int32), height (int32), raw bytes (int32), packed bytes (int32), packed data

Run:  python scripts/make_leaf_cards.py <folder with oak.png ash.png aspen.png> <out file>
"""
import colorsys
import struct
import sys
import zlib

from PIL import Image

SIDE = 1024


def spring(im):
    """The aspen's yellow leaves to a spring green: each pixel's hue moved to green, its lightness and saturation kept,
    the brightest a little less so (a yellow leaf is lighter than a green one)."""
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            h, l, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
            # Leaves (yellow, orange) to green; the stalks and twig (brown, dark) kept as they are.
            if s > 0.25 and 0.03 < h < 0.2:
                h = 0.25
                l *= 0.72
                s *= 0.62
            r2, g2, b2 = colorsys.hls_to_rgb(h, l, s)
            px[x, y] = (int(r2 * 255 + 0.5), int(g2 * 255 + 0.5), int(b2 * 255 + 0.5), a)
    return im


def main():
    folder, out = sys.argv[1], sys.argv[2]
    cards = []
    for name in ('oak', 'ash', 'aspen'):
        im = Image.open(f'{folder}/{name}.png').convert('RGBA').resize((SIDE, SIDE), Image.LANCZOS)
        cards.append(spring(im) if name == 'aspen' else im)
    cards.append(cards[0].transpose(Image.FLIP_LEFT_RIGHT))
    sheet = Image.new('RGBA', (SIDE * 4, SIDE), (0, 0, 0, 0))
    for i, c in enumerate(cards):
        sheet.paste(c, (i * SIDE, 0))
    # A hidden texel's colour is the card's nearest green, not black: its mip levels then do not darken the edges.
    raw = sheet.tobytes()
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = packer.compress(raw) + packer.flush()
    with open(out, 'wb') as f:
        f.write(b'RDXLEAF1')
        f.write(struct.pack('<iiii', sheet.width, sheet.height, len(raw), len(packed)))
        f.write(packed)
    print(f'{out}: {sheet.width}x{sheet.height}, {len(raw)} bytes raw, {len(packed)} packed')


if __name__ == '__main__':
    main()
