#!/bin/bash
. "$(dirname "$0")/lib.sh"
skip_if_missing "tests/unit-host" "unit-host not built (run 'make unit-tests')"

tests/unit-host
rc=$?
assert_eq 0 "$rc" "unit-host exits 0"
t_done
