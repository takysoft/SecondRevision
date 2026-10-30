"""The level's rounding (Spherifier=<units>, src\\RevisionDX12\\LevelRound.h) checked off line, on the game's own levels:
a level's polygons are read from its package and handed to the renderer's own code (tools\\LevelRoundCheck), which
checks what it makes - no place past the cap, nothing opening between the polygons, no triangle turned over, the light
sides along their polygons, the card's copy moving places as the CPU does - and says so in a line.

    python scripts\\level_round_check.py [--cap 8] [--angle 105] [--map <name> ...]

Without --map, every level of the game. The lines go to _shots\\<date-time>-level-round-check\\summary.txt as well."""
import argparse
import datetime
import os
import struct
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import content_packages as cp  # noqa: E402
import upkg  # noqa: E402
import water_edge_survey as wes  # noqa: E402
from volume_survey import Map  # noqa: E402

TOOL = os.path.join(PROJ, 'build', 'levelroundcheck', 'LevelRoundCheck.exe')
WORK = os.path.join(PROJ, '_levelround')

PF_INVISIBLE, PF_FAKEBACKDROP, PF_TWOSIDED, PF_PORTAL, PF_MIRRORED = 0x1, 0x80, 0x100, 0x4000000, 0x8000000


def use_for(flags):
    """RdxSphere::UseFor: whether a polygon is rounded (1), and a sheet drawn from both sides (2), by its surface's
    flags (no movers in a level's file)."""
    if flags & (PF_INVISIBLE | PF_FAKEBACKDROP | PF_PORTAL | PF_MIRRORED):
        return 0
    return 1 | (2 if flags & PF_TWOSIDED else 0)


def _read_surf(r):
    s = {'texture': r.index(), 'flags': r.int32() & 0xFFFFFFFF, 'base': r.index(), 'normal': r.index(),
         'tu': r.index(), 'tv': r.index()}
    r.index()
    r.index()
    r.raw(4)
    s['actor'] = r.index()
    return s


def read_level(mapname):
    """The level's BSP polygons: each node's points, plane and surface's flags."""
    wes.read_surf = _read_surf
    m = Map(cp.resolved('.dx')[mapname])
    best = None
    for e in m.exports:
        if e['size'] <= 0 or m.class_of(e) != 'Model':
            continue
        for order in ('flags-first', 'flags-last'):
            mod = wes.read_model(m, e, order)
            if mod is not None and mod['nodes']:
                if best is None or len(mod['nodes']) > len(best['nodes']):
                    best = mod
                break
    if best is None:
        return None
    polys = []
    for n in best['nodes']:
        if n['count'] < 3:
            continue
        polys.append({'pts': wes.node_points(best, n), 'plane': n['plane'], 'flags': best['surfs'][n['surf']]['flags']})
    return polys


def run(polys, cap, angle, name):
    """The polygons through tools\\LevelRoundCheck: its line, and whether all held."""
    os.makedirs(WORK, exist_ok=True)
    src = os.path.join(WORK, name + '.in')
    pts = []
    with open(src, 'wb') as f:
        f.write(b'SPI1')
        f.write(struct.pack('<d', cap))
        for p in polys:
            pts += p['pts']
        f.write(struct.pack('<i', len(pts)))
        f.write(np.asarray(pts, dtype='<f8').tobytes())
        f.write(struct.pack('<i', len(polys)))
        first = 0
        for p in polys:
            nx, ny, nz, w = p['plane']
            f.write(struct.pack('<ii4di', first, len(p['pts']), nx, ny, nz, w, use_for(p['flags'])))
            first += len(p['pts'])
    done = subprocess.run([TOOL, src, str(angle)], capture_output=True, text=True)
    return done.stdout.strip(), done.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cap', type=float, default=8.0)
    ap.add_argument('--angle', type=float, default=105.0)
    ap.add_argument('--map', action='append', default=[])
    args = ap.parse_args()
    if not os.path.exists(TOOL):
        print('tools\\LevelRoundCheck is not built: %s' % TOOL)
        return 1
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = os.path.join(PROJ, '_shots', stamp + '-level-round-check')
    os.makedirs(out, exist_ok=True)
    maps = list(args.map) or sorted(cp.resolved('.dx').keys())
    summary, failed = [], 0
    for name in maps:
        t0 = time.time()
        try:
            polys = read_level(name)
        except Exception as e:  # a package the reader does not know: said, not hidden
            polys = None
            summary.append('%s: not read (%s)' % (name, e))
        if not polys:
            continue
        line, ok = run(polys, args.cap, args.angle, name)
        failed += 0 if ok else 1
        text = '%-34s %s %s | %.0f s' % (name, '' if ok else 'FAILED', line, time.time() - t0)
        print(text, flush=True)
        summary.append(text)
    with open(os.path.join(out, 'summary.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(summary) + '\n')
    print('%d of %d levels with a fault; written to %s' % (failed, len(maps), out))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
