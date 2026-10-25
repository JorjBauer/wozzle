#include "wozhost.h"
#include "wozmsg.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef ARDUINO
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int posixOpen(const char *path, wozOpenMode mode, void *ctx)
{
  (void)ctx;
  switch (mode) {
  case WOZ_OPEN_READ:
    return open(path, O_RDONLY);
  case WOZ_OPEN_READWRITE:
    return open(path, O_RDWR);
  case WOZ_OPEN_CREATE:
    return open(path, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
  }
  return -1;
}

static int posixClose(int fd, void *ctx)
{
  (void)ctx;
  return close(fd);
}

static long posixRead(int fd, void *buf, size_t nbyte, void *ctx)
{
  (void)ctx;
  return (long)read(fd, buf, nbyte);
}

static long posixWrite(int fd, const void *buf, size_t nbyte, void *ctx)
{
  (void)ctx;
  return (long)write(fd, buf, nbyte);
}

static long posixSeek(int fd, long offset, int whence, void *ctx)
{
  (void)ctx;
  return (long)lseek(fd, (off_t)offset, whence);
}

#define DEFAULT_OPEN  posixOpen
#define DEFAULT_CLOSE posixClose
#define DEFAULT_READ  posixRead
#define DEFAULT_WRITE posixWrite
static time_t posixNow(void *ctx)
{
  (void)ctx;
  return time(NULL);
}

static int posixRename(const char *from, const char *to, void *ctx)
{
  (void)ctx;
#ifdef _WIN32
  // Windows won't rename over a file that's there
  remove(to);
#endif
  return rename(from, to);
}

static int posixRemove(const char *path, void *ctx)
{
  (void)ctx;
  return remove(path);
}

#define DEFAULT_SEEK   posixSeek
#define DEFAULT_NOW    posixNow
#define DEFAULT_RENAME posixRename
#define DEFAULT_REMOVE posixRemove

#else

// No POSIX files on Arduino, so until the program installs its own
// hooks every file operation fails
static int noOpen(const char *path, wozOpenMode mode, void *ctx)
{
  (void)mode; (void)ctx;
  wozMsg(WOZ_MSG_ERROR, "No file hooks installed (see wozSetHost); can't open %s", path);
  return -1;
}

static int noClose(int fd, void *ctx) { (void)fd; (void)ctx; return -1; }
static long noRead(int fd, void *buf, size_t nbyte, void *ctx) { (void)fd; (void)buf; (void)nbyte; (void)ctx; return -1; }
static long noWrite(int fd, const void *buf, size_t nbyte, void *ctx) { (void)fd; (void)buf; (void)nbyte; (void)ctx; return -1; }
static long noSeek(int fd, long offset, int whence, void *ctx) { (void)fd; (void)offset; (void)whence; (void)ctx; return -1; }

#define DEFAULT_OPEN  noOpen
#define DEFAULT_CLOSE noClose
#define DEFAULT_READ  noRead
#define DEFAULT_WRITE noWrite
static time_t noNow(void *ctx) { (void)ctx; return (time_t)-1; }

#define DEFAULT_SEEK   noSeek
#define DEFAULT_NOW    noNow
#define DEFAULT_RENAME NULL
#define DEFAULT_REMOVE NULL

#endif

static void *stdMalloc(size_t size, void *ctx) { (void)ctx; return malloc(size); }
static void *stdCalloc(size_t count, size_t size, void *ctx) { (void)ctx; return calloc(count, size); }
static void *stdRealloc(void *ptr, size_t size, void *ctx) { (void)ctx; return realloc(ptr, size); }
static void stdFree(void *ptr, void *ctx) { (void)ctx; free(ptr); }

static wozHost current = {
  DEFAULT_OPEN, DEFAULT_CLOSE, DEFAULT_READ, DEFAULT_WRITE, DEFAULT_SEEK,
  stdMalloc, stdCalloc, stdRealloc, stdFree,
  false, false,
  NULL,
  DEFAULT_NOW,
  DEFAULT_RENAME, DEFAULT_REMOVE
};

void wozSetHost(const wozHost *host)
{
  wozHost h;

  if (host) {
    h = *host;
  } else {
    h.keepFilesOpen = false;
    h.skipChecksum = false;
    h.ctx = NULL;
    h.open = NULL; h.close = NULL; h.read = NULL; h.write = NULL; h.seek = NULL;
    h.malloc = NULL; h.calloc = NULL; h.realloc = NULL; h.free = NULL;
    h.now = NULL;
    h.rename = NULL; h.remove = NULL;
  }

  if (!h.open)    h.open = DEFAULT_OPEN;
  if (!h.close)   h.close = DEFAULT_CLOSE;
  if (!h.read)    h.read = DEFAULT_READ;
  if (!h.write)   h.write = DEFAULT_WRITE;
  if (!h.seek)    h.seek = DEFAULT_SEEK;
  if (!h.malloc)  h.malloc = stdMalloc;
  if (!h.calloc)  h.calloc = stdCalloc;
  if (!h.realloc) h.realloc = stdRealloc;
  if (!h.free)    h.free = stdFree;
  if (!h.now)     h.now = DEFAULT_NOW;
  if (!h.rename)  h.rename = DEFAULT_RENAME;
  if (!h.remove)  h.remove = DEFAULT_REMOVE;

  current = h;
}

const wozHost *wozGetHost(void)
{
  return &current;
}

int wozOpen(const char *path, wozOpenMode mode)
{
  return current.open(path, mode, current.ctx);
}

int wozClose(int fd)
{
  return current.close(fd, current.ctx);
}

long wozRead(int fd, void *buf, size_t nbyte)
{
  return current.read(fd, buf, nbyte, current.ctx);
}

long wozWrite(int fd, const void *buf, size_t nbyte)
{
  return current.write(fd, buf, nbyte, current.ctx);
}

long wozSeek(int fd, long offset, int whence)
{
  return current.seek(fd, offset, whence, current.ctx);
}

void *wozMalloc(size_t size)
{
  return current.malloc(size, current.ctx);
}

void *wozCalloc(size_t count, size_t size)
{
  return current.calloc(count, size, current.ctx);
}

void *wozRealloc(void *ptr, size_t size)
{
  return current.realloc(ptr, size, current.ctx);
}

void wozFree(void *ptr)
{
  // Like free(), NULL is fine, whatever the hook makes of it
  if (ptr) current.free(ptr, current.ctx);
}

time_t wozNow(void)
{
  return current.now(current.ctx);
}

int wozRename(const char *from, const char *to)
{
  return current.rename ? current.rename(from, to, current.ctx) : -1;
}

int wozRemove(const char *path)
{
  return current.remove ? current.remove(path, current.ctx) : -1;
}

bool wozCanRename(void)
{
  return current.rename != NULL;
}
