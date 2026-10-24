#include "wozmsg.h"
#include <stdio.h>
#include <stdlib.h>

static void defaultSink(wozMsgLevel level, const char *msg, void *ctx)
{
  (void)ctx;
  if (level >= WOZ_MSG_WARNING) {
    // Keep the two streams in order when they land in the same place.
    fflush(stdout);
    fprintf(stderr, "%s\n", msg);
  } else {
    printf("%s\n", msg);
  }
}

static wozMsgSink currentSink = defaultSink;
static void *currentCtx = NULL;

void wozSetMsgSink(wozMsgSink sink, void *ctx)
{
  currentSink = sink ? sink : defaultSink;
  currentCtx = sink ? ctx : NULL;
}

void wozMsgV(wozMsgLevel level, const char *fmt, va_list ap)
{
  char buf[512];
  va_list ap2;
  va_copy(ap2, ap);
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (len < 0) {
    va_end(ap2);
    return;
  }
  if ((size_t)len < sizeof(buf)) {
    currentSink(level, buf, currentCtx);
  } else {
    // Long enough to need the heap (a multi-line explanation, say)
    char *big = (char *)malloc(len + 1);
    if (big) {
      vsnprintf(big, len + 1, fmt, ap2);
      currentSink(level, big, currentCtx);
      free(big);
    }
  }
  va_end(ap2);
}

void wozMsg(wozMsgLevel level, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  wozMsgV(level, fmt, ap);
  va_end(ap);
}
