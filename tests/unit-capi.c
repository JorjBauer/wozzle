// Unit tests for the C interface (wozlib.h). This file is C, not C++, so
// building it at all shows the header works from plain C. Every image it
// uses it makes itself, in $TMPDIR.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#include "wozlib.h"

static int passed = 0, failed = 0;

static void check(bool ok, const char *name)
{
  fprintf(stderr, "  %s: %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) passed++; else failed++;
}

// Keep the library quiet, apart from errors when a check fails
static void quietSink(wozMsgLevel level, const char *msg, void *ctx)
{
  (void)level; (void)msg; (void)ctx;
}

static int progressCalls = 0;
static void countProgress(const char *path, uint32_t dataBytes,
                          uint32_t resourceBytes, void *ctx)
{
  (void)path; (void)dataBytes; (void)resourceBytes; (void)ctx;
  progressCalls++;
}

int main(void)
{
  wozInit();
  wozSetMsgSink(quietSink, NULL);

  const char *dir = getenv("TMPDIR");
  if (!dir) dir = "/tmp";
  char po[512], po2[512], hdv[512], dsk[512], pas[512], saved[512];
  snprintf(po, sizeof(po), "%s/unit-capi-%d.po", dir, (int)getpid());
  snprintf(po2, sizeof(po2), "%s/unit-capi-%d-2.po", dir, (int)getpid());
  snprintf(hdv, sizeof(hdv), "%s/unit-capi-%d.hdv", dir, (int)getpid());
  snprintf(dsk, sizeof(dsk), "%s/unit-capi-%d.dsk", dir, (int)getpid());
  snprintf(pas, sizeof(pas), "%s/unit-capi-%d-pas.po", dir, (int)getpid());
  snprintf(saved, sizeof(saved), "%s/unit-capi-%d-saved.po", dir, (int)getpid());

  // ---- blank images ----
  check(wozParseSize("800k") == 1600, "800k is 1600 blocks");
  check(wozParseSize("bogus") == -1, "a bad size is -1");
  check(wozCreateBlankImage(po, WOZ_FS_PRODOS, "capi", 0, 280, false),
        "a blank ProDOS floppy is created");
  check(!wozCreateBlankImage(po, WOZ_FS_PRODOS, "capi", 0, 280, false),
        "an existing image isn't overwritten");
  check(strstr(wozLastError(), "already exists") != NULL,
        "wozLastError says why");
  check(!wozCreateBlankImage(hdv, WOZ_FS_PRODOS, "bad name!", 0, 1600, false),
        "a bad volume name is refused");
  check(wozCreateBlankImage(hdv, WOZ_FS_PRODOS, NULL, 0, 1600, true),
        "a bootable 800k hard-disk image is created");
  check(wozCreateBlankImage(dsk, WOZ_FS_DOS33, NULL, 7, 0, false),
        "a blank DOS 3.3 image is created");
  check(wozCreateBlankImage(pas, WOZ_FS_PASCAL, "PAS", 0, 280, false),
        "a blank Pascal image is created");

  check(wozImageDetect(po) == WOZ_FS_PRODOS, "ProDOS is detected");
  check(wozImageDetect(dsk) == WOZ_FS_DOS33, "DOS 3.3 is detected");
  check(wozImageDetect(pas) == WOZ_FS_PASCAL, "Pascal is detected");

  // ---- ProDOS ----
  wozImage *img = wozImageOpen(po, WOZ_FS_UNKNOWN);
  check(img != NULL, "the ProDOS image opens");
  if (!img) return 1;
  check(wozImageFsType(img) == WOZ_FS_PRODOS, "it opens as ProDOS");
  check(wozImageProbe(img), "it probes as ProDOS");

  wozVolumeInfo vi;
  check(wozImageGetVolumeInfo(img, &vi), "volume info reads");
  check(!strcmp(vi.volumeName, "CAPI"), "the volume name was upper-cased");
  check(vi.unitSize == 512 && vi.totalUnits == 280, "280 blocks of 512");
  check(vi.freeUnits == 273, "a blank floppy has 273 blocks free");

  check(wozImageList(img, NULL, NULL, 0) == 0, "a blank root is empty");
  check(wozImageMakeDirectory(img, "SUB"), "mkdir works");

  uint8_t bin[1000];
  for (int i = 0; i < (int)sizeof(bin); i++) bin[i] = (uint8_t)i;
  check(wozImageWriteFile(img, "SUB/DATA", bin, sizeof(bin), 0x06, 0x0800,
                          false), "a binary goes into a subdirectory");
  const char *prog = "10 PRINT \"HELLO\"\n20 GOTO 10\n";
  check(wozImageWriteFile(img, "HELLO", (const uint8_t *)prog,
                          (uint32_t)strlen(prog), -1, -1, true),
        "an Applesoft listing is tokenized in");
  check(wozImageIsDirty(img), "the image is dirty");

  wozEntry ents[8];
  int n = wozImageList(img, NULL, ents, 8);
  check(n == 2, "the root lists two entries");
  bool sawDir = false, sawHello = false;
  for (int i = 0; i < n && i < 8; i++) {
    if (!strcmp(ents[i].name, "SUB") && ents[i].isDirectory) sawDir = true;
    if (!strcmp(ents[i].name, "HELLO") && ents[i].fileType == 0xFC &&
        ents[i].auxType == 0x0801) sawHello = true;
  }
  check(sawDir, "SUB lists as a directory");
  check(sawHello, "HELLO lists as BAS at $0801");

  n = wozImageList(img, "SUB", ents, 8);
  check(n == 1 && !strcmp(ents[0].name, "DATA"), "SUB lists DATA");
  check(n == 1 && ents[0].length == sizeof(bin) && ents[0].auxType == 0x0800,
        "DATA has its length and aux type");
  check(wozImageList(img, "NOPE", ents, 8) == -1, "a missing directory is -1");
  // ProDOS dates go to the minute, so allow for the clock ticking over
  time_t now = time(NULL);
  check(n == 1 && ents[0].created != -1 && ents[0].modified != -1 &&
        now - ents[0].modified < 120 && ents[0].modified - now < 120,
        "a new file is dated now");

  wozEntry st;
  check(wozImageStat(img, "SUB/DATA", &st) && st.fileType == 0x06,
        "stat finds a nested file");

  uint8_t *data = NULL;
  uint32_t len = 0;
  check(wozImageReadFile(img, "SUB/DATA", false, false, &data, &len),
        "the binary reads back");
  check(len == sizeof(bin) && data && !memcmp(data, bin, len),
        "byte for byte");
  wozFree(data);
  check(!wozImageReadFile(img, "SUB/DATA", true, false, &data, &len),
        "a plain file has no resource fork");

  char *text = NULL;
  check(wozImageListBasic(img, "HELLO", &text, &len), "HELLO lists");
  // LIST puts a space before each keyword, as the real thing does
  check(text && !strcmp(text, "10  PRINT \"HELLO\"\n20  GOTO 10\n"),
        "the listing matches what went in");
  wozFree(text);
  check(!wozImageListBasic(img, "SUB/DATA", &text, &len),
        "a binary won't list as BASIC");

  check(wozImageRenameVolume(img, "renamed"), "the volume renames");
  check(wozImageGetVolumeInfo(img, &vi) && !strcmp(vi.volumeName, "RENAMED"),
        "the new name shows");
  check(wozImageInstallBootBlocks(img, NULL), "boot blocks install");
  check(wozImageSave(img, saved), "the image saves elsewhere");
  wozImageClose(img);

  img = wozImageOpen(saved, WOZ_FS_PRODOS);
  check(img != NULL, "the saved image reopens");
  if (!img) return 1;
  check(wozImageList(img, "SUB", ents, 8) == 1, "its subdirectory survived");
  check(wozImageDelete(img, "HELLO", false), "a file deletes");
  check(wozImageDelete(img, "SUB", true), "a directory deletes recursively");
  check(wozImageList(img, NULL, ents, 8) == 0, "the root is empty again");
  check(!wozImageDelete(img, "HELLO", false), "deleting it twice fails");
  check(!wozImageKrunch(img, NULL), "krunch is refused on ProDOS");
  wozImageClose(img);

  // ---- image to image ----
  wozImage *src = wozImageOpen(po, WOZ_FS_PRODOS);
  check(wozCreateBlankImage(po2, WOZ_FS_PRODOS, "DEST", 0, 280, false),
        "a destination is created");
  wozImage *dst = wozImageOpen(po2, WOZ_FS_PRODOS);
  check(src && dst, "both images open");
  if (!src || !dst) return 1;
  wozImageMakeDirectory(src, "SUB");
  wozImageWriteFile(src, "SUB/DATA", bin, sizeof(bin), 0x06, 0x0800, false);
  wozImageWriteFile(src, "TOP", bin, 10, 0x06, 0x2000, false);

  wozCopyStats cs;
  check(wozImageCopy(src, dst, NULL, 0, NULL, false, countProgress, NULL, &cs),
        "the whole volume copies");
  check(cs.files == 2 && cs.dirsMade == 1, "two files and one directory");
  check(progressCalls == 2, "progress was called per file");
  check(!wozImageCopy(src, dst, NULL, 0, NULL, false, NULL, NULL, &cs),
        "copying again collides");
  check(cs.collisions == 2 && strstr(wozLastError(), "SUB/DATA"),
        "the collisions are counted and named");
  const char *sel[] = { "TOP" };
  check(wozImageCopy(src, dst, sel, 1, "STASH", true, NULL, NULL, &cs) &&
        cs.files == 1, "one file copies under a new directory");
  check(wozImageStat(dst, "STASH/TOP", &st) && st.auxType == 0x2000,
        "it kept its aux type");
  wozImageClose(src);
  wozImageClose(dst);

  // ---- DOS 3.3 ----
  img = wozImageOpen(dsk, WOZ_FS_UNKNOWN);
  check(img && wozImageFsType(img) == WOZ_FS_DOS33, "the DOS image opens");
  if (!img) return 1;
  check(wozImageGetVolumeInfo(img, &vi) && vi.dosVolume == 7 &&
        vi.unitSize == 256 && vi.totalUnits == 560,
        "volume 7, 560 sectors of 256");
  check(wozImageWriteFile(img, "THING", bin, 300, 0x06, 0x0300, false),
        "a binary goes in");
  n = wozImageList(img, NULL, ents, 8);
  check(n == 1 && !strcmp(ents[0].name, "THING") && ents[0].fileType == 0x06,
        "it lists as BIN");
  check(n == 1 && ents[0].length == 300, "with the length from its header");
  check(n == 1 && ents[0].created == -1 && !ents[0].locked,
        "no date, not locked");
  check(n == 1 && ents[0].auxType == 0x0300,
        "its load address shows as the aux type");
  check(wozImageReadFile(img, "THING", false, false, &data, &len) &&
        len == 300 && !memcmp(data, bin, 300),
        "it reads back as it went in");
  wozFree(data);
  // As stored: DOS 3.3 keeps a B file's load address and length in front
  check(wozImageReadFile(img, "THING", false, true, &data, &len) &&
        len == 512 && data[0] == 0x00 && data[1] == 0x03 &&
        data[2] == 300 % 256 && data[3] == 300 / 256 &&
        !memcmp(data + 4, bin, 300), "the raw read has the header");
  wozFree(data);
  const char *txt = "HELLO\rWORLD\r";
  check(wozImageWriteFile(img, "NOTE", (const uint8_t *)txt,
                          (uint32_t)strlen(txt), 0x04, 0, false),
        "a text file goes in");
  check(wozImageReadFile(img, "NOTE", false, false, &data, &len) &&
        len == strlen(txt) && !memcmp(data, txt, len),
        "a T file reads back up to its first $00");
  wozFree(data);

  // 40000 bytes is 157 sectors, past what one T/S list sector can name
  uint32_t bigLen = 40000;
  uint8_t *big = (uint8_t *)malloc(bigLen);
  for (uint32_t i = 0; i < bigLen; i++) big[i] = (uint8_t)(i * 7);
  check(wozImageWriteFile(img, "BIG", big, bigLen, 0x06, 0x4000, false),
        "a 157-sector binary goes in");
  check(wozImageReadFile(img, "BIG", false, false, &data, &len) &&
        len == bigLen && !memcmp(data, big, bigLen),
        "and reads back across two T/S lists");
  wozFree(data);
  free(big);
  check(!wozImageMakeDirectory(img, "DIR"), "DOS 3.3 has no directories");
  check(!wozImageRenameVolume(img, "NAME"), "nor a volume name");
  wozImageClose(img);

  // ---- Pascal ----
  img = wozImageOpen(pas, WOZ_FS_UNKNOWN);
  check(img && wozImageFsType(img) == WOZ_FS_PASCAL, "the Pascal image opens");
  if (!img) return 1;
  check(wozImageGetVolumeInfo(img, &vi) && !strcmp(vi.volumeName, "PAS") &&
        vi.totalUnits == 280, "PAS, 280 blocks");
  check(wozImageWriteFile(img, "NOTES.TEXT", bin, 1024, -1, -1, false),
        "a file goes in");
  check(wozImageList(img, NULL, ents, 8) == 1, "it lists");
  check(wozImageKrunch(img, NULL), "krunch runs");
  wozImageClose(img);

  // ---- a hard-disk image knows when it's changed ----
  img = wozImageOpen(hdv, WOZ_FS_PRODOS);
  check(img && !wozImageIsDirty(img), "a fresh hard-disk image is clean");
  if (img) {
    check(wozImageMakeDirectory(img, "NEWDIR") && wozImageIsDirty(img),
          "and dirty once it's changed");
    check(wozImageSave(img, NULL) && !wozImageIsDirty(img),
          "and clean again once it's saved");
    wozImageClose(img);
  }

  // ---- dates on a real disk ----
  // ProDOS 2.4's files are from 2016, which ProDOS stores as year 16
  img = wozImageOpen("ProDOS_2_4.po", WOZ_FS_PRODOS);
  if (img) {
    check(wozImageStat(img, "PRODOS", &st) && st.modified != -1,
          "PRODOS on ProDOS 2.4 has a date");
    time_t mod = (time_t)st.modified;
    struct tm *lt = localtime(&mod);
    check(lt && lt->tm_year + 1900 == 2016, "and it's in 2016, not 1916");
    wozImageClose(img);
  }

  // ---- errors ----
  check(wozImageOpen("/nonexistent/x.po", WOZ_FS_PRODOS) == NULL,
        "a missing image doesn't open");
  wozClearError();
  check(wozLastError()[0] == '\0', "wozClearError clears it");

  unlink(po); unlink(po2); unlink(hdv); unlink(dsk); unlink(pas);
  unlink(saved);
  fprintf(stderr, "  %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
