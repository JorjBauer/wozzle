#!/bin/bash
# Command-dispatch word-boundary check. Before the fix, `performCommand`
# used `strncmp(name, cmd, strlen(name))`, so "cat7 FOO" would match the
# "cat" command first and forward a mangled argument. Regression-proof it.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

# "cat7 HELLO" must not degrade into "cat" with a garbled argument that
# findFileByName would reject as "File not found".
out=$(./wozit -I dos33.dsk -d -c "cat7 HELLO" 2>&1)
assert_no_match "File not found" "$out" "cat7 HELLO dispatches correctly"

# "cat HELLO" still works as cat.
out2=$(./wozit -I dos33.dsk -d -c "cat HELLO" 2>&1)
assert_no_match "File not found"       "$out2" "cat HELLO still dispatches"
assert_no_match "Unrecognized command" "$out2" "cat HELLO is recognized"

# Bogus command with a known prefix still errors out.
out3=$(./wozit -I dos33.dsk -d -c "catnope" 2>&1)
assert_match "Unrecognized command" "$out3" "catnope is not silently caught by cat"

t_done
