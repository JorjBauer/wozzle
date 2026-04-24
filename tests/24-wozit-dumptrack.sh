#!/bin/bash
# `dumptrack` — extract a raw NIBTRACKSIZE-byte revolution of latched
# nibbles from a physical track. Useful for analyzing non-standard
# disk formats (rwts18, copy-protected layouts) where there's no
# filesystem to walk.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Standard DOS 3.3 track 17 has the VTOC + catalog; 16 `D5 AA 96` address
# prologues are expected in its raw nibble stream.
./wozit -I dos33.dsk -d -c "dumptrack 17 $TMP/t17.nib" >/dev/null 2>&1
assert_file_size "$TMP/t17.nib" 6656 "dumptrack writes NIBTRACKSIZE bytes"

# Count address-field prologues via rolling (not byte-aligned) search.
# Using xxd to a flat hex stream lets grep find patterns regardless of
# where the dump happens to have broken byte boundaries.
hex=$(xxd -p "$TMP/t17.nib" | tr -d '\n')
n=$(grep -oE "d5aa96" <<<"$hex" | wc -l | tr -d ' ')
if [ "$n" -ge 10 ] && [ "$n" -le 32 ]; then
  printf "  PASS: track 17 has %s D5 AA 96 prologues (expected 10..32)\n" "$n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: track 17 D5 AA 96 count %s outside expected range\n" "$n" >&2
  t_fail=$((t_fail + 1))
fi

# Bad args.
out=$(./wozit -I dos33.dsk -d -c "dumptrack" 2>&1)
assert_match "Usage"   "$out" "bare dumptrack prints usage"

out=$(./wozit -I dos33.dsk -d -c "dumptrack 99 $TMP/junk.nib" 2>&1)
assert_match "out of range" "$out" "out-of-range track rejected"

t_done
