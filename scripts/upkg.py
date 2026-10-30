"""Reads the engine's own package files (.dx maps, .utx texture packages) well enough to list what is in them and to
write their textures out as PNG.

The game keeps every texture inside these packages, and a height map has to be made for each one that is worth it, so
the first thing the work needs is the textures themselves as ordinary image files, under the names the game calls them
by. That is all this does: header, name table, import and export tables, then - for a Texture export - its tagged
property list (for the format and the palette) and its mip levels.

    python scripts/upkg.py names   <package>            what the package calls things
    python scripts/upkg.py imports <package>            what it uses from elsewhere, as Package.Name
    python scripts/upkg.py list    <package>            the textures in it, with size and format
    python scripts/upkg.py export  <package> <outdir>   every texture as <outdir>/<Package>/<Name>.png
    python scripts/upkg.py export  <package> <outdir> --only NameA,NameB

Package version 68 (Deus Ex). A palette that lives in another package is looked up beside the package being read and in
the folders named by --paths.
"""
import argparse
import os
import struct
import sys

# Texture formats, as the engine numbers them.
TEXF_P8 = 0
TEXF_RGBA7 = 1
TEXF_RGB16 = 2
TEXF_DXT1 = 3
TEXF_RGB8 = 4
TEXF_RGBA8 = 5
TEXF_NAMES = {0: 'P8', 1: 'RGBA7', 2: 'RGB16', 3: 'DXT1', 4: 'RGB8', 5: 'RGBA8', 6: 'DXT3', 7: 'DXT5'}

# Property types in a tagged property list.
PROP_BYTE, PROP_INT, PROP_BOOL, PROP_FLOAT, PROP_OBJECT = 1, 2, 3, 4, 5
PROP_NAME, PROP_STRING, PROP_CLASS, PROP_ARRAY, PROP_STRUCT = 6, 7, 8, 9, 10
PROP_VECTOR, PROP_ROTATOR, PROP_STR, PROP_MAP, PROP_FIXED_ARRAY = 11, 12, 13, 14, 15


class Reader:
    """A cursor over the package bytes. The engine's own variable-length index is the only unusual part."""

    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def byte(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def int32(self):
        v = struct.unpack_from('<i', self.d, self.p)[0]
        self.p += 4
        return v

    def float32(self):
        v = struct.unpack_from('<f', self.d, self.p)[0]
        self.p += 4
        return v

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def index(self):
        """The engine's compact index: the first byte carries the sign and six bits, every further byte seven more."""
        b = self.byte()
        neg = b & 0x80
        value = b & 0x3F
        if b & 0x40:
            shift = 6
            while True:
                b = self.byte()
                value |= (b & 0x7F) << shift
                shift += 7
                if not (b & 0x80):
                    break
        return -value if neg else value

    def string(self):
        """A length byte, then the characters with their closing zero."""
        n = self.byte()
        s = self.raw(n)
        return s.split(b'\0')[0].decode('latin1')

    def zstring(self):
        """Just the characters up to a zero, with no length in front - how the older package versions store a name."""
        start = self.p
        while self.d[self.p]:
            self.p += 1
        s = self.d[start:self.p]
        self.p += 1
        return s.decode('latin1')


class Package:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        with open(path, 'rb') as f:
            self.d = f.read()
        (tag, self.version, self.licensee, self.flags, name_count, name_off,
         export_count, export_off, import_count, import_off) = struct.unpack_from('<IHHIIIIIII', self.d, 0)
        if tag != 0x9E2A83C1:
            raise ValueError('%s: not a package' % path)
        self.names = self._read_names(name_off, name_count)
        self.imports = self._read_imports(import_off, import_count)
        self.exports = self._read_exports(export_off, export_count)

    def _read_names(self, off, count):
        """The name table. Up to package version 63 a name is simply the characters and a closing zero; from 64 on a
        length comes first. A few of the game's older packages are still version 61, and reading those the new way
        yields nonsense names - and with them nonsense classes, so their textures look like nothing at all."""
        r = Reader(self.d, off)
        out = []
        for _ in range(count):
            out.append(r.string() if self.version >= 64 else r.zstring())
            r.int32()                      # the name's own flags, which nothing here needs
        return out

    def _read_imports(self, off, count):
        r = Reader(self.d, off)
        out = []
        for _ in range(count):
            class_package = self.names[r.index()]
            class_name = self.names[r.index()]
            package = r.int32()            # a reference like any other: positive export, negative import
            object_name = self.names[r.index()]
            out.append({'class_package': class_package, 'class': class_name,
                        'package': package, 'name': object_name})
        return out

    def _read_exports(self, off, count):
        r = Reader(self.d, off)
        out = []
        for _ in range(count):
            class_ref = r.index()
            r.index()                      # the parent class, which nothing here needs
            package = r.int32()
            object_name = self.names[r.index()]
            r.int32()                      # object flags
            size = r.index()
            pos = r.index() if size > 0 else 0
            out.append({'class_ref': class_ref, 'package': package, 'name': object_name,
                        'size': size, 'pos': pos})
        return out

    def ref_name(self, ref):
        """What a reference points at, by name. Positive is an export of this package, negative an import."""
        if ref > 0:
            return self.exports[ref - 1]['name']
        if ref < 0:
            return self.imports[-ref - 1]['name']
        return None

    def class_of(self, e):
        """What class an export is of. Its class_ref points at the class OBJECT, so that object's NAME is the class
        name - the class of that object would be 'Class' for every one of them."""
        return self.ref_name(e['class_ref'])

    def path_of(self, ref):
        """The full name of a reference, as Package.Group.Name - the same spelling the renderer logs."""
        parts = []
        seen = 0
        while ref != 0 and seen < 8:
            seen += 1
            if ref > 0:
                e = self.exports[ref - 1]
                parts.append(e['name'])
                ref = e['package']
            else:
                i = self.imports[-ref - 1]
                parts.append(i['name'])
                ref = i['package']
        return '.'.join(reversed(parts))

    def textures(self):
        """Every texture, including the kinds that are a texture with something added - fire, water, and the rest.
        They all end in Texture, and they all carry the same mip levels."""
        return [(i + 1, e) for i, e in enumerate(self.exports) if (self.class_of(e) or '').endswith('Texture')]

    def palettes(self):
        return [(i + 1, e) for i, e in enumerate(self.exports) if self.class_of(e) == 'Palette']

    # -- object reading -----------------------------------------------------

    def properties(self, r):
        """A tagged property list, read until the closing None. Values this job has no use for are stepped over."""
        props = {}
        while True:
            name = self.names[r.index()]
            if name == 'None':
                return props
            info = r.byte()
            ptype = info & 0x0F
            size_bits = (info >> 4) & 0x07
            struct_name = self.names[r.index()] if ptype == PROP_STRUCT else None
            size = [1, 2, 4, 12, 16, 0, 0, 0][size_bits]
            if size_bits == 5:
                size = r.byte()
            elif size_bits == 6:
                size = struct.unpack_from('<H', self.d, r.p)[0]
                r.p += 2
            elif size_bits == 7:
                size = r.int32()
            # The top bit is the array flag - except for a bool, where it IS the value and nothing follows.
            if info & 0x80 and ptype != PROP_BOOL:
                r.index()
            if ptype == PROP_BOOL:
                props[name] = bool(info & 0x80)
                continue
            at = r.p
            if ptype == PROP_BYTE:
                props[name] = r.byte()
            elif ptype == PROP_INT:
                props[name] = r.int32()
            elif ptype == PROP_FLOAT:
                props[name] = r.float32()
            elif ptype in (PROP_OBJECT, PROP_CLASS):
                props[name] = r.index()
            elif ptype == PROP_NAME:
                props[name] = self.names[r.index()]
            elif ptype in (PROP_STRING, PROP_STR):
                props[name] = r.string()
            else:
                props[name] = ('struct', struct_name) if ptype == PROP_STRUCT else ('?', ptype)
            r.p = at + size

    def _read_mips(self, r):
        count = r.index()
        mips = []
        for _ in range(count):
            # From package version 63 each mip is preceded by where its data ends - the engine's own lazy-array
            # marker. The older packages have no such marker, and reading one there swallows the data's length.
            end = r.int32() if self.version >= 63 else 0
            n = r.index()
            data = r.raw(n)
            usize = r.int32()
            vsize = r.int32()
            r.byte()
            r.byte()
            mips.append({'data': data, 'u': usize, 'v': vsize})
            if end and end > r.p:
                r.p = end
        return mips

    def read_texture(self, ref):
        """A texture's properties and its mip levels, biggest first.

        A texture can carry TWO chains: the palettised original the game shipped with, and - when bHasComp is set - a
        compressed one after it. The high resolution replacements live in that second chain, so it is the one worth
        having; the first is then only the small fallback."""
        e = self.exports[ref - 1]
        r = Reader(self.d, e['pos'])
        props = self.properties(r)
        mips = self._read_mips(r)
        comp = self._read_mips(r) if props.get('bHasComp') else []
        return props, mips, comp

    def read_palette(self, ref):
        e = self.exports[ref - 1]
        r = Reader(self.d, e['pos'])
        self.properties(r)
        count = r.index()
        return [tuple(r.raw(4)) for _ in range(count)]   # blue, green, red, alpha


def find_package(name, folders):
    for folder in folders:
        for ext in ('.utx', '.u', '.dx'):
            p = os.path.join(folder, name + ext)
            if os.path.exists(p):
                return p
    return None


def palette_of(pkg, ref, folders, cache):
    """A texture's palette, wherever it lives: in this package, or in another one beside it."""
    if ref > 0:
        return pkg.read_palette(ref)
    if ref == 0:
        return None
    imp = pkg.imports[-ref - 1]
    outer = imp['package']
    pkg_name = pkg.ref_name(outer) if outer else None
    while outer < 0 and pkg.imports[-outer - 1]['package'] != 0:
        outer = pkg.imports[-outer - 1]['package']
        pkg_name = pkg.ref_name(outer)
    if not pkg_name:
        return None
    other = cache.get(pkg_name.lower())
    if other is None:
        path = find_package(pkg_name, folders)
        if not path:
            return None
        other = Package(path)
        cache[pkg_name.lower()] = other
    for ref2, e in other.palettes():
        if e['name'].lower() == imp['name'].lower():
            return other.read_palette(ref2)
    return None


# -- decoding ---------------------------------------------------------------

def _decode_dxt(data, u, v, step, four_colour_always):
    """The colour half of a DXT block: two endpoint colours and two bits a pixel choosing between them and the two
    mixtures. Whole-array arithmetic, because these are a thousand pixels on a side and a Python loop over them takes
    minutes a texture. DXT3 and DXT5 carry alpha in the first eight bytes, which nothing here wants."""
    import numpy as np
    bu, bv = (u + 3) // 4, (v + 3) // 4
    if len(data) < bu * bv * step:
        return None
    b = np.frombuffer(data[:bu * bv * step], dtype=np.uint8).reshape(bu * bv, step)[:, step - 8:]
    c0 = b[:, 0].astype(np.uint16) | (b[:, 1].astype(np.uint16) << 8)
    c1 = b[:, 2].astype(np.uint16) | (b[:, 3].astype(np.uint16) << 8)

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], 1)

    a, d = rgb(c0).astype(np.int16), rgb(c1).astype(np.int16)
    third = (2 * a + d) // 3
    quarter = (a + 2 * d) // 3
    if not four_colour_always:
        # In DXT1 the endpoints' ORDER says whether the block has a third mixture or a see-through slot.
        one_bit = (c0 <= c1)[:, None]
        third = np.where(one_bit, (a + d) // 2, third)
        quarter = np.where(one_bit, 0, quarter)
    table = np.stack([a, d, third, quarter], 1).astype(np.uint8)          # blocks x 4 x 3
    bits = (b[:, 4].astype(np.uint32) | (b[:, 5].astype(np.uint32) << 8) |
            (b[:, 6].astype(np.uint32) << 16) | (b[:, 7].astype(np.uint32) << 24))
    idx = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None, :]) & 3
    px = np.take_along_axis(table, idx[:, :, None], axis=1)               # blocks x 16 x 3
    out = px.reshape(bv, bu, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bv * 4, bu * 4, 3)
    return out[:v, :u]


def decode(fmt, data, u, v, palette):
    """One mip level as an (v, u, 3) array of bytes."""
    import numpy as np
    if fmt == TEXF_P8:
        if not palette:
            return None
        idx = np.frombuffer(data[:u * v], dtype=np.uint8)
        if idx.size < u * v:
            return None
        pal = np.array([(c[2], c[1], c[0]) for c in palette], dtype=np.uint8)
        return pal[idx].reshape(v, u, 3)
    if fmt == TEXF_RGB8:
        return np.frombuffer(data[:u * v * 3], dtype=np.uint8).reshape(v, u, 3)
    if fmt == TEXF_RGBA8:
        a = np.frombuffer(data[:u * v * 4], dtype=np.uint8).reshape(v, u, 4)
        return a[:, :, [2, 1, 0]]
    if fmt == TEXF_DXT1:
        return _decode_dxt(data, u, v, 8, False)
    if fmt in (6, 7):
        return _decode_dxt(data, u, v, 16, True)
    return None


def best_chain(props, mips, comp):
    """Which of a texture's two chains to take, and in what format: the compressed one when it is there, because that
    is where a high resolution replacement lives."""
    if comp and (not mips or comp[0]['u'] >= mips[0]['u']):
        return comp, props.get('CompFormat', TEXF_DXT1)
    return mips, props.get('Format', TEXF_P8)


def export_textures(pkg, outdir, only=None, folders=(), mips=1):
    from PIL import Image
    cache = {}
    made = []
    folders = list(folders) + [os.path.dirname(pkg.path)]
    for ref, e in pkg.textures():
        if only and e['name'].lower() not in only:
            continue
        try:
            props, plain, comp = pkg.read_texture(ref)
        except Exception as err:                                  # a package this reader does not understand
            print('  %-28s READ FAILED (%s)' % (e['name'], err))
            continue
        levels, fmt = best_chain(props, plain, comp)
        if not levels:
            continue
        pal = palette_of(pkg, props.get('Palette', 0), folders, cache) if fmt == TEXF_P8 else None
        wrote = 0
        for i, m in enumerate(levels[:mips]):
            img = decode(fmt, m['data'], m['u'], m['v'], pal)
            if img is None:
                continue
            folder = os.path.join(outdir, pkg.name)
            os.makedirs(folder, exist_ok=True)
            suffix = '' if i == 0 else '.mip%d' % i
            path = os.path.join(folder, '%s%s.png' % (e['name'], suffix))
            Image.fromarray(img, 'RGB').save(path)
            wrote += 1
        if wrote:
            made.append((e['name'], levels[0]['u'], levels[0]['v'], TEXF_NAMES.get(fmt, str(fmt))))
        else:
            print('  %-28s NOT DECODED (%s, %d mips)' % (e['name'], TEXF_NAMES.get(fmt, fmt), len(levels)))
    return made


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=['names', 'imports', 'list', 'export'])
    ap.add_argument('package')
    ap.add_argument('outdir', nargs='?')
    ap.add_argument('--only', default='', help='comma separated texture names')
    ap.add_argument('--paths', default='', help='further folders to look for packages in, separated by ;')
    ap.add_argument('--mips', type=int, default=1)
    a = ap.parse_args()
    pkg = Package(a.package)
    folders = [f for f in a.paths.split(';') if f]
    if a.what == 'names':
        for i, n in enumerate(pkg.names):
            print('%5d %s' % (i, n))
    elif a.what == 'imports':
        for i, imp in enumerate(pkg.imports):
            print('%5d %-10s %s' % (i, imp['class'], pkg.path_of(-(i + 1))))
    elif a.what == 'list':
        for ref, e in pkg.textures():
            try:
                props, plain, comp = pkg.read_texture(ref)
                levels, fmt = best_chain(props, plain, comp)
                print('%-32s %4dx%-4d %-6s %d mips' % (e['name'], levels[0]['u'] if levels else 0,
                                                       levels[0]['v'] if levels else 0,
                                                       TEXF_NAMES.get(fmt, '?'), len(levels)))
            except Exception as err:
                print('%-32s READ FAILED (%s)' % (e['name'], err))
    else:
        if not a.outdir:
            sys.exit('export needs an output folder')
        only = set(n.strip().lower() for n in a.only.split(',') if n.strip())
        made = export_textures(pkg, a.outdir, only, folders, a.mips)
        for name, u, v, fmt in made:
            print('%-32s %4dx%-4d %s' % (name, u, v, fmt))
        print('%d textures written to %s' % (len(made), os.path.join(a.outdir, pkg.name)))


if __name__ == '__main__':
    main()
