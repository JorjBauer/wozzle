# Shared helpers for wozzle tests. Source at the top of each test:
#   . "$(dirname "$0")/lib.sh"
# Scripts are run from the project root by run-tests.sh, so relative paths
# like ./wozit and "woz test images/..." work as-is.

# Portable MD5 of a file. macOS ships `md5 -q`; Linux ships `md5sum`.
# Falls back to `shasum` (SHA-1) as a deterministic hash if nothing else
# is available — tests should treat the output as opaque.
hash_file() {
  if command -v md5sum >/dev/null 2>&1; then
    md5sum "$1" | cut -f1 -d' '
  elif command -v md5 >/dev/null 2>&1; then
    md5 -q "$1"
  elif command -v shasum >/dev/null 2>&1; then
    shasum "$1" | cut -f1 -d' '
  else
    echo "no hash tool (md5sum/md5/shasum) found" >&2
    return 1
  fi
}

# Assertion counters, scoped per test script.
t_pass=0
t_fail=0

assert_eq() {
  # assert_eq <expected> <actual> <description>
  if [ "$1" = "$2" ]; then
    printf "  PASS: %s\n" "$3"
    t_pass=$((t_pass + 1))
  else
    printf "  FAIL: %s\n    expected: %s\n    actual:   %s\n" "$3" "$1" "$2" >&2
    t_fail=$((t_fail + 1))
  fi
}

assert_match() {
  # assert_match <literal substring> <string> <description>
  # Checks that <string> contains <literal substring> anywhere. Uses a
  # here-string (not a pipe) so grep -q's early exit doesn't SIGPIPE
  # the producer.
  if grep -qF -- "$1" <<<"$2"; then
    printf "  PASS: %s\n" "$3"
    t_pass=$((t_pass + 1))
  else
    printf "  FAIL: %s\n    needle: %s\n    input:  %s\n" "$3" "$1" "$2" >&2
    t_fail=$((t_fail + 1))
  fi
}

assert_no_match() {
  # assert_no_match <literal substring> <string> <description>
  if grep -qF -- "$1" <<<"$2"; then
    printf "  FAIL: %s (found unexpected substring)\n    needle: %s\n    input:  %s\n" "$3" "$1" "$2" >&2
    t_fail=$((t_fail + 1))
  else
    printf "  PASS: %s\n" "$3"
    t_pass=$((t_pass + 1))
  fi
}

assert_regex() {
  # assert_regex <extended-regex> <string> <description>
  if grep -qE -- "$1" <<<"$2"; then
    printf "  PASS: %s\n" "$3"
    t_pass=$((t_pass + 1))
  else
    printf "  FAIL: %s\n    regex: %s\n    input: %s\n" "$3" "$1" "$2" >&2
    t_fail=$((t_fail + 1))
  fi
}

assert_file_size() {
  # assert_file_size <path> <expected-bytes> <description>
  local actual
  actual=$(wc -c < "$1" | tr -d ' ')
  assert_eq "$2" "$actual" "$3"
}

# Skip (exit 77) when a required fixture is missing. run-tests.sh reports
# these as SKIP rather than FAIL.
skip_if_missing() {
  # skip_if_missing <path> <reason>
  if [ ! -e "$1" ]; then
    printf "  SKIP: %s (%s)\n" "$2" "$1"
    exit 77
  fi
}

# Call at the end of each test to emit a per-test summary and exit with
# 0 (all passed) or 1 (at least one failure).
t_done() {
  printf "  -- %d passed, %d failed --\n" "$t_pass" "$t_fail"
  if [ "$t_fail" -gt 0 ]; then
    exit 1
  fi
  exit 0
}
