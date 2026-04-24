#!/bin/bash
. "$(dirname "$0")/lib.sh"
skip_if_missing "tests/unit-intbas" "unit-intbas not built (run 'make unit-tests')"

tests/unit-intbas
rc=$?
assert_eq 0 "$rc" "unit-intbas exits 0"
t_done
