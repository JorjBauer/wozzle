#!/bin/bash
. "$(dirname "$0")/lib.sh"
skip_if_missing "tests/unit-applesoft" "unit-applesoft not built (run 'make unit-tests')"

tests/unit-applesoft
rc=$?
assert_eq 0 "$rc" "unit-applesoft exits 0"
t_done
