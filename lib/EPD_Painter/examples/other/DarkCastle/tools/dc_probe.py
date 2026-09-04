#!/usr/bin/env python3
"""
Room geometry prober -- an authoring aid, not part of the build.

Dark Castle keeps no collision data: the original blitter drew straight to the
screen and the game's floors live only in the artwork. This walks a room's
bitmap and reports where the solid masses start, which is enough to author the
Platform tables in dc_rooms.cpp by hand.

    python3 dc_probe.py <disk image> <room id> [--map]

"Solid" is a density test rather than a plain black test, because the walls are
drawn as 50% dither and would otherwise read as floor, while the cave rooms use
large genuinely solid black masses as scenery.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dc_extract import HFS, parse_resources, unpack_pscr   # noqa: E402

CELL = 4
THRESH = 13          # of CELL*CELL pixels


def load_room(dsk, rid):
    vol = HFS(dsk)
    for f in vol.files():
        if not f['name'].startswith('Data '):
            continue
        res = parse_resources(vol.resource_fork(f))
        if 'PSCR' in res and rid in res['PSCR']:
            return unpack_pscr(res['PSCR'][rid])
    return None


def px(b, x, y):
    if x < 0 or y < 0 or x >= 512 or y >= 310:
        return 0
    return (b[y * 64 + (x >> 3)] >> (7 - (x & 7))) & 1


def solid_grid(b):
    gw, gh = 512 // CELL, 310 // CELL
    g = [[0] * gw for _ in range(gh)]
    for gy in range(gh):
        for gx in range(gw):
            c = sum(px(b, gx * CELL + i, gy * CELL + j)
                    for i in range(CELL) for j in range(CELL))
            g[gy][gx] = 1 if c >= THRESH else 0
    return g


def surfaces(g, min_run=3):
    """Top edges of solid masses, merged into horizontal spans."""
    gh, gw = len(g), len(g[0])
    spans = []
    for gy in range(1, gh):
        run = None
        for gx in range(gw):
            top = g[gy][gx] and not g[gy - 1][gx]
            # require some depth, so single-pixel detail is not a floor
            if top and gy + 1 < gh:
                top = g[gy + 1][gx]
            if top:
                run = [gx, gx] if run is None else [run[0], gx]
            else:
                if run and run[1] - run[0] + 1 >= min_run:
                    spans.append((gy, run[0], run[1]))
                run = None
        if run and run[1] - run[0] + 1 >= min_run:
            spans.append((gy, run[0], run[1]))
    return spans


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dsk')
    ap.add_argument('room', type=int)
    ap.add_argument('--map', action='store_true',
                    help='print the solidity grid as ASCII')
    args = ap.parse_args()

    b = load_room(args.dsk, args.room)
    if b is None:
        sys.exit('room %d not found' % args.room)
    g = solid_grid(b)

    if args.map:
        for gy, row in enumerate(g):
            print('%3d %s' % (gy * CELL, ''.join('#' if v else '.' for v in row)))
        print('    ' + ''.join(str((x * CELL // 10) % 10)
                               for x in range(len(g[0]))))
        return

    print('room %d -- candidate surfaces, in Mac pixels' % args.room)
    print('  %-6s %-6s %-6s %s' % ('y', 'x0', 'x1', 'width'))
    for gy, gx0, gx1 in surfaces(g):
        y, x0, x1 = gy * CELL, gx0 * CELL, gx1 * CELL + CELL - 1
        print('  %-6d %-6d %-6d %d' % (y, x0, x1, x1 - x0 + 1))


if __name__ == '__main__':
    main()
