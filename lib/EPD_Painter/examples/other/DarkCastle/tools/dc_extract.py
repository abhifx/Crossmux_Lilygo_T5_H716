#!/usr/bin/env python3
"""
Dark Castle asset extractor.

Reads an original Dark Castle 800K HFS disk image and emits the C++ asset
files used by the DarkCastle example. Everything here was reverse engineered
from the game's own 68000 code; see FORMATS.md for the derivations.

    python3 dc_extract.py /path/to/DarkCastle_1_2.dsk [--out ..] [--png dir]

Nothing from the game ships in this repository. You supply the disk image of
your own copy and this script produces the assets locally.

Pipeline
    HFS volume  ->  resource forks  ->  PSCR / PPCT resources  ->  C++ arrays

No third-party modules; modern macOS can no longer mount HFS standard, so the
volume is parsed here directly.
"""

import argparse
import os
import struct
import sys
import zlib

# --------------------------------------------------------------------------
# HFS (Macintosh Standard, not HFS+)
# --------------------------------------------------------------------------


class HFS:
    """Just enough HFS to walk the catalog and pull forks out."""

    def __init__(self, path):
        self.img = open(path, 'rb').read()
        mdb = self.img[1024:1536]
        if struct.unpack('>H', mdb[0:2])[0] != 0x4244:      # 'BD'
            raise ValueError('%s is not an HFS volume' % path)
        self.alBlkSiz = struct.unpack('>I', mdb[20:24])[0]
        self.alBlSt = struct.unpack('>H', mdb[28:30])[0]
        self.volname = mdb[37:37 + mdb[36]].decode('mac_roman')
        xt_size = struct.unpack('>I', mdb[130:134])[0]
        ct_size = struct.unpack('>I', mdb[146:150])[0]
        self.overflow = []
        self.overflow = self._parse_overflow(
            self._read(self._extrec(mdb[134:146]), xt_size))
        self.catalog = self._read(self._extrec(mdb[150:162]), ct_size, 4)

    @staticmethod
    def _extrec(b):
        return [struct.unpack('>HH', b[i * 4:i * 4 + 4]) for i in range(3)]

    def _blk(self, n):
        off = self.alBlSt * 512 + n * self.alBlkSiz
        return self.img[off:off + self.alBlkSiz]

    def _read(self, extrec, size, fileid=None, rsrc=False):
        out = bytearray()
        for start, cnt in extrec:
            for i in range(cnt):
                out += self._blk(start + i)
        if fileid is not None and len(out) < size:
            want = 0xFF if rsrc else 0x00
            for ftype, fid, startblk, ext in self.overflow:
                if fid == fileid and ftype == want and \
                        startblk * self.alBlkSiz == len(out):
                    for start, cnt in ext:
                        for i in range(cnt):
                            out += self._blk(start + i)
        return bytes(out[:size]) if size else bytes(out)

    @staticmethod
    def _records(node):
        nrecs = struct.unpack('>H', node[10:12])[0]
        n = len(node)
        offs = [struct.unpack('>H', node[n - 2 * (i + 1):n - 2 * i])[0]
                for i in range(nrecs + 1)]
        return [node[offs[i]:offs[i + 1]] for i in range(nrecs)]

    def _leaves(self, data, nodesize=512):
        for i in range(0, len(data), nodesize):
            node = data[i:i + nodesize]
            if len(node) < 14:
                return
            if node[8] == 0xFF:                              # leaf node
                for rec in self._records(node):
                    yield rec

    def _parse_overflow(self, data):
        out = []
        for rec in self._leaves(data):
            if len(rec) < 19:
                continue
            d = 1 + rec[0]
            d += d & 1
            out.append((rec[1], struct.unpack('>I', rec[2:6])[0],
                        struct.unpack('>H', rec[6:8])[0],
                        self._extrec(rec[d:d + 12])))
        return out

    def files(self):
        dirs, files = {}, []
        for rec in self._leaves(self.catalog):
            if len(rec) < 8:
                continue
            parID = struct.unpack('>I', rec[2:6])[0]
            name = rec[7:7 + rec[6]].decode('mac_roman', 'replace')
            d = 1 + rec[0]
            d += d & 1
            body = rec[d:]
            if not body:
                continue
            if body[0] == 1:                                 # directory
                dirs[struct.unpack('>I', body[6:10])[0]] = (parID, name)
            elif body[0] == 2:                               # file
                files.append({
                    'name': name,
                    'id': struct.unpack('>I', body[20:24])[0],
                    'rsrcLen': struct.unpack('>I', body[36:40])[0],
                    'rsrcExt': self._extrec(body[86:98]),
                })
        return files

    def resource_fork(self, f):
        return self._read(f['rsrcExt'], f['rsrcLen'], f['id'], True)


def parse_resources(data):
    """Resource fork -> {type: {id: bytes}}."""
    if len(data) < 16:
        return {}
    dataOff, mapOff, _, mapLen = struct.unpack('>IIII', data[:16])
    if mapOff + mapLen > len(data):
        return {}
    m = data[mapOff:mapOff + mapLen]
    typeListOff, nameListOff = struct.unpack('>HH', m[24:28])
    tl = m[typeListOff:]
    out = {}
    for i in range(struct.unpack('>H', tl[:2])[0] + 1):
        e = tl[2 + i * 8:10 + i * 8]
        if len(e) < 8:
            break
        rtype = e[:4].decode('mac_roman', 'replace')
        nrefs = struct.unpack('>H', e[4:6])[0] + 1
        refOff = struct.unpack('>H', e[6:8])[0]
        out[rtype] = {}
        for j in range(nrefs):
            r = tl[refOff + j * 12:refOff + j * 12 + 12]
            if len(r) < 12:
                break
            rid = struct.unpack('>h', r[:2])[0]
            p = dataOff + struct.unpack('>I', b'\x00' + r[5:8])[0]
            if p + 4 > len(data):
                continue
            ln = struct.unpack('>I', data[p:p + 4])[0]
            out[rtype][rid] = data[p + 4:p + 4 + ln]
    return out


# --------------------------------------------------------------------------
# The two Dark Castle bitmap codecs
# --------------------------------------------------------------------------


def unpack_pscr(src):
    """Full-screen dictionary codec. From ASMSEG1 +0x587e.

    Layout:
        +0        uint16   control stream length, in bytes
        +2..+9             run dictionary, 8 entries
        +10..+136          single dictionary, indices 1..127
        +138               control stream

    Control byte c:
        c == 0        escape: emit the next raw byte (consumes two)
        0 < c < 0x80  emit single_dict[c]
        c >= 0x80     emit run_dict[(c >> 4) & 7], (c & 0x0f) + 1 times
    """
    n = struct.unpack('>H', src[:2])[0]
    out = bytearray()
    p, left = 138, n
    while left > 0 and p < len(src):
        c = src[p]
        p += 1
        left -= 1
        if c == 0:
            out.append(src[p])
            p += 1
            left -= 1
        elif c < 0x80:
            out.append(src[9 + c])
        else:
            out += bytes((src[2 + ((c >> 4) & 7)],)) * ((c & 0x0F) + 1)
    return bytes(out)


def unpack_ppct(src, dstlen, pos=14):
    """Sprite codec: bit-oriented RLE. From ASMSEG1 +0x57c0.

    The destination starts zeroed and is filled MSB-first:
        b < 0x80        literal, the low 7 bits of b
        b == 0x80       end of stream
        0x80 < b < 0xC0 leave (b & 0x3f) + 7 bits clear
        b >= 0xC0       set  (b & 0x3f) + 7 bits
    """
    dst = bytearray(dstlen)
    bit = 0
    while pos < len(src):
        b = src[pos]
        pos += 1
        if b < 0x80:
            for i in range(6, -1, -1):
                if bit >= dstlen * 8:
                    return bytes(dst)
                if (b >> i) & 1:
                    dst[bit >> 3] |= 0x80 >> (bit & 7)
                bit += 1
        elif b == 0x80:
            break
        elif b < 0xC0:
            bit += (b & 0x3F) + 7
        else:
            for _ in range((b & 0x3F) + 7):
                if bit >= dstlen * 8:
                    return bytes(dst)
                dst[bit >> 3] |= 0x80 >> (bit & 7)
                bit += 1
    return bytes(dst)


def ppct_header(d):
    """mode, frames, widthWords, height, bytesPerPlane, hotX, hotY."""
    return struct.unpack('>7H', d[:14])


def pack_ppct(bits, nbits):
    """Encode a bitmap in the sprite codec, so the port needs one decoder.

    Inverse of unpack_ppct. Runs pay for themselves at 7 bits (8 for zeros --
    0x80 is the terminator, so a zero run's count starts at 1), anything
    shorter goes out as 7-bit literals.
    """
    def bit(i):
        return (bits[i >> 3] >> (7 - (i & 7))) & 1 if i < nbits else 0

    out = bytearray()
    i = 0
    while i < nbits:
        v = bit(i)
        run = 1
        while i + run < nbits and bit(i + run) == v and run < 70:
            run += 1
        if v and run >= 7:
            out.append(0xC0 | (run - 7))
            i += run
        elif not v and run >= 8:
            out.append(0x80 | (run - 7))
            i += run
        else:
            byte = 0
            for k in range(7):
                byte = (byte << 1) | bit(i + k)
            out.append(byte & 0x7F)
            i += 7
    out.append(0x80)
    return bytes(out)


# --------------------------------------------------------------------------
# QuickDraw PICT v1 -- the Great Hall and the status furniture are PICTs, not
# PSCRs. The loader in GreatHall +0x182 asks for PICT first and only falls back
# to PSCR, which is easy to miss.
# --------------------------------------------------------------------------

_PICT_FIXED = {
    0x00: 0, 0x1C: 0, 0x1E: 0, 0x02: 8, 0x03: 2, 0x04: 1, 0x05: 2, 0x06: 4,
    0x07: 4, 0x08: 2, 0x09: 8, 0x0A: 8, 0x0B: 4, 0x0C: 4, 0x0D: 2, 0x0E: 4,
    0x0F: 4, 0x10: 8, 0x11: 1, 0x15: 2, 0x16: 2, 0x17: 0, 0x18: 0, 0x19: 0,
    0x20: 8, 0x21: 4, 0x22: 6, 0x23: 2,
    0x30: 8, 0x31: 8, 0x32: 8, 0x33: 8, 0x34: 8,
    0x38: 0, 0x39: 0, 0x3A: 0, 0x3B: 0, 0x3C: 0,
    0x40: 8, 0x41: 8, 0x42: 8, 0x43: 8, 0x44: 8,
    0x48: 0, 0x49: 0, 0x4A: 0, 0x4B: 0, 0x4C: 0,
    0x50: 8, 0x51: 8, 0x52: 8, 0x53: 8, 0x54: 8,
    0x58: 0, 0x59: 0, 0x5A: 0, 0x5B: 0, 0x5C: 0,
    0x60: 12, 0x61: 12, 0x62: 12, 0x63: 12, 0x64: 12,
    0x68: 4, 0x69: 4, 0x6A: 4, 0x6B: 4, 0x6C: 4,
    0x70: None, 0x71: None, 0x72: None, 0x73: None, 0x74: None,
    0xA0: 2,
}


def _packbits(src, want):
    out = bytearray()
    p = 0
    while len(out) < want and p < len(src):
        n = src[p]; p += 1
        if n < 128:
            n += 1
            out += src[p:p + n]; p += n
        elif n > 128:
            out += bytes((src[p],)) * (257 - n); p += 1
    return bytes(out[:want])


def decode_pict(data):
    """Composite every band of a 1-bit PICT. Returns (w, h, rowbytes, bits).

    Big pictures are cut into horizontal bands, each its own BitsRect with a
    dstRect saying where it goes -- taking the first band alone yields a
    48-row sliver of a 342-row screen.
    """
    if len(data) < 10:
        return None
    _size, t, l, b, r = struct.unpack('>Hhhhh', data[:10])
    W, H = r - l, b - t
    if W <= 0 or H <= 0 or W > 2048 or H > 2048:
        return None
    RB = (W + 7) // 8
    canvas = bytearray(RB * H)
    drew = False
    p = 10
    while p < len(data):
        op = data[p]; p += 1
        if op == 0xFF:
            break
        if op in (0x90, 0x91, 0x98, 0x99):
            rb = struct.unpack('>H', data[p:p + 2])[0]
            isPix = (rb & 0x8000) != 0
            rb &= 0x7FFF
            p += 2
            bt, bl, bb, br = struct.unpack('>hhhh', data[p:p + 8]); p += 8
            if isPix:
                p += 36
            st, sl, _sb, _sr = struct.unpack('>hhhh', data[p:p + 8]); p += 8
            dt, dl, _db, _dr = struct.unpack('>hhhh', data[p:p + 8]); p += 8
            p += 2
            if op in (0x91, 0x99):
                p += struct.unpack('>H', data[p:p + 2])[0]
            w, h = br - bl, bb - bt
            rows = bytearray()
            if rb < 8:
                rows = bytearray(data[p:p + rb * h]); p += rb * h
            else:
                for _ in range(h):
                    if rb > 250:
                        n = struct.unpack('>H', data[p:p + 2])[0]; p += 2
                    else:
                        n = data[p]; p += 1
                    rows += _packbits(data[p:p + n], rb).ljust(rb, b'\0')
                    p += n
            shift = (dl - l) - (sl - bl)
            for y in range(h):
                dy = dt - t + y - (st - bt)
                if dy < 0 or dy >= H:
                    continue
                row = rows[y * rb:(y + 1) * rb]
                if shift == 0:
                    for i in range(min(RB, len(row))):
                        canvas[dy * RB + i] |= row[i]
                else:
                    for x in range(min(w, W)):
                        if row[x >> 3] & (0x80 >> (x & 7)):
                            X = x + shift
                            if 0 <= X < W:
                                canvas[dy * RB + (X >> 3)] |= 0x80 >> (X & 7)
            drew = True
            continue
        if op == 0x01:
            p += struct.unpack('>H', data[p:p + 2])[0]; continue
        if op == 0xA1:
            p += 2
            p += 2 + struct.unpack('>H', data[p:p + 2])[0]; continue
        if op == 0x28:
            p += 4; p += 1 + data[p]; continue
        if op in (0x29, 0x2A):
            p += 1; p += 1 + data[p]; continue
        if op == 0x2B:
            p += 2; p += 1 + data[p]; continue
        n = _PICT_FIXED.get(op)
        if n is None:
            p += struct.unpack('>H', data[p:p + 2])[0]; continue
        p += n
    return (W, H, RB, bytes(canvas)) if drew else None


# --------------------------------------------------------------------------
# PNG writer, for eyeballing what came out
# --------------------------------------------------------------------------


def write_png(path, w, h, bits, rowbytes=None):
    rowbytes = rowbytes or (w + 7) // 8
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = bits[y * rowbytes:(y + 1) * rowbytes]
        raw += bytes((~c) & 0xFF for c in row.ljust(rowbytes, b'\0'))

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack('>I', len(payload)) + body +
                struct.pack('>I', zlib.crc32(body)))

    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 1, 0, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
        + chunk(b'IEND', b''))


# --------------------------------------------------------------------------
# C++ emission
# --------------------------------------------------------------------------

BANNER = """// Generated by tools/dc_extract.py -- do not edit.
//
// Original Dark Castle artwork, still in the game's own compressed form.
// Decoded on the fly by dc_codec.h, so what sits in flash here is byte for
// byte what shipped on the disk.
"""


def c_bytes(name, data, per_line=20):
    """Emit as a string literal: far quicker to compile than a brace list."""
    out = ['static const unsigned char %s[] = {' % name]
    for i in range(0, len(data), per_line):
        out.append('  ' + ''.join('0x%02x,' % b for b in data[i:i + per_line]))
    out.append('};')
    return '\n'.join(out)


def emit(out_dir, screens, sprites):
    os.makedirs(out_dir, exist_ok=True)

    # ---- screens -------------------------------------------------------
    body = [BANNER, '#include "dc_assets.h"', '']
    for rid, blob, fmt, w, h, rb in screens:
        body.append(c_bytes('scr_%d' % rid, blob))
    body.append('\nconst DCScreen DC_SCREENS[] = {')
    for rid, blob, fmt, w, h, rb in screens:
        body.append('  { %6d, scr_%d, %6d, %d, %3d, %3d, %3d },'
                    % (rid, rid, len(blob), fmt, w, h, rb))
    body.append('};')
    body.append('const int DC_SCREEN_COUNT = %d;\n' % len(screens))
    open(os.path.join(out_dir, 'dc_gfx_screens.cpp'), 'w').write(
        '\n'.join(body))

    # ---- sprites -------------------------------------------------------
    body = [BANNER, '#include "dc_assets.h"', '']
    for rid, blob, hdr in sprites:
        body.append(c_bytes('spr_%d' % rid, blob))
    body.append('\nconst DCSprite DC_SPRITES[] = {')
    for rid, blob, hdr in sprites:
        mode, frames, ww, h, plane, hx, hy = hdr
        body.append('  { %5d, spr_%d, %6d, %d, %2d, %3d, %3d, %5d, %3d, %3d },'
                    % (rid, rid, len(blob), mode, frames, ww * 16, h,
                       plane, hx, hy))
    body.append('};')
    body.append('const int DC_SPRITE_COUNT = %d;\n' % len(sprites))
    open(os.path.join(out_dir, 'dc_gfx_sprites.cpp'), 'w').write(
        '\n'.join(body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dsk', help='Dark Castle 800K disk image')
    ap.add_argument('--out', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..'),
        help='directory to write the generated .cpp into')
    ap.add_argument('--png', help='also dump every asset as PNG here')
    args = ap.parse_args()

    vol = HFS(args.dsk)
    print('volume: %s' % vol.volname)

    res = {}
    for f in vol.files():
        if f['name'].startswith('Data '):
            for rtype, ids in parse_resources(vol.resource_fork(f)).items():
                res.setdefault(rtype, {}).update(ids)
    if 'PSCR' not in res or 'PPCT' not in res:
        sys.exit('no PSCR/PPCT resources found -- is this a Dark Castle disk?')

    screens = []
    for rid in sorted(res['PSCR']):
        blob = res['PSCR'][rid]
        bits = unpack_pscr(blob)
        if len(bits) != 512 * 342 // 8:
            print('  ! PSCR %d decoded to %d bytes, expected %d'
                  % (rid, len(bits), 512 * 342 // 8))
            continue
        screens.append((rid, blob, 0, 512, 342, 64))
        if args.png:
            os.makedirs(args.png + '/screens', exist_ok=True)
            write_png('%s/screens/%d.png' % (args.png, rid), 512, 342, bits, 64)
    print('screens from PSCR: %d' % len(screens))

    # PICTs. The Great Hall is one of these, not a PSCR -- the loader asks for
    # PICT first. They are re-encoded in the sprite codec so the port carries
    # one decoder rather than a QuickDraw interpreter.
    npict = 0
    for rid in sorted(res.get('PICT', {})):
        dec = decode_pict(res['PICT'][rid])
        if not dec:
            continue
        w, h, rb, bits = dec
        if w < 64 or h < 32:            # digits and name plates: not screens
            continue
        packed = pack_ppct(bits, rb * 8 * h)
        screens.append((rid, packed, 1, w, h, rb))
        npict += 1
        if args.png:
            os.makedirs(args.png + '/screens', exist_ok=True)
            write_png('%s/screens/pict%d.png' % (args.png, rid), w, h, bits, rb)
    print('screens from PICT: %d' % npict)

    sprites = []
    for rid in sorted(res['PPCT']):
        blob = res['PPCT'][rid]
        if len(blob) < 14:
            continue
        hdr = ppct_header(blob)
        mode, frames, ww, h, plane, hx, hy = hdr
        if not (frames and ww and h):
            continue
        planes = 2 if mode in (0, 3) else 1
        bits = unpack_ppct(blob, frames * plane * planes)
        sprites.append((rid, blob, hdr))
        if args.png:
            os.makedirs(args.png + '/sprites', exist_ok=True)
            rb, W, H = ww * 2, ww * 16 * frames, h * planes
            srb = W // 8
            sheet = bytearray(srb * H)
            for f in range(frames):
                for p in range(planes):
                    base = (f * planes + p) * plane
                    for y in range(h):
                        for xb in range(rb):
                            sheet[(p * h + y) * srb + f * rb + xb] = \
                                bits[base + y * rb + xb]
            write_png('%s/sprites/%d_%dx%d_f%d.png'
                      % (args.png, rid, ww * 16, h, frames), W, H,
                      bytes(sheet), srb)
    print('sprites: %d' % len(sprites))

    emit(args.out, screens, sprites)
    total = (sum(len(x[1]) for x in screens)
             + sum(len(b) for _, b, _ in sprites))
    print('wrote dc_gfx_screens.cpp / dc_gfx_sprites.cpp into %s (%d KB of '
          'asset data)' % (os.path.abspath(args.out), total // 1024))


if __name__ == '__main__':
    main()
