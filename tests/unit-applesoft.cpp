// Unit tests for ApplesoftLister. See tests/unit-intbas.cpp for the
// collection/assertion pattern - this file mirrors it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "applesoft.h"
#include "wozmsg.h"

static int passed = 0, failed = 0;

// Everything one listFile() call produced: its return value, the listing,
// and any messages it raised along the way.
struct Listed {
  bool rc;
  BasicListing lines;
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
  ApplesoftLister l;
  r.rc = l.listFile(buf, size, skipBytes, &r.lines);
  wozSetMsgSink(NULL, NULL);
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

static bool contains(const std::string &h, const char *n) {
  return h.find(n) != std::string::npos;
}

// ----- cases ---------------------------------------------------------

// Program with only the end-of-program marker (next-line-ptr = 0x0000).
// Applesoft reads lo,hi each loop; 0,0 means done. Lister returns true.
static void case_empty_program() {
  uint8_t buf[] = { 0x00, 0x00 };
  Listed r = list(buf, sizeof(buf), 0);
  check("empty program returns true", r.rc == true);
  check("empty program lists nothing", r.lines.empty());
  check("empty program raises no messages", r.messages.empty());
}

// 10 END
//   Layout: [next-ptr lo,hi][line-num lo,hi][tokens...][0x00 EOL]
//   then end-of-program [00 00]
// next-ptr value doesn't affect listing; we put 0x08 0x08 for illustration.
// line-num = 10 → 0A 00.  END = 0x80 (Applesoft END token).
static void case_end_line() {
  uint8_t buf[] = {
    0x08, 0x08, 0x0A, 0x00, 0x80, 0x00,
    0x00, 0x00
  };
  Listed r = list(buf, sizeof(buf), 0);
  check("10 END returns true", r.rc == true);
  check("one line listed", r.lines.size() == 1);
  check("line number 10", r.lines.size() == 1 && r.lines[0].lineNumber == 10);
  check("END keyword decoded", r.lines.size() == 1 && contains(r.lines[0].text, "END"));
}

// 20 PRINT "HI"
//   next-ptr, line-num 14 00, PRINT = 0xBA, then ASCII bytes 22 'H' 'I' 22
//   (Applesoft stores string chars as plain ASCII, not high-bit), then 0x00
static void case_print_string() {
  uint8_t buf[] = {
    0x10, 0x08, 0x14, 0x00, 0xBA,
    0x22, 'H', 'I', 0x22, 0x00,
    0x00, 0x00
  };
  Listed r = list(buf, sizeof(buf), 0);
  check("PRINT \"HI\" returns true", r.rc == true);
  check("line 20", r.lines.size() == 1 && r.lines[0].lineNumber == 20);
  check("PRINT keyword and string decoded",
        r.lines.size() == 1 && r.lines[0].text == " PRINT \"HI\"");
}

// Missing end-of-program marker: the file runs off the end looking for
// 0/0, which the lister reports as "ended early" (returns false).
static void case_missing_end_marker() {
  // Line 10 END, but no 00 00 terminator afterwards.
  uint8_t buf[] = {
    0x08, 0x08, 0x0A, 0x00, 0x80, 0x00
  };
  Listed r = list(buf, sizeof(buf), 0);
  check("missing end marker returns false", r.rc == false);
  check("lines before the problem are kept",
        r.lines.size() == 1 && r.lines[0].lineNumber == 10);
  check("error names the early end", contains(r.messages, "ended early"));
}

// skipBytes: DOS stores a 2-byte length prefix ahead of the tokenized
// program; skipping those must not lose the first real bytes.
static void case_skipbytes() {
  uint8_t buf[] = {
    0x05, 0x00,  // DOS-style length prefix (value doesn't matter here)
    0x08, 0x08, 0x0A, 0x00, 0x80, 0x00,
    0x00, 0x00
  };
  Listed r = list(buf, sizeof(buf), 2);
  check("skipBytes returns true", r.rc == true);
  check("END still decoded after skip",
        r.lines.size() == 1 && contains(r.lines[0].text, "END"));
}

int main(int, char **) {
  case_empty_program();
  case_end_line();
  case_print_string();
  case_missing_end_marker();
  case_skipbytes();

  fprintf(stderr, "  -- unit-applesoft: %d passed, %d failed --\n",
          passed, failed);
  return failed ? 1 : 0;
}
