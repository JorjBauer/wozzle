// DOS 3.3 reads checked against CiderPress2's own expectations for its
// test image dos33/simple-sparse.do (DiskArcTests/TestDOS_Prefab.cs), a
// disk written by real DOS: I/A/B headers, stale lengths left by a second
// SAVE, sequential text, and a random-access text file spread across five
// T/S list sectors.
//
//   unit-dosref <path to simple-sparse.do>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wozlib.h"

static int passed = 0, failed = 0;

static void check(bool ok, const char *name)
{
  fprintf(stderr, "  %s: %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) passed++; else failed++;
}

static void quietSink(wozMsgLevel level, const char *msg, void *ctx)
{
  (void)level; (void)msg; (void)ctx;
}

static const struct {
  const char *name;
  uint32_t length;   // contents
  uint8_t fileType;
  uint16_t auxType;
} expected[] = {
  { "HELLO",          37,   0xFC, 0x0801 },
  { "BAS BIG",        5762, 0xFC, 0x0801 },
  { "BAS OVERSIZED",  12,   0xFC, 0x0801 },
  { "BAS SMALL",      12,   0xFC, 0x0801 },
  { "MK-SPARSE-TEXT", 323,  0xFC, 0x0801 },
  { "SPARSE-TEXT",    0,    0x04, 0x0000 },
  { "MK-BIG BIN",     255,  0xFC, 0x0801 },
  { "BIG BIN",        8184, 0x06, 0x2000 },
  { "OVERSIZED BIN",  8,    0x06, 0x2000 },
  { "SMALL BIN",      8,    0x06, 0x2000 },
  { "MK-TXT",         547,  0xFC, 0x0801 },
  { "TXT SMALL",      23,   0x04, 0x0000 },
  { "TXT BIG",        2790, 0x04, 0x0000 },
  { "TXT NOTRIM",     2790, 0x04, 0x0000 },
  { "CASE TEST",      1,    0x06, 0x0000 },
  { "case test",      2,    0x06, 0x0000 },
};

int main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s <simple-sparse.do>\n", argv[0]);
    return 2;
  }
  wozInit();
  wozSetMsgSink(quietSink, NULL);

  // A .do is DOS-order sectors, which is what .dsk means here
  wozImage *img = wozImageOpen(argv[1], WOZ_FS_DOS33);
  check(img != NULL, "the image opens");
  if (!img) return 1;

  wozEntry ents[32];
  int n = wozImageList(img, NULL, ents, 32);
  check(n == 16, "16 catalog entries");
  for (size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); i++) {
    char what[96];
    wozEntry e;
    bool found = wozImageStat(img, expected[i].name, &e);
    snprintf(what, sizeof(what), "%s: %u bytes, type $%02X, aux $%04X",
             expected[i].name, (unsigned)expected[i].length,
             expected[i].fileType, expected[i].auxType);
    check(found && e.length == expected[i].length &&
          e.fileType == expected[i].fileType &&
          e.auxType == expected[i].auxType, what);

    uint8_t *data = NULL;
    uint32_t len = 0;
    snprintf(what, sizeof(what), "%s reads %u bytes", expected[i].name,
             (unsigned)expected[i].length);
    check(wozImageReadFile(img, expected[i].name, false, false, &data, &len) &&
          len == expected[i].length, what);
    wozFree(data);
  }

  // "BAS SMALL" is 100 TEXT : NORMAL : HOME, and "BAS OVERSIZED" is the
  // same program saved over a longer one
  static const uint8_t textNormalHome[] = {
    0x0b, 0x08, 0x64, 0x00, 0x89, 0x3a, 0x9d, 0x3a, 0x97, 0x00, 0x00, 0x00
  };
  uint8_t *a = NULL, *b = NULL;
  uint32_t alen = 0, blen = 0;
  wozImageReadFile(img, "BAS SMALL", false, false, &a, &alen);
  wozImageReadFile(img, "BAS OVERSIZED", false, false, &b, &blen);
  check(alen == sizeof(textNormalHome) && !memcmp(a, textNormalHome, alen),
        "BAS SMALL is 100 TEXT : NORMAL : HOME");
  check(blen == alen && !memcmp(a, b, alen),
        "BAS OVERSIZED's contents stop where its header says");
  wozFree(a);
  wozFree(b);

  // The raw read keeps the random-access records where they were written:
  // records 2, 8, 122 and 488, each holding its own number and a CR
  uint8_t *raw = NULL;
  uint32_t rawLen = 0;
  check(wozImageReadFile(img, "SPARSE-TEXT", false, true, &raw, &rawLen) &&
        rawLen == 489 * 256, "SPARSE-TEXT is 489 sectors raw");
  static const int recs[] = { 2, 8, 122, 488 };
  bool recsOk = raw && rawLen == 489 * 256;
  for (int r = 0; recsOk && r < 489; r++) {
    char want[8] = "";
    for (size_t k = 0; k < 4; k++)
      if (recs[k] == r) snprintf(want, sizeof(want), "%d\r", r);
    char got[8];
    size_t g = 0;
    while (g < sizeof(got) - 1 && raw[r * 256 + g]) {
      got[g] = (char)(raw[r * 256 + g] & 0x7F);
      g++;
    }
    got[g] = '\0';
    if (strcmp(got, want)) recsOk = false;
  }
  check(recsOk, "records 2, 8, 122 and 488 are in place, holes are zero");
  wozFree(raw);

  wozImageClose(img);
  fprintf(stderr, "  %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
