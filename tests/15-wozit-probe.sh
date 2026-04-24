#!/bin/bash
# Filesystem-mode mismatch probe. If wozit is invoked with -d on a ProDOS
# image (or -p on a DOS image), it should print a warning on stdout.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"        "wozit not built"
skip_if_missing "dos33.dsk"      "dos33.dsk fixture missing"
skip_if_missing "ProDOS_2_4.po"  "ProDOS_2_4.po fixture missing"

# DOS invocation on a ProDOS image → warn.
out1=$(./wozit -I ProDOS_2_4.po -d -c "ls" 2>&1)
assert_match "WARNING"       "$out1" "DOS-on-ProDOS warns"
assert_match "ProDOS image"  "$out1" "warning identifies image as ProDOS"

# ProDOS invocation on a DOS image → warn.
out2=$(./wozit -I dos33.dsk -p -c "ls" 2>&1)
assert_match "WARNING"       "$out2" "ProDOS-on-DOS warns"
assert_match "DOS 3.3 image" "$out2" "warning identifies image as DOS 3.3"

# Correct modes: no warning.
out3=$(./wozit -I dos33.dsk -d -c "ls" 2>&1)
assert_no_match "WARNING" "$out3" "DOS-on-DOS does not warn"

out4=$(./wozit -I ProDOS_2_4.po -p -c "ls" 2>&1)
assert_no_match "WARNING" "$out4" "ProDOS-on-ProDOS does not warn"

t_done
