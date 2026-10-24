#ifndef __WOZMSG_H
#define __WOZMSG_H

// Diagnostics from the disk image library.
//
// The library never writes to stdout or stderr itself. Every error,
// warning and trace message goes through wozMsg(), which hands it to
// whatever sink the program installed. The command line tools leave the
// default sink in place (it prints to the terminal); a GUI can install
// its own and show the messages however it likes.
//
// A message is one complete thought. It has no trailing newline (the sink
// adds one if it wants one), but it may contain embedded newlines when a
// message runs to several lines.

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  WOZ_MSG_DEBUG   = 0, // tracing; mostly emitted only in verbose mode
  WOZ_MSG_INFO    = 1, // normal progress the user would want to see
  WOZ_MSG_WARNING = 2, // something is off, but the operation carries on
  WOZ_MSG_ERROR   = 3  // the operation failed
} wozMsgLevel;

typedef void (*wozMsgSink)(wozMsgLevel level, const char *msg, void *ctx);

// Install a sink. NULL restores the default, which prints DEBUG and INFO
// to stdout and WARNING and ERROR to stderr.
void wozSetMsgSink(wozMsgSink sink, void *ctx);
// The sink and context installed now, so one can be put back afterward
void wozGetMsgSink(wozMsgSink *sink, void **ctx);

void wozMsg(wozMsgLevel level, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
void wozMsgV(wozMsgLevel level, const char *fmt, va_list ap);

// The most recent ERROR message, whatever sink is installed, so a caller
// that only learns "it failed" can ask why. "" if there hasn't been one
// since the last wozClearError(). Long messages are cut short.
const char *wozLastError(void);
void wozClearError(void);

#ifdef __cplusplus
};
#endif

#endif
