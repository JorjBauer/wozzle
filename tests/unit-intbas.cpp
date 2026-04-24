// Unit tests for IntegerLister. Each case hand-assembles a small tokenized
// program, redirects stdout to a tmpfile, runs listFile(), and asserts on
// the captured text. Exits 0 only if every case passes.
//
// Build: see Makefile (unit-tests target).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "intbas.h"

static int passed = 0, failed = 0;

// Captures everything written to stdout within its lifetime. Restores
// the real stdout on destruction. Single-threaded, not reentrant.
struct StdoutCapture {
  int savedFd;
  FILE *tmp;

  StdoutCapture() {
    fflush(stdout);
    savedFd = dup(STDOUT_FILENO);
    tmp = tmpfile();
    if (!tmp || savedFd < 0) { perror("capture setup"); exit(2); }
    dup2(fileno(tmp), STDOUT_FILENO);
  }

  ~StdoutCapture() {
    fflush(stdout);
    dup2(savedFd, STDOUT_FILENO);
    close(savedFd);
    if (tmp) fclose(tmp);
  }

  // Read captured contents into a heap buffer. Caller frees.
  char *take() {
    fflush(stdout);
    fseek(tmp, 0, SEEK_END);
    long n = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) return NULL;
    size_t r = fread(buf, 1, (size_t)n, tmp);
    buf[r] = '\0';
    return buf;
  }
};

static void check(const char *name, bool cond, const char *detail = NULL) {
  if (cond) {
    fprintf(stderr, "  PASS: %s\n", name);
    passed++;
  } else {
    fprintf(stderr, "  FAIL: %s%s%s\n", name,
            detail ? " — " : "", detail ? detail : "");
    failed++;
  }
}

static bool contains(const char *haystack, const char *needle) {
  return haystack && strstr(haystack, needle) != NULL;
}

// ----- cases ---------------------------------------------------------

// Zero-byte input: loop never executes; returns true; no output.
static void case_empty() {
  uint8_t buf[1] = {0};
  StdoutCapture cap;
  IntegerLister l;
  bool rc = l.listFile(buf, 0, 0);
  char *out = cap.take();
  check("empty input returns true", rc == true);
  check("empty input prints nothing", out && out[0] == '\0');
  free(out);
}

// Single PRINT "HI" line. Token layout:
//   [lineLen=09][lineNum=0A 00][PRINT=61][open-quote=28][H=C8][I=C9]
//   [close-quote=29][EOL=01]
static void case_print_string() {
  uint8_t buf[] = {
    0x09, 0x0A, 0x00, 0x61, 0x28, 0xC8, 0xC9, 0x29, 0x01
  };
  StdoutCapture cap;
  IntegerLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("PRINT \"HI\" returns true", rc == true);
  check("output contains line number 10", contains(out, "10"));
  check("output contains PRINT keyword", contains(out, "PRINT"));
  check("output contains string body HI", contains(out, "HI"));
  free(out);
}

// 30 GOTO 100
//   [0B][1E 00][GOTO=5F][num-marker=B0][100-LE: 64 00][EOL=01]
//  length = 1+2+1+1+2+1 = 8, so lineLen = 8 ... wait
static void case_goto_constant() {
  // Correct length calc:
  //   len(1) + linenum(2) + GOTO(1) + marker(1) + value(2) + EOL(1) = 8
  uint8_t buf[] = {
    0x08, 0x1E, 0x00, 0x5F, 0xB0, 0x64, 0x00, 0x01
  };
  StdoutCapture cap;
  IntegerLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("GOTO 100 returns true", rc == true);
  check("line number 30 present", contains(out, "30"));
  check("GOTO keyword present", contains(out, "GOTO"));
  check("integer constant 100 decoded", contains(out, "100"));
  free(out);
}

// 40 REM HELLO
//   [0B][28 00][REM=5D][H=C8 E=C5 L=CC L=CC O=CF][EOL=01]
static void case_rem() {
  uint8_t buf[] = {
    0x0A, 0x28, 0x00, 0x5D, 0xC8, 0xC5, 0xCC, 0xCC, 0xCF, 0x01
  };
  StdoutCapture cap;
  IntegerLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("REM line returns true", rc == true);
  check("REM keyword present", contains(out, "REM"));
  check("REM text body present", contains(out, "HELLO"));
  free(out);
}

// Error path: open-quote with no close-quote or EOL before end-of-buffer.
static void case_truncated_string() {
  uint8_t buf[] = {
    0x09, 0x0A, 0x00, 0x61, 0x28, 0xC8, 0xC9  // truncated mid-string
  };
  StdoutCapture cap;
  IntegerLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("truncated string returns false", rc == false);
  check("truncated string error named",
        contains(out, "string constant"));
  free(out);
}

// Resync: a line whose stored lineLen doesn't match the bytes we consumed
// (because an inline 0x01 inside a REM was interpreted as EOL). The reader
// should still return success, emit a resync warning, and advance to the
// byte after the stored length — so a following line is parsed cleanly.
static void case_resync_after_rem_with_embedded_eol() {
  // Line 10: REM with text containing a raw 0x01 (no high bit) — reader
  //   will mistake it for EOL and stop early.
  //   len=0x07, linenum=10(0A 00), REM(5D), raw-0x01, then 2 more payload
  //   bytes (0x41 'A' low-bit) to make lineLen=0x07 plausible if you counted
  //   through them. Real EOL at the end.
  //
  // Layout: 07 0A 00 5D 01 41 01
  //   After parsing, actual = 1+2+1+1(EOL)=5 bytes, but stored lineLen=7.
  //   Resync target = startOffset + 7; remaining bytes get re-aligned.
  //
  // Line 20: simple "END" — must parse cleanly after resync.
  //   len=05, linenum=14(0x14 0x00), END(0x51), EOL(0x01)
  uint8_t buf[] = {
    0x07, 0x0A, 0x00, 0x5D, 0x01, 0x41, 0x01,
    0x05, 0x14, 0x00, 0x51, 0x01
  };
  StdoutCapture cap;
  IntegerLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("resync case returns true", rc == true);
  check("resync warning was printed", contains(out, "resyncing"));
  check("line 20 still reached after resync", contains(out, "20"));
  check("END keyword after resync", contains(out, "END"));
  free(out);
}

// skipBytes: DOS-style 2-byte length prefix ahead of the tokenized
// program. The lister should skip those bytes before parsing.
static void case_skipbytes() {
  // Prefix: 0xAA 0xBB (arbitrary); then a simple PRINT "HI" line.
  uint8_t buf[] = {
    0xAA, 0xBB,
    0x09, 0x0A, 0x00, 0x61, 0x28, 0xC8, 0xC9, 0x29, 0x01
  };
  StdoutCapture cap;
  IntegerLister l;
  bool rc = l.listFile(buf, sizeof(buf), 2);
  char *out = cap.take();
  check("skipBytes case returns true", rc == true);
  check("PRINT parsed after skipBytes", contains(out, "PRINT"));
  check("string body after skipBytes", contains(out, "HI"));
  // Prefix bytes must not leak into output.
  check("skipped prefix not echoed (no 0xAA)",
        out && strchr(out, (char)0xAA) == NULL);
  free(out);
}

int main(int, char **) {
  case_empty();
  case_print_string();
  case_goto_constant();
  case_rem();
  case_truncated_string();
  case_resync_after_rem_with_embedded_eol();
  case_skipbytes();

  fprintf(stderr, "  -- unit-intbas: %d passed, %d failed --\n",
          passed, failed);
  return failed ? 1 : 0;
}
