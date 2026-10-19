#!/usr/bin/env python
"""
Byte-safe UE1 ini editor.

Why it is needed: Windows' `sed -i` under git-bash rewrites the line endings of
the WHOLE file from CRLF to LF. On a UE1 ini that is a silent change to 800+
lines for the sake of setting a single key. This script replaces only that one
line of bytes, every other byte stays as it was.

The key can be given with its section too, if it appears in several sections:
    Section.Name/Key      e.g.  WinDrvLite.WindowsClientLite/WindowedViewportX

Usage:
    python ini.py get  <ini> <key>
    python ini.py set  <ini> <key> <value>        # + backup: <project>/_ini-backups/<ini>.bak-<timestamp>
"""
import sys, os, time, re


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def find_line(data, key):
    section = None
    if '/' in key:
        section, key = key.rsplit('/', 1)

    scope = data
    offset = 0
    if section is not None:
        head = re.search(rb'^\[' + re.escape(section.encode()) + rb'\]\r?\n', data, re.M)
        if not head:
            sys.exit("ERROR: no such section: [%s]" % section)
        offset = head.end()
        nxt = re.search(rb'^\[', data[offset:], re.M)
        end = offset + nxt.start() if nxt else len(data)
        scope = data[offset:end]

    pat = re.compile(rb'^' + re.escape(key.encode()) + rb'=(.*?)(\r?\n)', re.M)
    hits = list(pat.finditer(scope))
    if len(hits) != 1:
        extra = "" if section else "  (try it with a section: Section.Name/%s)" % key
        sys.exit("ERROR: '%s' %d hits (exactly 1 is needed)%s" % (key, len(hits), extra))

    m = hits[0]
    if offset:
        # the match positions have to be shifted back to the whole file
        class Shifted(object):
            def __init__(self, m, off):
                self._m, self._off = m, off

            def group(self, i):
                return self._m.group(i)

            def start(self, i=0):
                return self._m.start(i) + self._off

            def end(self, i=0):
                return self._m.end(i) + self._off
        return Shifted(m, offset)
    return m


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    op, path, key = sys.argv[1], sys.argv[2], sys.argv[3]
    data = read(path)
    m = find_line(data, key)

    if op == 'get':
        print(m.group(1).decode('latin-1'))
        return

    if op != 'set' or len(sys.argv) < 5:
        sys.exit(__doc__)

    value = sys.argv[4].encode('latin-1')
    old = m.group(1)
    if old == value:
        print("unchanged: %s=%s" % (key, value.decode('latin-1')))
        return

    # The backup goes into the project folder, not next to the ini: in the game's System folder 64
    # Revision.ini.bak-* piled up in 3 days (every renderer switch makes two: there and back).
    bak_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '_ini-backups')
    os.makedirs(bak_dir, exist_ok=True)
    bak = os.path.join(bak_dir, "%s.bak-%s" % (os.path.basename(path), time.strftime('%Y%m%d-%H%M%S')))
    with open(bak, 'wb') as f:
        f.write(data)

    out = data[:m.start(1)] + value + data[m.end(1):]
    with open(path, 'wb') as f:
        f.write(out)

    print("%s: %s -> %s" % (key, old.decode('latin-1'), value.decode('latin-1')))
    print("backup: %s" % os.path.basename(bak))
    print("line endings unchanged: CRLF %d -> %d | LF %d -> %d"
          % (data.count(b'\r\n'), out.count(b'\r\n'), data.count(b'\n'), out.count(b'\n')))


if __name__ == '__main__':
    main()
