// ============================================================================
// dc_codec.h -- the two Dark Castle bitmap codecs.
//
// Both were reverse engineered from the game's own ASMSEG1 code resource and
// are reproduced here so the assets can stay in flash exactly as they shipped.
// Decoding a full screen costs about 22 KB of output and runs in well under a
// millisecond, so rooms are unpacked on entry rather than held decoded.
//
// See FORMATS.md for where each opcode came from.
// ============================================================================

#pragma once
#include <stdint.h>
#include <string.h>
#include "dc_assets.h"

namespace dc {

// ---------------------------------------------------------------------------
// Full screens: a two-table dictionary coder. ASMSEG1 +0x587e.
//
//   +0        uint16  control stream length in bytes
//   +2..+9            run dictionary, 8 entries
//   +10..+136         single dictionary, indices 1..127
//   +138              control stream
//
// The original walked a counter of control bytes rather than tracking output
// length, and the escape case decremented it twice because it consumes two.
// Returns bytes written.
// ---------------------------------------------------------------------------
inline int unpackScreen(const uint8_t *src, int srcLen,
                        uint8_t *dst, int dstLen) {
  if (srcLen < 139) return 0;
  int left = (src[0] << 8) | src[1];
  int p = 138, o = 0;
  while (left > 0 && p < srcLen && o < dstLen) {
    const uint8_t c = src[p++];
    left--;
    if (c == 0) {                       // escape: one raw byte follows
      if (p >= srcLen) break;
      dst[o++] = src[p++];
      left--;
    } else if (c < 0x80) {              // one byte from the big dictionary
      dst[o++] = src[9 + c];
    } else {                            // a run of one of the common eight
      int n = (c & 0x0F) + 1;
      const uint8_t v = src[2 + ((c >> 4) & 7)];
      if (o + n > dstLen) n = dstLen - o;
      memset(dst + o, v, n);
      o += n;
    }
  }
  return o;
}

// ---------------------------------------------------------------------------
// Sprites: bit oriented RLE. ASMSEG1 +0x57c0.
//
//   b <  0x80        literal, the low 7 bits, written MSB first
//   b == 0x80        end of stream
//   0x80 < b < 0xC0  leave (b & 0x3f) + 7 bits clear
//   b >= 0xC0        set   (b & 0x3f) + 7 bits
//
// The destination must start zeroed -- the original cleared it up front and
// only ever ORs, which is what makes the skip opcode free.
// ---------------------------------------------------------------------------
inline void unpackSprite(const uint8_t *src, int srcLen,
                         uint8_t *dst, int dstLen, int pos = 14) {
  memset(dst, 0, dstLen);
  const int nbits = dstLen * 8;
  int bit = 0;
  while (pos < srcLen && bit < nbits) {
    const uint8_t b = src[pos++];
    if (b < 0x80) {
      for (int i = 6; i >= 0 && bit < nbits; i--, bit++)
        if ((b >> i) & 1) dst[bit >> 3] |= 0x80 >> (bit & 7);
    } else if (b == 0x80) {
      return;
    } else if (b < 0xC0) {
      bit += (b & 0x3F) + 7;
    } else {
      int n = (b & 0x3F) + 7;
      // Whole bytes in the middle of a long run are worth the special case:
      // ground and wall spans in these sprites are frequently 40+ bits.
      while (n > 0 && (bit & 7) && bit < nbits) {
        dst[bit >> 3] |= 0x80 >> (bit & 7);
        bit++; n--;
      }
      while (n >= 8 && bit + 8 <= nbits) {
        dst[bit >> 3] = 0xFF;
        bit += 8; n -= 8;
      }
      while (n > 0 && bit < nbits) {
        dst[bit >> 3] |= 0x80 >> (bit & 7);
        bit++; n--;
      }
    }
  }
}

// Bytes a sprite needs once unpacked.
inline int spriteBytes(const DCSprite &s) {
  const int planes = (s.mode == 0 || s.mode == 3) ? 2 : 1;
  return s.frames * s.plane * planes;
}

inline bool spriteHasMask(const DCSprite &s) {
  return s.mode == 0 || s.mode == 3;
}

}  // namespace dc
