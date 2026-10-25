// The WebAssembly build's glue: the parts of the C interface (wozlib.h)
// that JavaScript can't use as they are. Structs come back as JSON, since
// reading C struct layouts from JS would break the first time a field
// moved, and library messages go to a JS callback instead of the console.
//
// Strings handed back come from wozMalloc; the JS side wozFree()s them.
// Everything else the JS wrapper (wozzle.mjs) calls straight from
// wozlib.h.

#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "wozlib.h"

using namespace std;

// Library messages go to Module.wozOnMessage(level, text), if it's set
EM_JS(void, jsMessage, (int level, const char *msg), {
  if (Module.wozOnMessage) Module.wozOnMessage(level, UTF8ToString(msg));
});

static void sink(wozMsgLevel level, const char *msg, void *ctx)
{
  (void)ctx;
  jsMessage((int)level, msg);
}

// JSON string escaping. Apple II names are 7-bit, but a WOZ creator string
// can hold anything, so bytes past 0x7F are passed as Latin-1 code points.
static void appendString(string &out, const char *s)
{
  out += '"';
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      out += '\\';
      out += (char)*p;
    } else if (*p < 0x20 || *p >= 0x7F) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", *p);
      out += esc;
    } else {
      out += (char)*p;
    }
  }
  out += '"';
}

static void appendNumber(string &out, long long v)
{
  char n[24];
  snprintf(n, sizeof(n), "%lld", v);
  out += n;
}

static void appendEntry(string &out, const wozEntry *e)
{
  out += "{\"name\":";
  appendString(out, e->name);
  out += ",\"fileType\":";
  appendNumber(out, e->fileType);
  out += ",\"auxType\":";
  appendNumber(out, e->auxType);
  out += ",\"length\":";
  appendNumber(out, e->length);
  out += ",\"unitsUsed\":";
  appendNumber(out, e->unitsUsed);
  out += ",\"access\":";
  appendNumber(out, e->access);
  out += ",\"isDirectory\":";
  out += e->isDirectory ? "true" : "false";
  out += ",\"locked\":";
  out += e->locked ? "true" : "false";
  out += ",\"hasResourceFork\":";
  out += e->hasResourceFork ? "true" : "false";
  out += ",\"created\":";
  appendNumber(out, e->created);
  out += ",\"modified\":";
  appendNumber(out, e->modified);
  out += '}';
}

static char *handBack(const string &s)
{
  char *p = (char *)wozMalloc(s.size() + 1);
  if (p) memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void wozJsInit(void)
{
  wozInit();
  wozSetMsgSink(sink, NULL);
}

EMSCRIPTEN_KEEPALIVE char *wozJsVolumeInfo(wozImage *img)
{
  wozVolumeInfo vi;
  if (!wozImageGetVolumeInfo(img, &vi)) return NULL;
  string out = "{\"fsType\":";
  appendNumber(out, vi.fsType);
  out += ",\"volumeName\":";
  appendString(out, vi.volumeName);
  out += ",\"dosVolume\":";
  appendNumber(out, vi.dosVolume);
  out += ",\"unitSize\":";
  appendNumber(out, vi.unitSize);
  out += ",\"totalUnits\":";
  appendNumber(out, vi.totalUnits);
  out += ",\"freeUnits\":";
  appendNumber(out, vi.freeUnits);
  out += ",\"isHardDisk\":";
  out += vi.isHardDisk ? "true" : "false";
  out += ",\"writeProtected\":";
  out += vi.writeProtected ? "true" : "false";
  out += ",\"wozVersion\":";
  appendNumber(out, vi.wozVersion);
  out += ",\"diskType\":";
  appendNumber(out, vi.diskType);
  out += ",\"creator\":";
  appendString(out, vi.creator);
  out += ",\"sectorsPerTrack\":";
  appendNumber(out, vi.sectorsPerTrack);
  out += '}';
  return handBack(out);
}

EMSCRIPTEN_KEEPALIVE char *wozJsList(wozImage *img, const char *dir)
{
  int n = wozImageList(img, dir, NULL, 0);
  if (n < 0) return NULL;
  wozEntry *ents = n ? (wozEntry *)wozMalloc(n * sizeof(wozEntry)) : NULL;
  if (n && !ents) return NULL;
  if (n) wozImageList(img, dir, ents, n);
  string out = "[";
  for (int i = 0; i < n; i++) {
    if (i) out += ',';
    appendEntry(out, &ents[i]);
  }
  out += ']';
  wozFree(ents);
  return handBack(out);
}

EMSCRIPTEN_KEEPALIVE char *wozJsStat(wozImage *img, const char *path)
{
  wozEntry e;
  if (!wozImageStat(img, path, &e)) return NULL;
  string out;
  appendEntry(out, &e);
  return handBack(out);
}

// wozImageCopy's statistics, as JSON. Also returns them when the copy
// fails, so the caller can see how many files collided.
EMSCRIPTEN_KEEPALIVE char *wozJsCopy(wozImage *src, wozImage *dst,
                                     const char *const *paths, int npaths,
                                     const char *destDir, bool force)
{
  wozCopyStats st;
  memset(&st, 0, sizeof(st));
  bool ok = wozImageCopy(src, dst, paths, npaths, destDir, force, NULL, NULL,
                         &st);
  string out = "{\"ok\":";
  out += ok ? "true" : "false";
  out += ",\"files\":";
  appendNumber(out, st.files);
  out += ",\"dirsMade\":";
  appendNumber(out, st.dirsMade);
  out += ",\"bytes\":";
  appendNumber(out, (long long)st.bytes);
  out += ",\"collisions\":";
  appendNumber(out, st.collisions);
  out += '}';
  return handBack(out);
}

}
