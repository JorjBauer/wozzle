#ifndef __WOZHOST_H
#define __WOZHOST_H

// What the disk image library needs from the program it's built into.
//
// The library doesn't call open/read/write/lseek/close or the allocator
// directly. Everything goes through the hooks here, so a program that
// has its own idea of files (an emulator with an SD card file manager,
// say) or its own memory (a Teensy's external PSRAM) can hand those to
// the library without patching it.
//
// A POSIX program doesn't have to do anything: the defaults are the
// POSIX file calls and the C allocator. On Arduino there's no POSIX file
// layer to fall back on, so the file hooks have no default there and the
// program must install its own before opening an image.
//
// One host serves the whole program, and so does the message sink in
// wozmsg.h; neither is locked. Install them before opening an image, and
// keep every call into the library on one thread at a time (a GUI's
// worker thread, say), since the objects aren't locked either.
//
// File descriptors are whatever the open hook returns, and -1 means
// failure. Seeks take SEEK_SET, SEEK_CUR and SEEK_END from <stdio.h>.

#include <stddef.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  WOZ_OPEN_READ      = 0, // existing file, read only
  WOZ_OPEN_READWRITE = 1, // existing file, read and write in place
  WOZ_OPEN_CREATE    = 2  // read/write, created if missing, truncated if not
} wozOpenMode;

typedef struct {
  int  (*open)(const char *path, wozOpenMode mode, void *ctx);
  int  (*close)(int fd, void *ctx);
  long (*read)(int fd, void *buf, size_t nbyte, void *ctx);
  long (*write)(int fd, const void *buf, size_t nbyte, void *ctx);
  // Returns the new offset, or -1 on failure
  long (*seek)(int fd, long offset, int whence, void *ctx);

  void *(*malloc)(size_t size, void *ctx);
  void *(*calloc)(size_t count, size_t size, void *ctx);
  void *(*realloc)(void *ptr, size_t size, void *ctx);
  void  (*free)(void *ptr, void *ctx);

  // Leave a floppy image's file open after its tracks are preloaded,
  // instead of closing it, for hosts whose open is expensive
  bool keepFilesOpen;
  // Write 0 in a WOZ file's CRC field instead of computing it (the spec
  // allows 0, meaning "don't verify"). Computing it means reading the
  // whole image back in, which a slow card may not want to do.
  bool skipChecksum;

  void *ctx;

  // The current time, for dating new files and volumes. (time_t)-1 means
  // the program doesn't know, and things get no date. This is last in the
  // struct so a host written before it existed still lines up; the
  // default is time() on POSIX and "don't know" on Arduino, which may
  // have no clock.
  time_t (*now)(void *ctx);
} wozHost;

// Install a host. Any hook left NULL gets the default; the struct is
// copied, so it doesn't have to outlive the call. NULL restores all of
// the defaults.
void wozSetHost(const wozHost *host);
const wozHost *wozGetHost(void);

// What the library itself calls
int   wozOpen(const char *path, wozOpenMode mode);
int   wozClose(int fd);
long  wozRead(int fd, void *buf, size_t nbyte);
long  wozWrite(int fd, const void *buf, size_t nbyte);
long  wozSeek(int fd, long offset, int whence);
void *wozMalloc(size_t size);
void *wozCalloc(size_t count, size_t size);
void *wozRealloc(void *ptr, size_t size);
void  wozFree(void *ptr);
time_t wozNow(void);

#ifdef __cplusplus
};
#endif

#endif
