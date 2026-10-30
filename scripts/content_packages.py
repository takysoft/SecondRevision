"""The game's packages as the engine resolves them, and a reader for the objects in them - shared by content_survey.py
and content_query.py.

A feature of the renderer decides by what a thing IS - a texture's name, a surface's flags, an actor's class - and such
a rule has to be checked against everything the game holds, not against the scene it was written on. The engine looks
a package up along the search path of Revision.ini ([Core.System] Paths=), and the first folder that has it wins: a
texture package of New Vision hides the original of the same name. This module walks the same path.
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upkg  # noqa: E402

import game_path

GAME = game_path.GAME_ROOT
PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The survey's output: working data, never part of the repository (.gitignore keeps every /_*/ folder out).
OUT = os.path.join(PROJECT, '_content')

REV = os.path.join(GAME, 'Revision')
# In the order of [Core.System] Paths= in Revision\System\Revision.ini.
SEARCH = [
    (os.path.join(REV, 'Maps'), '.dx'),
    (os.path.join(REV, 'Textures'), '.utx'),
    (os.path.join(REV, 'System'), '.u'),
    (os.path.join(REV, 'NewVision', 'Textures'), '.utx'),
    (os.path.join(REV, 'HDTP', 'Textures'), '.utx'),
    (os.path.join(REV, 'HDTP', 'System'), '.u'),
    (os.path.join(GAME, 'Textures'), '.utx'),
    (os.path.join(GAME, 'Maps'), '.dx'),
    (os.path.join(GAME, 'System'), '.u'),
]

RF_HAS_STACK = 0x02000000


def out_path(*parts):
    os.makedirs(OUT, exist_ok=True)
    return os.path.join(OUT, *parts)


def resolved(ext=None):
    """{lower name: path}: of each package, the file the engine loads."""
    found = {}
    for folder, e in SEARCH:
        if ext and e != ext:
            continue
        for p in sorted(glob.glob(os.path.join(folder, '*' + e))):
            found.setdefault(os.path.splitext(os.path.basename(p))[0].lower(), p)
    return found


def hidden(ext):
    """[(name, the file loaded, [the files it hides])] for the packages that are in more than one folder."""
    seen = {}
    for folder, e in SEARCH:
        if e != ext:
            continue
        for p in sorted(glob.glob(os.path.join(folder, '*' + e))):
            seen.setdefault(os.path.splitext(os.path.basename(p))[0].lower(), []).append(p)
    return [(n, ps[0], ps[1:]) for n, ps in sorted(seen.items()) if len(ps) > 1]


class Pkg(upkg.Package):
    """A package whose exports keep their object flags and their parent, which upkg.Package has no use for."""

    def _read_exports(self, off, count):
        r = upkg.Reader(self.d, off)
        exports = []
        for _ in range(count):
            class_ref = r.index()
            super_ref = r.index()
            package = r.int32()
            object_name = self.names[r.index()]
            flags = r.int32() & 0xFFFFFFFF
            size = r.index()
            pos = r.index() if size > 0 else 0
            exports.append({'class_ref': class_ref, 'super_ref': super_ref, 'package': package, 'name': object_name,
                            'flags': flags, 'size': size, 'pos': pos})
        return exports

    def export_path(self, index1):
        """An export's full name as the renderer logs it: Package.Group.Name."""
        return self.name + '.' + self.path_of(index1)

    def object_path(self, ref):
        """A reference's full name; an export of this package gets the package's own name in front."""
        if not ref:
            return None
        return self.export_path(ref) if ref > 0 else self.path_of(ref)


class Unreadable(Exception):
    """A tagged property list that does not read as one."""


def read_props(p, r, end):
    """A tagged property list with its values: numbers, names as 'name:X', object references as 'obj:Path', a vector
    or a rotator as three numbers, a colour as four. An array's element i is keyed 'Name[i]'."""
    props = {}
    count = 0
    while True:
        if r.p >= end:
            raise Unreadable()
        i = r.index()
        if i < 0 or i >= len(p.names):
            raise Unreadable()
        name = p.names[i]
        if name == 'None':
            return props
        count += 1
        if count > 400 or not name or not all(c.isalnum() or c == '_' for c in name):
            raise Unreadable()
        info = r.byte()
        ptype = info & 0x0F
        if ptype == 0:
            raise Unreadable()
        size_bits = (info >> 4) & 7
        struct_name = None
        if ptype == upkg.PROP_STRUCT:
            si = r.index()
            if si < 0 or si >= len(p.names):
                raise Unreadable()
            struct_name = p.names[si]
        size = [1, 2, 4, 12, 16, 0, 0, 0][size_bits]
        if size_bits == 5:
            size = r.byte()
        elif size_bits == 6:
            size = struct.unpack_from('<H', p.d, r.p)[0]
            r.p += 2
        elif size_bits == 7:
            size = r.int32()
        if size < 0 or size > 1 << 20:
            raise Unreadable()
        element = 0
        # The top bit is the array flag - except for a bool, where it IS the value and nothing follows.
        if info & 0x80 and ptype != upkg.PROP_BOOL:
            b = r.byte()
            if b < 128:
                element = b
            elif (b & 0xC0) == 0x80:
                element = ((b & 0x7F) << 8) | r.byte()
            else:
                element = ((b & 0x3F) << 24) | (r.byte() << 16) | (r.byte() << 8) | r.byte()
        key = name if element == 0 else '%s[%d]' % (name, element)
        if ptype == upkg.PROP_BOOL:
            props[key] = bool(info & 0x80)
            continue
        at = r.p
        if at + size > end:
            raise Unreadable()
        if ptype == upkg.PROP_BYTE and size == 1:
            props[key] = r.byte()
        elif ptype == upkg.PROP_INT and size == 4:
            props[key] = r.int32()
        elif ptype == upkg.PROP_FLOAT and size == 4:
            props[key] = round(r.float32(), 5)
        elif ptype in (upkg.PROP_OBJECT, upkg.PROP_CLASS):
            ref = r.index()
            if ref > len(p.exports) or -ref > len(p.imports):
                raise Unreadable()
            props[key] = ('obj:' + p.object_path(ref)) if ref else None
        elif ptype == upkg.PROP_NAME:
            ni = r.index()
            if ni < 0 or ni >= len(p.names):
                raise Unreadable()
            props[key] = 'name:' + p.names[ni]
        elif ptype == upkg.PROP_STRUCT and struct_name in ('Vector', 'Rotator') and size == 12:
            fmt = '<fff' if struct_name == 'Vector' else '<iii'
            props[key] = [round(v, 3) if isinstance(v, float) else v for v in struct.unpack_from(fmt, p.d, at)]
        elif ptype == upkg.PROP_STRUCT and struct_name == 'Color' and size == 4:
            props[key] = list(p.d[at:at + 4])
        else:
            props[key] = '<%s %d bytes>' % (struct_name or ptype, size)
        r.p = at + size


def actor_props(p, e):
    """An export's own properties, read past its script state where it carries one; None where they do not read."""
    r = upkg.Reader(p.d, e['pos'])
    end = e['pos'] + e['size']
    try:
        if e['flags'] & RF_HAS_STACK:
            node = r.index()
            r.index()                  # the state node
            r.raw(8)                   # the probe mask
            r.int32()                  # the latent action
            if node != 0:
                r.index()              # where in the state's code it stands
        return read_props(p, r, end)
    except (Unreadable, IndexError, ValueError, struct.error):
        return None
