#!/bin/bash
# cpin -B / cpout -B: Applesoft tokenize and detokenize.
#
# These assertions look at the tokenized BYTES, not just at the text that
# comes back out of the lister. That matters: the lister walks the program
# sequentially and ignores each line's next-line link, so a tokenizer that
# writes garbage links still produces a perfect-looking listing while
# handing a real Apple II a program it can't run.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Dump a file from an image as a lowercase hex string.
img_hex() { # <image> <mode> <name>
  ./wozit -I "$1" "$2" -c "cpout $3 $TMP/hex.bin" >/dev/null 2>&1
  hexdump -ve '1/1 "%02x"' "$TMP/hex.bin"
}

./wozit -I dos33.dsk -p -c "format $TMP/v.po WOZZLETEST" >/dev/null 2>&1
skip_if_missing "$TMP/v.po" "format did not produce an image"

# ---------------------------------------------------------------- basics

cat > "$TMP/src.txt" <<'EOF'
10 PRINT "HI"
20 END
EOF

cp "$TMP/v.po" "$TMP/a.po"
out=$(./wozit -I "$TMP/a.po" -p -c "cpin -B $TMP/src.txt BASPROG" -c "save $TMP/a.po" 2>&1)
assert_no_match "ERROR" "$out" "cpin -B did not error"
assert_no_match "DEBUG" "$out" "cpin -B left no debug output behind"

# Byte for byte, with the program image based at $0801:
#   0b08  line 10's link -> line 20, which lands at $080b
#   0a00  line number 10
#   ba 22 4849 22 00        PRINT "HI", end of line
#   1108  line 20's link -> the end marker at $0811
#   1400  line number 20
#   80 00                   END, end of line
#   0000  end of program
assert_eq "0b080a00ba22484922001108140080000000" \
          "$(img_hex "$TMP/a.po" -p BASPROG)" \
          "tokenized bytes match Applesoft's own layout"

# The links are absolute load addresses, not file offsets. A file-offset
# tokenizer would put 0a00 here (line 20 is 10 bytes in) and the program
# would jump into zero page on a real Apple II.
assert_regex "^0b08" "$(img_hex "$TMP/a.po" -p BASPROG)" \
             "first line's link is a load address, not a file offset"

# ProDOS records an Applesoft program's load address in the aux type.
assert_match "BAS" "$(./wozit -I "$TMP/a.po" -p -c ls 2>&1)" "file type is BAS"
assert_match '[$0801]' "$(./wozit -I "$TMP/a.po" -p -c ls 2>&1)" "aux type is \$0801"

# ------------------------------------------------------- listing it back

./wozit -I "$TMP/a.po" -p -c "cpout -B BASPROG $TMP/out.txt" >/dev/null 2>&1
listing=$(cat "$TMP/out.txt")
assert_match "PRINT" "$listing" "cpout -B produced PRINT"
assert_match "HI"    "$listing" "string preserved"
assert_match "END"   "$listing" "final line is present"
assert_eq 2 "$(wc -l < "$TMP/out.txt" | tr -d ' ')" "both lines listed"

# ------------------------------------------------- context-sensitive text

cat > "$TMP/ctx.txt" <<'EOF'
10 REM COPY AND PASTE
20 PR#3 : IN#0
30 PRINT SCRN(1,2), TAB(5); SPC(3)
40 DATA ONE,TWO,THREE
50 DEF FN A(X) = X ^ 2
60 FOR I = A TO B: PRINT ATN(I): NEXT I
70 HLIN 1,2 AT 3
80 ? "SHORTHAND"
EOF
cp "$TMP/v.po" "$TMP/c.po"
./wozit -I "$TMP/c.po" -p -c "cpin -B $TMP/ctx.txt CTX" -c "save $TMP/c.po" >/dev/null 2>&1
hex=$(img_hex "$TMP/c.po" -p CTX)

# REM and DATA bodies are literal text; keyword substitution must not run
# inside them or "AND" becomes $cd and "ONE" becomes $b4 "E".
assert_match "b2434f505920414e442050415354" "$hex" "REM body is literal, AND not tokenized"
assert_match "834f4e452c54574f2c5448524545" "$hex" "DATA body is literal, ONE not tokenized"

# Keywords whose spelling contains punctuation, which a cosmetic
# display table ("PR #", "SCRN (") would never match.
assert_match "8a333a8b30" "$hex" "PR# and IN# tokenized"
assert_match "d7"         "$hex" "SCRN( tokenized"
assert_match "c035293b"   "$hex" "TAB( tokenized"
assert_match "c333"       "$hex" "SPC( tokenized"
assert_match "b8c2"       "$hex" "DEF and FN are separate tokens"
assert_match "cc"         "$hex" "^ is token \$cc"
assert_match "e1"         "$hex" "ATN tokenized (not AT followed by N)"
assert_match "41c142"     "$hex" "A TO B is TO, not AT"
assert_match "c533"       "$hex" "AT tokenized"
assert_match "ba2253484f525448414e4422" "$hex" "? is shorthand for PRINT"

# ------------------------------------------------------------ round trip

# Detokenizing and re-tokenizing must be a fixed point. Applesoft discards
# whitespace outside quotes, so the cosmetic spaces LIST prints don't
# accumulate in the program on each trip through.
./wozit -I "$TMP/c.po" -p -c "cpout -B CTX $TMP/ctx.list" >/dev/null 2>&1
./wozit -I "$TMP/c.po" -p -c "cpin -B $TMP/ctx.list CTX2" -c "save $TMP/c.po" >/dev/null 2>&1
assert_eq "$hex" "$(img_hex "$TMP/c.po" -p CTX2)" \
          "cpout -B | cpin -B is a fixed point"

# --------------------------------------------------------------- DOS 3.3

# The same program image, behind DOS 3.3's 2-byte length header.
./wozit -I dos33.dsk -d -c "format $TMP/d.dsk 254" >/dev/null 2>&1
out=$(./wozit -I "$TMP/d.dsk" -d -c "cpin -B $TMP/ctx.txt CTX" -c "save $TMP/d.dsk" 2>&1)
assert_no_match "ERROR" "$out" "cpin -B works on a DOS 3.3 image"
assert_regex " A +0*2 CTX" "$(./wozit -I "$TMP/d.dsk" -d -c ls 2>&1)" \
             "DOS catalog shows an 'A' file of 2 sectors"

doshex=$(img_hex "$TMP/d.dsk" -d CTX)
proglen=$(( ${#hex} / 2 ))
printf -v lenhdr "%02x%02x" $(( proglen & 255 )) $(( proglen >> 8 ))
assert_eq "${lenhdr}${hex}" "$doshex" \
          "DOS 3.3 file is the same program behind a 2-byte length header"

# --------------------------------------------------------- bad input

for bad in "10 PRINT 1
NOTALINE" "10 PRINT 1
70000 END" "100 PRINT 1
50 PRINT 2" "10 PRINT 1
10 PRINT 2"; do
  printf '%s\n' "$bad" > "$TMP/bad.txt"
  out=$(./wozit -I "$TMP/v.po" -p -c "cpin -B $TMP/bad.txt BAD" 2>&1)
  assert_match "ERROR: source line 2" "$out" \
               "rejects: $(printf '%s' "$bad" | tail -1)"
done

# Integer BASIC can be listed but not yet tokenized; say so rather than
# quietly writing an Applesoft file under an Integer type.
out=$(./wozit -I "$TMP/v.po" -p -c "cpin -B $TMP/src.txt INTPROG FA" 2>&1)
assert_match "can't tokenize Integer BASIC" "$out" "cpin -B rejects an Integer type"

t_done
