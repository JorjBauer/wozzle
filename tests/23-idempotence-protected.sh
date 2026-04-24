#!/bin/bash
# Copy-protected / half-track images can't round-trip through DSK (the
# sector-based format can't represent their quirks), but the WOZ format
# itself should reproduce them losslessly. The property to check:
# re-encoding an already-WOZ-2 image must produce bit-identical output.
# A second pass then does the same. This is stronger than hashing against
# a committed value — it's self-consistency, so encoder format changes
# don't break the test as long as the encoder remains deterministic and
# idempotent.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozzle" "wozzle not built"
BL="woz test images/WOZ 2.0/The Bilestoad - Disk 1, Side A.woz"
skip_if_missing "$BL" "Bilestoad WOZ 2.0 missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

./wozzle -i "$BL"           -o "$TMP/a.woz" >/dev/null 2>&1
./wozzle -i "$TMP/a.woz"    -o "$TMP/b.woz" >/dev/null 2>&1
if cmp -s "$TMP/a.woz" "$TMP/b.woz"; then
  printf "  PASS: WOZ 2 → WOZ 2 is idempotent (Bilestoad)\n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: re-encoding WOZ 2 changed output (Bilestoad)\n" >&2
  t_fail=$((t_fail + 1))
fi

# Same property with the -s (smaller memory footprint) mode.
./wozzle -s -i "$BL"         -o "$TMP/sa.woz" >/dev/null 2>&1
./wozzle -s -i "$TMP/sa.woz" -o "$TMP/sb.woz" >/dev/null 2>&1
if cmp -s "$TMP/sa.woz" "$TMP/sb.woz"; then
  printf "  PASS: -s (small-footprint) mode is also idempotent\n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: -s mode re-encoding changed output\n" >&2
  t_fail=$((t_fail + 1))
fi

# The two modes should produce the same file — they're implementation
# variants, not different encoding strategies.
if cmp -s "$TMP/a.woz" "$TMP/sa.woz"; then
  printf "  PASS: normal and -s modes produce identical WOZ output\n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: normal and -s modes diverge\n" >&2
  t_fail=$((t_fail + 1))
fi

t_done
