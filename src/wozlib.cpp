#include "wozlib.h"
#include "wozops.h"
#include "crc32.h"
#include "dosspector.h"
#include "prodosspector.h"
#include "pascalspector.h"

#include <string.h>

#include <string>
#include <vector>

using namespace std;

struct wozImage {
  Wozspector *fs;
  string path;
};

void wozInit(void)
{
  preload_crc();
}

// ---- images ----

wozImage *wozImageOpen(const char *path, wozFsType type)
{
  Wozspector *fs = wozOpenFs(path, type, false);
  if (!fs) return NULL;
  wozImage *img = new wozImage;
  img->fs = fs;
  img->path = path;
  return img;
}

void wozImageClose(wozImage *img)
{
  if (!img) return;
  delete img->fs;
  delete img;
}

wozFsType wozImageDetect(const char *path)
{
  return wozDetectFs(path);
}

wozFsType wozImageFsType(const wozImage *img)
{
  return img->fs->fsType();
}

bool wozImageProbe(wozImage *img)
{
  return img->fs->probe();
}

bool wozImageIsDirty(wozImage *img)
{
  return img->fs->isDirty();
}

bool wozImageSave(wozImage *img, const char *path)
{
  return img->fs->writeFile(path ? path : img->path.c_str());
}

bool wozImageGetVolumeInfo(wozImage *img, wozVolumeInfo *out)
{
  memset(out, 0, sizeof(*out));
  Wozspector *fs = img->fs;
  out->fsType = fs->fsType();

  const diskInfo *di = fs->getDiskInfo();
  out->isHardDisk = fs->isHdv();
  out->writeProtected = fs->isWriteProtected();
  out->wozVersion = di->version;
  out->diskType = di->diskType;
  memcpy(out->creator, di->creator, sizeof(out->creator));
  out->creator[sizeof(out->creator) - 1] = '\0';

  switch (out->fsType) {
  case WOZ_FS_DOS33: {
    DosSpector *d = (DosSpector *)fs;
    bool used[35][16];
    if (!d->getSectorUsage(used))
      return false;
    out->dosVolume = d->volumeNumber();
    out->unitSize = 256;
    out->totalUnits = 35 * 16;
    for (int t = 0; t < 35; t++)
      for (int s = 0; s < 16; s++)
        if (!used[t][s]) out->freeUnits++;
    return true;
  }
  case WOZ_FS_PRODOS: {
    ProdosSpector *p = (ProdosSpector *)fs;
    Vent *tree = p->getTree();
    if (!tree || !tree->isHeader()) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: can't read the volume directory");
      return false;
    }
    strncpy(out->volumeName, tree->getName(), sizeof(out->volumeName) - 1);
    out->unitSize = 512;
    out->totalUnits = p->volumeBlockCount();
    out->freeUnits = p->calculateBlocksFree();
    return true;
  }
  case WOZ_FS_PASCAL: {
    PascalVolumeInfo vi;
    if (!((PascalSpector *)fs)->getVolumeInfo(&vi))
      return false;
    strncpy(out->volumeName, vi.name, sizeof(out->volumeName) - 1);
    out->unitSize = 512;
    out->totalUnits = vi.totalBlocks;
    out->freeUnits = vi.freeBlocks;
    return true;
  }
  default:
    return false;
  }
}

// ---- directories ----

static void fillEntry(Wozspector *fs, Vent *e, wozEntry *out)
{
  memset(out, 0, sizeof(*out));
  strncpy(out->name, e->getName(), sizeof(out->name) - 1);
  out->fileType = e->getFileType();
  out->unitsUsed = e->getBlocksUsed();
  out->isDirectory = e->isDirectory();
  out->created = -1;
  out->modified = -1;

  switch (fs->fsType()) {
  case WOZ_FS_DOS33: {
    out->locked = (e->getDosFent()->fileTypeAndFlags & 0x80) != 0;
    out->auxType = ((DosSpector *)fs)->getLoadAddress(e);
    // DOS keeps no length in the catalog, and a T file's ends at its
    // first $00, so the only sure way to the length is to read it
    char *dat = NULL;
    out->length = fs->getFileContents(e, &dat);
    wozFree(dat);
    break;
  }
  case WOZ_FS_PRODOS:
    out->auxType = e->getAuxType();
    out->length = e->getEofLength();
    out->access = e->getAccessFlags();
    // What ProDOS's CAT shows as locked: no write, rename or destroy
    out->locked = (out->access & 0xC2) == 0;
    out->hasResourceFork = ProdosSpector::isExtendedFile(e);
    if (e->getCreationDate() != NO_DATE) out->created = e->getCreationDate();
    if (e->getLastModified() != NO_DATE) out->modified = e->getLastModified();
    break;
  default:
    out->length = e->getEofLength();
    break;
  }
}

int wozImageList(wozImage *img, const char *dir, wozEntry *out, int max)
{
  Wozspector *fs = img->fs;
  bool root = !dir || !dir[0] || !strcmp(dir, "/");
  Vent *list = NULL;
  Vent *first;
  if (root) {
    first = fs->getTree();
    // ProDOS's tree is flat, every directory's entries after a header
    // node of its own, so the root is the first group
    if (fs->fsType() == WOZ_FS_PRODOS) {
      if (!first) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: can't read the volume directory");
        return -1;
      }
      first = first->nextEnt();
    }
  } else {
    list = fs->getDirectoryListing(dir);
    if (!list) return -1;
    first = list->isHeader() ? list->nextEnt() : list;
  }

  int n = 0;
  for (Vent *p = first; p && !p->isHeader(); p = p->nextEnt()) {
    if (n < max && out) fillEntry(fs, p, &out[n]);
    n++;
  }
  if (list) fs->freeListing(list);
  return n;
}

bool wozImageStat(wozImage *img, const char *path, wozEntry *out)
{
  Vent *e = img->fs->findEntry(path);
  if (!e) {
    wozMsg(WOZ_MSG_ERROR, "'%s' not found", path);
    return false;
  }
  fillEntry(img->fs, e, out);
  return true;
}

// ---- files ----

static Vent *findFile(wozImage *img, const char *path)
{
  Vent *e = img->fs->findEntry(path);
  if (!e) {
    wozMsg(WOZ_MSG_ERROR, "File '%s' not found", path);
    return NULL;
  }
  if (e->isDirectory()) {
    wozMsg(WOZ_MSG_ERROR, "'%s' is a directory", path);
    return NULL;
  }
  return e;
}

bool wozImageReadFile(wozImage *img, const char *path, bool resourceFork,
                      bool fullAllocation, uint8_t **data, uint32_t *len)
{
  *data = NULL;
  *len = 0;
  Vent *e = findFile(img, path);
  if (!e) return false;
  WozExportOptions xo;
  xo.resourceFork = resourceFork;
  xo.fullAllocation = fullAllocation;
  return wozExportFile(img->fs, e, &xo, path, data, len);
}

bool wozImageListBasic(wozImage *img, const char *path, char **text,
                       uint32_t *len)
{
  *text = NULL;
  *len = 0;
  Vent *e = findFile(img, path);
  if (!e) return false;
  if (!wozIsBasicFile(e)) {
    wozMsg(WOZ_MSG_ERROR, "File is not BASIC (Applesoft or Integer)");
    return false;
  }

  char *dat = NULL;
  uint32_t s = img->fs->getFileContents(e, &dat);
  BasicListing lines;
  bool ok = s && dat && wozDetokenize(img->fs, e, (uint8_t *)dat, s, &lines);
  wozFree(dat);
  if (!s) {
    wozMsg(WOZ_MSG_ERROR, "File is empty");
    return false;
  }
  if (!ok)
    wozMsg(WOZ_MSG_ERROR, "Failed to list file");

  string listing = wozFormatBasicListing(lines, e->getFileType() == FT_INT);
  *text = (char *)wozMalloc(listing.size() + 1);
  if (!*text) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: out of memory");
    return false;
  }
  memcpy(*text, listing.c_str(), listing.size() + 1);
  *len = (uint32_t)listing.size();
  return ok;
}

bool wozImageWriteFile(wozImage *img, const char *path, const uint8_t *data,
                       uint32_t len, int fileType, int auxType,
                       bool tokenize)
{
  WozImportOptions io;
  io.tokenize = tokenize;
  io.explicitType = (fileType >= 0);
  io.fileType = (uint8_t)(fileType >= 0 ? fileType : 0);
  io.explicitAux = (auxType >= 0);
  io.auxType = (uint16_t)(auxType >= 0 ? auxType : 0);
  io.sourceName = NULL;
  return wozImportFile(img->fs, data, len, path, &io);
}

bool wozImageDelete(wozImage *img, const char *path, bool recursive)
{
  if (recursive) {
    Vent *e = img->fs->findEntry(path);
    if (e && e->isDirectory())
      return img->fs->removeDirectoryRecursive(path);
  }
  return img->fs->removeFile(path);
}

bool wozImageRemoveDirectory(wozImage *img, const char *path)
{
  return img->fs->removeDirectory(path);
}

bool wozImageMakeDirectory(wozImage *img, const char *path)
{
  return img->fs->makeDirectory(path);
}

// ---- volumes ----

bool wozImageRenameVolume(wozImage *img, const char *name)
{
  // Pascal checks its own (different) rules; DOS 3.3 says it can't
  if (img->fs->fsType() != WOZ_FS_PRODOS)
    return img->fs->renameVolume(name);
  char vol[16];
  if (!wozNormalizeProdosVolumeName(name, vol))
    return false;
  return img->fs->renameVolume(vol);
}

bool wozImageInstallBootBlocks(wozImage *img, const char *donorPath)
{
  return wozInstallBootBlocks(img->fs, donorPath);
}

bool wozImageKrunch(wozImage *img, const char *afterFile)
{
  return img->fs->krunch(afterFile);
}

bool wozImageResize(wozImage *img, uint32_t blocks, bool relocate)
{
  if (img->fs->fsType() != WOZ_FS_PRODOS) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: resize is ProDOS-only");
    return false;
  }
  return ((ProdosSpector *)img->fs)->resizeVolume(blocks, relocate);
}

bool wozCreateBlankImage(const char *path, wozFsType type,
                         const char *volumeName, int dosVolume,
                         uint32_t blocks, bool bootable)
{
  if (type == WOZ_FS_DOS33) blocks = 280;
  if (!wozCheckImageName(type, path, blocks))
    return false;

  WozFormatOptions fo;
  fo.type = type;
  fo.volumeName = volumeName;
  fo.dosVolume = dosVolume;
  fo.blocks = blocks;
  fo.bootable = bootable;
  uint8_t *imgData = NULL;
  uint32_t size = 0;
  if (!wozBuildBlankImage(&fo, &imgData, &size, NULL, NULL))
    return false;

  int fd = wozOpen(path, WOZ_OPEN_READ);
  if (fd != -1) {
    wozClose(fd);
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' already exists; refusing to "
           "overwrite", path);
    wozFree(imgData);
    return false;
  }
  fd = wozOpen(path, WOZ_OPEN_CREATE);
  bool ok = (fd != -1);
  if (ok) {
    ok = (wozWrite(fd, imgData, size) == (long)size);
    ok = (wozClose(fd) == 0) && ok;
  }
  wozFree(imgData);
  if (!ok)
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write '%s'", path);
  return ok;
}

int32_t wozParseSize(const char *token)
{
  return wozParseSizeSpec(token);
}

// ---- image to image ----

struct ProgressRelay {
  wozCopyProgress cb;
  void *ctx;
};

static void relayProgress(const char *path, uint32_t dataBytes,
                          uint32_t resourceBytes, bool extended,
                          bool replaced, void *ctx)
{
  (void)extended; (void)replaced;
  ProgressRelay *r = (ProgressRelay *)ctx;
  r->cb(path, dataBytes, resourceBytes, r->ctx);
}

bool wozImageCopy(wozImage *src, wozImage *dst, const char *const *paths,
                  int npaths, const char *destDir, bool force,
                  wozCopyProgress progress, void *ctx, wozCopyStats *stats)
{
  WozCopyOptions opts;
  for (int i = 0; i < npaths; i++)
    opts.selections.push_back(paths[i]);
  if (destDir) opts.destDir = destDir;
  opts.force = force;
  ProgressRelay relay = { progress, ctx };
  if (progress) {
    opts.progress = relayProgress;
    opts.progressCtx = &relay;
  }

  WozCopyResult r;
  bool ok = wozCopyFiles(src->fs, dst->fs, &opts, &r);
  if (!ok && !r.collisions.empty() && !force) {
    string msg = "ERROR: these files already exist on the destination:";
    for (size_t i = 0; i < r.collisions.size(); i++)
      msg += "\n  " + r.collisions[i];
    wozMsg(WOZ_MSG_ERROR, "%s", msg.c_str());
  }
  if (stats) {
    stats->files = (uint32_t)r.files;
    stats->dirsMade = (uint32_t)r.dirsMade;
    stats->bytes = r.bytes;
    stats->collisions = (uint32_t)r.collisions.size();
  }
  return ok;
}
