// Unit tests for IntegerLister. Each case hand-assembles a small tokenized
// program, runs listFile(), and asserts on the lines it returns and the
// messages it raised. Exits 0 only if every case passes.
//
// Build: see Makefile (unit-tests target).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "intbas.h"
#include "wozmsg.h"

static int passed = 0, failed = 0;

// Everything one listFile() call produced: its return value, the listing,
// and any messages it raised along the way.
struct Listed {
  bool rc;
  BasicListing lines;
  std::string text;      // the lines joined, one per row
  std::string messages;
};

static void collectMsg(wozMsgLevel level, const char *msg, void *ctx)
{
  std::string *s = (std::string *)ctx;
  s->append(msg);
  s->append("\n");
}

static Listed list(uint8_t *buf, uint32_t size, uint8_t skipBytes)
{
  Listed r;
  wozSetMsgSink(collectMsg, &r.messages);
  IntegerLister l;
  r.rc = l.listFile(buf, size, skipBytes, &r.lines);
  wozSetMsgSink(NULL, NULL);
  for (size_t i = 0; i < r.lines.size(); i++) {
    r.text += r.lines[i].text;
    r.text += "\n";
  }
  return r;
}

static void check(const char *name, bool cond, const char *detail = NULL) {
  if (cond) {
    fprintf(stderr, "  PASS: %s\n", name);
    passed++;
  } else {
    fprintf(stderr, "  FAIL: %s%s%s\n", name,
            detail ? " - " : "", detail ? detail : "");
    failed++;
  }
}

static bool contains(const std::string &haystack, const char *needle) {
  return haystack.find(needle) != std::string::npos;
}

// ----- cases ---------------------------------------------------------

// Zero-byte input: loop never executes; returns true; no output.
static void case_empty() {
  uint8_t buf[1] = {0};
  Listed r = list(buf, 0, 0);
  check("empty input returns true", r.rc == true);
  check("empty input lists nothing", r.lines.empty());
  check("empty input raises no messages", r.messages.empty());
}

// Single PRINT "HI" line. Token layout:
//   [lineLen=09][lineNum=0A 00][PRINT=61][open-quote=28][H=C8][I=C9]
//   [close-quote=29][EOL=01]
static void case_print_string() {
  uint8_t buf[] = {
    0x09, 0x0A, 0x00, 0x61, 0x28, 0xC8, 0xC9, 0x29, 0x01
  };
  Listed r = list(buf, sizeof(buf), 0);
  check("PRINT \"HI\" returns true", r.rc == true);
  check("line number 10", r.lines.size() == 1 && r.lines[0].lineNumber == 10);
  check("output contains PRINT keyword", contains(r.text, "PRINT"));
  check("output contains string body HI", contains(r.text, "\"HI\""));
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
  Listed r = list(buf, sizeof(buf), 0);
  check("GOTO 100 returns true", r.rc == true);
  check("line number 30", r.lines.size() == 1 && r.lines[0].lineNumber == 30);
  check("GOTO keyword present", contains(r.text, "GOTO"));
  check("integer constant 100 decoded", contains(r.text, "100"));
}

// 40 REM HELLO
//   [0B][28 00][REM=5D][H=C8 E=C5 L=CC L=CC O=CF][EOL=01]
static void case_rem() {
  uint8_t buf[] = {
    0x0A, 0x28, 0x00, 0x5D, 0xC8, 0xC5, 0xCC, 0xCC, 0xCF, 0x01
  };
  Listed r = list(buf, sizeof(buf), 0);
  check("REM line returns true", r.rc == true);
  check("REM keyword present", contains(r.text, "REM"));
  check("REM text body present", contains(r.text, "HELLO"));
}

// Error path: open-quote with no close-quote or EOL before end-of-buffer.
static void case_truncated_string() {
  uint8_t buf[] = {
    0x09, 0x0A, 0x00, 0x61, 0x28, 0xC8, 0xC9  // truncated mid-string
  };
  Listed r = list(buf, sizeof(buf), 0);
  check("truncated string returns false", r.rc == false);
  check("truncated string error named",
        contains(r.messages, "string constant"));
  check("partial line kept", contains(r.text, "PRINT \"HI"));
}

// Binary payload inside a REM: a REM whose body contains raw control bytes
// (here a 0x01, which a naive scan would mistake for EOL). The reader should
// use the stored line length to span the whole body, render it as hex octets
// instead of garbled control characters, NOT trip the resync path, and parse
// the following line cleanly.
static void case_rem_with_embedded_binary() {
  // Line 10: REM with a 2-byte binary body {0x01, 0x41}. A scan-to-EOL would
  //   stop at the 0x01; the length-driven reader spans both bytes.
  //   len=0x07, linenum=10(0A 00), REM(5D), body(01 41), real EOL(01).
  //
  // Line 20: simple "END" - must parse cleanly afterward.
  //   len=05, linenum=14(0x14 0x00), END(0x51), EOL(0x01)
  uint8_t buf[] = {
    0x07, 0x0A, 0x00, 0x5D, 0x01, 0x41, 0x01,
    0x05, 0x14, 0x00, 0x51, 0x01
  };
  Listed r = list(buf, sizeof(buf), 0);
  check("binary REM case returns true", r.rc == true);
  check("binary body labelled", contains(r.text, "binary"));
  check("binary body rendered as hex octets", contains(r.text, "01 41"));
  check("no resync needed for length-spanned REM",
        !contains(r.messages, "resyncing"));
  check("line 20 still reached", r.lines.size() == 2 && r.lines[1].lineNumber == 20);
  check("END keyword after binary REM",
        r.lines.size() == 2 && contains(r.lines[1].text, "END"));
}

// skipBytes: DOS-style 2-byte length prefix ahead of the tokenized
// program. The lister should skip those bytes before parsing.
static void case_skipbytes() {
  // Prefix: 0xAA 0xBB (arbitrary); then a simple PRINT "HI" line.
  uint8_t buf[] = {
    0xAA, 0xBB,
    0x09, 0x0A, 0x00, 0x61, 0x28, 0xC8, 0xC9, 0x29, 0x01
  };
  Listed r = list(buf, sizeof(buf), 2);
  check("skipBytes case returns true", r.rc == true);
  check("PRINT parsed after skipBytes", contains(r.text, "PRINT"));
  check("string body after skipBytes", contains(r.text, "HI"));
  // Prefix bytes must not leak into output.
  check("skipped prefix not echoed (no 0xAA)",
        r.text.find((char)0xAA) == std::string::npos);
}

int main(int, char **) {
  case_empty();
  case_print_string();
  case_goto_constant();
  case_rem();
  case_truncated_string();
  case_rem_with_embedded_binary();
  case_skipbytes();

  fprintf(stderr, "  -- unit-intbas: %d passed, %d failed --\n",
          passed, failed);
  return failed ? 1 : 0;
}
