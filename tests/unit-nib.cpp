// Unit tests for the 6-and-2 nibble encoder/decoder. The property is
// round-trip: decode(encode(X)) == X for every 256-byte sector we might
// care about. The existing legacy/nib-test only exercised the all-0xFF
// pattern; this file adds zeros, alternating bits, and a sequential
// 0..255 pattern so every byte value shows up somewhere.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "nibutil.h"

static int passed = 0, failed = 0;

static bool roundtrip(const char *name, const uint8_t in[256]) {
  uint8_t nib[343];
  uint8_t out[256];
  _encode62Data(nib, (uint8_t *)in);
  if (!_decode62Data(nib, out)) {
    fprintf(stderr, "  FAIL: %s — decode rejected the encoded data\n", name);
    failed++;
    return false;
  }
  for (int i = 0; i < 256; i++) {
    if (in[i] != out[i]) {
      fprintf(stderr, "  FAIL: %s — byte %d diverged (in=%02X out=%02X)\n",
              name, i, in[i], out[i]);
      failed++;
      return false;
    }
  }
  fprintf(stderr, "  PASS: %s\n", name);
  passed++;
  return true;
}

// 5-and-3 round-trip with extra sanity on the encoded output: every byte
// must belong to the 32-value disk alphabet (high bit set, no two
// adjacent zero bits, etc. — the Apple II disk controller's constraint).
static bool roundtrip53(const char *name, const uint8_t in[256]) {
  static const uint8_t alphabet[32] = {
    0xAB, 0xAD, 0xAE, 0xAF, 0xB5, 0xB6, 0xB7, 0xBA,
    0xBB, 0xBD, 0xBE, 0xBF, 0xD6, 0xD7, 0xDA, 0xDB,
    0xDD, 0xDE, 0xDF, 0xEA, 0xEB, 0xED, 0xEE, 0xEF,
    0xF5, 0xF6, 0xF7, 0xFA, 0xFB, 0xFD, 0xFE, 0xFF };

  uint8_t enc[411];
  uint8_t out[256];
  _encode53Data(enc, in);

  // Every encoded byte must be in the disk alphabet.
  for (int i = 0; i < 411; i++) {
    bool ok = false;
    for (int j = 0; j < 32; j++) if (enc[i] == alphabet[j]) { ok = true; break; }
    if (!ok) {
      fprintf(stderr, "  FAIL: %s — encoded byte %d (%02X) not in alphabet\n",
              name, i, enc[i]);
      failed++;
      return false;
    }
  }

  if (!_decode53Data(enc, out)) {
    fprintf(stderr, "  FAIL: %s — 5&3 decode rejected encoded data\n", name);
    failed++;
    return false;
  }
  for (int i = 0; i < 256; i++) {
    if (in[i] != out[i]) {
      fprintf(stderr, "  FAIL: %s — byte %d diverged (in=%02X out=%02X)\n",
              name, i, in[i], out[i]);
      failed++;
      return false;
    }
  }
  fprintf(stderr, "  PASS: %s\n", name);
  passed++;
  return true;
}

int main(int, char **) {
  uint8_t buf[256];

  memset(buf, 0x00, 256);
  roundtrip("all zeros", buf);

  memset(buf, 0xFF, 256);
  roundtrip("all 0xFF", buf);

  memset(buf, 0x55, 256);
  roundtrip("alternating 0x55", buf);

  memset(buf, 0xAA, 256);
  roundtrip("alternating 0xAA", buf);

  for (int i = 0; i < 256; i++) buf[i] = (uint8_t)i;
  roundtrip("sequential 0..255", buf);

  // A pattern picked to exercise the 6/2 split: the low-2 bits of each
  // triplet are packed into separate auxiliary nibbles, so varying only
  // the low bits across neighbouring bytes is a good stress case.
  for (int i = 0; i < 256; i++) buf[i] = (uint8_t)((i * 31) ^ 0x5A);
  roundtrip("pseudo-random xor pattern", buf);

  // 5-and-3 round-trips: same patterns through the 13-sector codec.
  memset(buf, 0x00, 256);      roundtrip53("5&3 all zeros", buf);
  memset(buf, 0xFF, 256);      roundtrip53("5&3 all 0xFF", buf);
  memset(buf, 0x55, 256);      roundtrip53("5&3 alternating 0x55", buf);
  memset(buf, 0xAA, 256);      roundtrip53("5&3 alternating 0xAA", buf);
  for (int i = 0; i < 256; i++) buf[i] = (uint8_t)i;
  roundtrip53("5&3 sequential 0..255", buf);
  // Specifically stress the low-3-bit packing: values whose low bits
  // cycle through all 8 combinations.
  for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(((i * 13) ^ 0x3C) & 0xFF);
  roundtrip53("5&3 low-bit-varying pattern", buf);

  fprintf(stderr, "  -- unit-nib: %d passed, %d failed --\n",
          passed, failed);
  return failed ? 1 : 0;
}
