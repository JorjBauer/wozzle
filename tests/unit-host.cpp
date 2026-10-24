// Unit tests for the host hooks (wozhost.h). A program like Aiie installs
// its own file and memory hooks, so the library must send every file and
// allocator call through them, and honor keepFilesOpen and skipChecksum.
// The hooks here are POSIX underneath, with counters on top.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "woz.h"
#include "wozhost.h"

static int passed = 0, failed = 0;

static void check(bool ok, const char *name)
{
  fprintf(stderr, "  %s: %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) passed++; else failed++;
}

static struct {
  int opens, closes, reads, writes, seeks;
  int allocs, frees;
} counts;

static int tOpen(const char *path, wozOpenMode mode, void *ctx)
{
  (void)ctx;
  int flags = (mode == WOZ_OPEN_READ) ? O_RDONLY : O_RDWR;
  if (mode == WOZ_OPEN_CREATE) flags |= O_CREAT|O_TRUNC;
  int fd = open(path, flags, S_IRUSR|S_IWUSR);
  if (fd != -1) counts.opens++;
  return fd;
}
static int tClose(int fd, void *ctx) { (void)ctx; counts.closes++; return close(fd); }
static long tRead(int fd, void *buf, size_t n, void *ctx) { (void)ctx; counts.reads++; return read(fd, buf, n); }
static long tWrite(int fd, const void *buf, size_t n, void *ctx) { (void)ctx; counts.writes++; return write(fd, buf, n); }
static long tSeek(int fd, long off, int whence, void *ctx) { (void)ctx; counts.seeks++; return lseek(fd, off, whence); }
static void *tMalloc(size_t n, void *ctx) { (void)ctx; counts.allocs++; return malloc(n); }
static void *tCalloc(size_t c, size_t n, void *ctx) { (void)ctx; counts.allocs++; return calloc(c, n); }
static void *tRealloc(void *p, size_t n, void *ctx) { (void)ctx; if (!p) counts.allocs++; return realloc(p, n); }
static void tFree(void *p, void *ctx) { (void)ctx; if (p) counts.frees++; free(p); }

static void installHost(bool keepFilesOpen, bool skipChecksum)
{
  wozHost h;
  memset(&h, 0, sizeof(h));
  h.open = tOpen; h.close = tClose; h.read = tRead; h.write = tWrite; h.seek = tSeek;
  h.malloc = tMalloc; h.calloc = tCalloc; h.realloc = tRealloc; h.free = tFree;
  h.keepFilesOpen = keepFilesOpen;
  h.skipChecksum = skipChecksum;
  wozSetHost(&h);
  memset(&counts, 0, sizeof(counts));
}

static bool readAll(const char *path, uint8_t *buf, size_t n)
{
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  return got == n;
}

static uint32_t wozHeaderCRC(const char *path)
{
  uint8_t hdr[12];
  if (!readAll(path, hdr, sizeof(hdr))) return 0xFFFFFFFF;
  return hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
}

int main(int argc, char *argv[])
{
  (void)argc; (void)argv;
  const char *dir = getenv("TMPDIR");
  if (!dir) dir = "/tmp";
  char dsk[512], woz[512], back[512];
  snprintf(dsk, sizeof(dsk), "%s/unit-host-%d.dsk", dir, (int)getpid());
  snprintf(woz, sizeof(woz), "%s/unit-host-%d.woz", dir, (int)getpid());
  snprintf(back, sizeof(back), "%s/unit-host-%d-back.dsk", dir, (int)getpid());

  // A 35-track image where every sector is different, so a sector that
  // lands in the wrong place on the way back shows up
  static uint8_t image[35*16*256], again[35*16*256];
  for (size_t i = 0; i < sizeof(image); i++)
    image[i] = (uint8_t)((i * 7) ^ (i >> 8));
  FILE *f = fopen(dsk, "wb");
  if (!f || fwrite(image, 1, sizeof(image), f) != sizeof(image)) {
    fprintf(stderr, "  FAIL: couldn't write %s\n", dsk);
    return 1;
  }
  fclose(f);

  // Load and convert with the default settings
  installHost(false, false);
  Woz *w = new Woz(false);
  check(w->readFile(dsk, true, T_DSK), "DSK loads through the hooks");
  check(counts.opens >= 1 && counts.reads >= 35, "open and read went through the hooks");
  check(counts.opens == counts.closes, "a preloaded image's file is closed when keepFilesOpen is off");
  check(w->writeFile(woz, T_WOZ), "WOZ writes through the hooks");
  check(counts.writes > 0 && counts.seeks > 0, "write and seek went through the hooks");
  check(wozHeaderCRC(woz) != 0, "the WOZ CRC is computed when skipChecksum is off");
  delete w;
  check(counts.allocs > 0, "memory came from the allocator hooks");
  check(counts.allocs == counts.frees, "every allocation was freed through the hooks");

  // The WOZ comes back as the same sectors
  installHost(false, false);
  w = new Woz(false);
  check(w->readFile(woz, true, T_WOZ), "WOZ reloads");
  check(w->writeFile(back, T_DSK), "WOZ converts back to DSK");
  delete w;
  check(readAll(back, again, sizeof(again)) && memcmp(image, again, sizeof(image)) == 0,
        "DSK to WOZ to DSK round-trips byte for byte");

  // The two settings
  installHost(true, true);
  w = new Woz(false);
  check(w->readFile(dsk, true, T_DSK), "DSK loads with keepFilesOpen on");
  check(counts.opens == counts.closes + 1, "the image's file stays open when keepFilesOpen is on");
  check(w->writeFile(woz, T_WOZ), "WOZ writes with skipChecksum on");
  check(wozHeaderCRC(woz) == 0, "the WOZ CRC is 0 when skipChecksum is on");
  delete w;
  check(counts.opens == counts.closes, "deleting the Woz closes the file it kept open");

  // NULL puts the POSIX defaults back
  wozSetHost(NULL);
  memset(&counts, 0, sizeof(counts));
  w = new Woz(false);
  check(w->readFile(dsk, true, T_DSK), "DSK loads with the default hooks");
  delete w;
  check(counts.opens == 0 && counts.allocs == 0, "wozSetHost(NULL) restores the defaults");

  unlink(dsk); unlink(woz); unlink(back);
  fprintf(stderr, "  %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
