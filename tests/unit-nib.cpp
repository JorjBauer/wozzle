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

  fprintf(stderr, "  -- unit-nib: %d passed, %d failed --\n",
          passed, failed);
  return failed ? 1 : 0;
}
