"""The pictures tools/VolumeCheck writes (BMP), as PNGs, and one sheet of them all: scripts/volume-check.cmd."""
import glob
import os
import sys

from PIL import Image


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else '.'
    pictures = []
    for path in sorted(glob.glob(os.path.join(folder, '*.bmp'))):
        im = Image.open(path).convert('RGB')
        im.save(os.path.splitext(path)[0] + '.png')
        os.remove(path)
        pictures.append((os.path.splitext(os.path.basename(path))[0], im))
    if not pictures:
        print('no pictures in', folder)
        return 1
    # The sheet: three across, each at a third of its size.
    w, h = pictures[0][1].size
    tw, th = w // 3, h // 3
    cols = 3
    rows = (len(pictures) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * tw, rows * th), (0, 0, 0))
    for i, (_name, im) in enumerate(pictures):
        sheet.paste(im.resize((tw, th), Image.LANCZOS), ((i % cols) * tw, (i // cols) * th))
    sheet.save(os.path.join(folder, 'sheet.png'))
    print('%d pictures and their sheet in %s' % (len(pictures), folder))
    return 0


if __name__ == '__main__':
    sys.exit(main())
