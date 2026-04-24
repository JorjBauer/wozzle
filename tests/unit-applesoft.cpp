// Unit tests for ApplesoftLister. See tests/unit-intbas.cpp for the
// capture/assertion pattern — this file mirrors it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applesoft.h"

static int passed = 0, failed = 0;

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

static bool contains(const char *h, const char *n) {
  return h && strstr(h, n) != NULL;
}

// ----- cases ---------------------------------------------------------

// Program with only the end-of-program marker (next-line-ptr = 0x0000).
// Applesoft reads lo,hi each loop; 0,0 means done. Lister returns true.
static void case_empty_program() {
  uint8_t buf[] = { 0x00, 0x00 };
  StdoutCapture cap;
  ApplesoftLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("empty program returns true", rc == true);
  check("empty program prints nothing", out && out[0] == '\0');
  free(out);
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
  StdoutCapture cap;
  ApplesoftLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("10 END returns true", rc == true);
  check("line number 10 present", contains(out, "10"));
  check("END keyword decoded", contains(out, "END"));
  free(out);
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
  StdoutCapture cap;
  ApplesoftLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("PRINT \"HI\" returns true", rc == true);
  check("line 20 present", contains(out, "20"));
  check("PRINT keyword decoded", contains(out, "PRINT"));
  check("string HI present", contains(out, "HI"));
  free(out);
}

// Missing end-of-program marker: the file runs off the end looking for
// 0/0, which the lister reports as "ended early" (returns false).
static void case_missing_end_marker() {
  // Line 10 END, but no 00 00 terminator afterwards.
  uint8_t buf[] = {
    0x08, 0x08, 0x0A, 0x00, 0x80, 0x00
  };
  StdoutCapture cap;
  ApplesoftLister l;
  bool rc = l.listFile(buf, sizeof(buf), 0);
  char *out = cap.take();
  check("missing end marker returns false", rc == false);
  check("error names the early end", contains(out, "ended early"));
  free(out);
}

// skipBytes: DOS stores a 2-byte length prefix ahead of the tokenized
// program; skipping those must not lose the first real bytes.
static void case_skipbytes() {
  uint8_t buf[] = {
    0x05, 0x00,  // DOS-style length prefix (value doesn't matter here)
    0x08, 0x08, 0x0A, 0x00, 0x80, 0x00,
    0x00, 0x00
  };
  StdoutCapture cap;
  ApplesoftLister l;
  bool rc = l.listFile(buf, sizeof(buf), 2);
  char *out = cap.take();
  check("skipBytes returns true", rc == true);
  check("END still decoded after skip", contains(out, "END"));
  free(out);
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
