#!/bin/bash
. "$(dirname "$0")/lib.sh"
skip_if_missing "tests/unit-capi" "unit-capi not built (run 'make unit-tests')"

tests/unit-capi
rc=$?
assert_eq 0 "$rc" "unit-capi exits 0"
t_done
