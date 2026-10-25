#!/bin/bash
# DOS 3.3 reads against CiderPress2's test image and its expected values.
# The image lives in a CiderPress2 source tree, if there's one next to us
# or in reference/.
. "$(dirname "$0")/lib.sh"
skip_if_missing "tests/unit-dosref" "unit-dosref not built (run 'make unit-tests')"
img=$(ls CiderPress2-*/TestData/dos33/simple-sparse.do reference/CiderPress2-*/TestData/dos33/simple-sparse.do 2>/dev/null | head -1)
skip_if_missing "$img" "CiderPress2's TestData/dos33/simple-sparse.do not found"

tests/unit-dosref "$img"
rc=$?
assert_eq 0 "$rc" "unit-dosref exits 0"
t_done
