#!/bin/bash
. "$(dirname "$0")/lib.sh"
skip_if_missing "tests/unit-nib" "unit-nib not built (run 'make unit-tests')"

tests/unit-nib
rc=$?
assert_eq 0 "$rc" "unit-nib exits 0"
t_done
