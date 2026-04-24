#!/bin/bash
# `cat` vs `cat7`: `cat7` forces the high-bit strip; `cat` (with no prior
# `strip on`) emits the raw bytes. On a DOS BAS file, those differ.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

cat_raw=$(./wozit -I dos33.dsk -d -c "cat HELLO"  2>&1)
cat_7=$(./wozit -I dos33.dsk -d -c "cat7 HELLO"   2>&1)

# Whatever the exact bytes, the two forms should differ for a high-bit file.
if [ "$cat_raw" = "$cat_7" ]; then
  printf "  FAIL: cat and cat7 output identical (expected differences)\n" >&2
  t_fail=$((t_fail + 1))
else
  printf "  PASS: cat and cat7 produce different output\n"
  t_pass=$((t_pass + 1))
fi

# cat7 output should be 7-bit-clean (every byte < 0x80 when reducing).
# We check that the number of bytes with the high bit set in cat7 is 0.
hi_bits=$(printf "%s" "$cat_7" | LC_ALL=C tr -d '\0-\177' | wc -c | tr -d ' ')
assert_eq 0 "$hi_bits" "cat7 output contains no high-bit bytes"

# `cat7 HELLO` must not be silently routed to `cat` (prefix-match bug).
# If it were, the output would equal `cat HELLO` output. Already covered
# above by the "different output" check, but pin the "file not found"
# negative case too:
assert_no_match "File not found" "$cat_7" "cat7 found the file (dispatch OK)"

t_done
