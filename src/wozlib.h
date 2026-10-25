#ifndef __WOZLIB_H
#define __WOZLIB_H

// A plain C interface to the library, for anything that can't use the
// C++ classes directly: a GUI in another language, a WebAssembly build, a
// Rust or Swift wrapper. It's a thin layer over wozops.h and the
// filesystem inspectors, and says nothing they don't.
//
// Conventions:
//   - Functions that can fail return false (or NULL, or -1). The reason
//     went through the message sink (wozmsg.h), and wozLastError() has it.
//   - Buffers handed back come from wozMalloc; release them with wozFree.
//   - Paths inside an image are volume-root relative, '/'-separated, and
//     only ProDOS has more than one directory. Paths to images are host
//     paths, opened through the wozhost.h hooks.
//   - Changes stay in memory until wozImageSave().
//   - One thread at a time (see wozhost.h).

#include <stdint.h>
#include <stdbool.h>

#include "woztypes.h"
#include "wozhost.h"
#include "wozmsg.h"

#ifdef __cplusplus
extern "C" {
#endif

// An open disk image and the filesystem on it
typedef struct wozImage wozImage;

// Load the CRC table the WOZ code needs. Call once before anything else.
void wozInit(void);

// ---- images ----

// Open an image as the given filesystem, or WOZ_FS_UNKNOWN to detect it.
// An image opened with a forced type that doesn't probe as that type is
// still opened (a damaged volume can still be worth reading); check
// wozImageProbe().
wozImage *wozImageOpen(const char *path, wozFsType type);
void wozImageClose(wozImage *img);

// Which filesystem a file at `path` holds, without keeping it open
wozFsType wozImageDetect(const char *path);

wozFsType wozImageFsType(const wozImage *img);
// Whether the image really looks like the filesystem it was opened as
bool wozImageProbe(wozImage *img);
// Whether there are changes not yet saved
bool wozImageIsDirty(wozImage *img);

// Write the image out. NULL means back where it came from. The format
// follows the extension (.woz, .dsk/.do, .po, .nib, .hdv/.img, .2mg), so this
// is also how an image is converted.
bool wozImageSave(wozImage *img, const char *path);

typedef struct {
  wozFsType fsType;
  // ProDOS and Pascal: the volume name. DOS 3.3: "".
  char volumeName[16];
  // DOS 3.3: the volume number. Otherwise 0.
  uint8_t dosVolume;
  // Allocation units: 512-byte blocks, or 256-byte sectors on DOS 3.3
  uint32_t unitSize;
  uint32_t totalUnits;
  uint32_t freeUnits;
  // From the image itself
  bool isHardDisk;        // a raw block image (.hdv, .img, a big .po)
  bool writeProtected;    // the WOZ INFO chunk's flag
  uint8_t wozVersion;     // WOZ version, as read or as it would be written
  uint8_t diskType;       // 1 = 5.25", 2 = 3.5"
  char creator[33];       // WOZ creator string
} wozVolumeInfo;

bool wozImageGetVolumeInfo(wozImage *img, wozVolumeInfo *out);

// ---- directories ----

typedef struct {
  char name[32];
  // The ProDOS file type. DOS 3.3 types are mapped onto ProDOS ones (T is
  // TXT, A is BAS, I is INT, B is BIN, S is SYS, R is REL). On Pascal
  // this is the Pascal file kind instead (2 CODE, 3 TEXT, 5 DATA, ...).
  uint8_t fileType;
  // The aux type: a BIN file's load address, say. DOS 3.3 gives what
  // ProDOS would (a B file's load address, $0801 for A). Pascal: 0.
  uint16_t auxType;
  // The length of the contents, as wozImageReadFile reads them
  uint32_t length;
  // Blocks used (sectors on DOS 3.3), index blocks included
  uint32_t unitsUsed;
  // ProDOS access bits (0x80 destroy, 0x40 rename, 0x20 backup,
  // 0x02 write, 0x01 read). Otherwise 0.
  uint8_t access;
  bool isDirectory;
  bool locked;
  // A GS/OS extended (forked) ProDOS file
  bool hasResourceFork;
  // Seconds since 1970 in local time, or -1 when the entry has none. Only
  // ProDOS keeps them.
  int64_t created;
  int64_t modified;
} wozEntry;

// The entries of a directory: NULL or "" for the root. Fills up to max
// entries and returns how many there are in all (so a short buffer can be
// sized and tried again), or -1 on error.
int wozImageList(wozImage *img, const char *dir, wozEntry *out, int max);

// One entry by path
bool wozImageStat(wozImage *img, const char *path, wozEntry *out);

// ---- files ----

// A file's contents: just its data, the same on every filesystem, so
// what wozImageWriteFile put in comes back out. (DOS 3.3 keeps a header in
// front of I, A and B files; it isn't part of the contents.)
// resourceFork: the resource fork of an extended ProDOS file instead.
// fullAllocation: everything the file has allocated on disk, read as it
// lies, DOS 3.3 headers included. An empty file gives *data NULL and *len 0.
bool wozImageReadFile(wozImage *img, const char *path, bool resourceFork,
                      bool fullAllocation, uint8_t **data, uint32_t *len);

// A BASIC program (Applesoft or Integer) as text, the way LIST shows it.
// *text is NUL-terminated; *len doesn't count the NUL. A malformed
// program gives what could be listed and returns false.
bool wozImageListBasic(wozImage *img, const char *path, char **text,
                       uint32_t *len);

// Write a file into the image, replacing one of the same name. fileType
// and auxType are ProDOS values, or -1 to take them from AppleSingle
// metadata or the name (.SYSTEM is SYS), falling back to BIN at $2000.
// An AppleSingle file brings its resource fork too (ProDOS only).
// tokenize: data is an Applesoft listing to store as a program.
bool wozImageWriteFile(wozImage *img, const char *path, const uint8_t *data,
                       uint32_t len, int fileType, int auxType,
                       bool tokenize);

// Delete a file, or with `recursive` a ProDOS directory and its contents
bool wozImageDelete(wozImage *img, const char *path, bool recursive);
// Delete an empty ProDOS directory
bool wozImageRemoveDirectory(wozImage *img, const char *path);
// Create a ProDOS directory
bool wozImageMakeDirectory(wozImage *img, const char *path);

// ---- volumes ----

bool wozImageRenameVolume(wozImage *img, const char *name);
// Install boot code (ProDOS or Pascal): from a donor image, or NULL for
// the embedded code
bool wozImageInstallBootBlocks(wozImage *img, const char *donorPath);
// Pascal: gather free space at the end of the volume, or right after
// afterFile if it isn't NULL
bool wozImageKrunch(wozImage *img, const char *afterFile);
// ProDOS: shrink a raw block volume to `blocks`, moving files down out of
// the way unless `relocate` is false
bool wozImageResize(wozImage *img, uint32_t blocks, bool relocate);

// A blank image written to `path`, which mustn't exist. volumeName may be
// NULL (BLANK); dosVolume is DOS 3.3's (1-254); blocks is ignored for
// DOS 3.3 (always 280). See wozCheckImageName() in wozops.h for which
// sizes need a hard-disk extension.
bool wozCreateBlankImage(const char *path, wozFsType type,
                         const char *volumeName, int dosVolume,
                         uint32_t blocks, bool bootable);

// Convert an image from one format to another, as wozzle does: the
// formats follow the extensions. It works below the filesystem, so a
// copy-protected disk converts as well as any other.
bool wozConvertImage(const char *inPath, const char *outPath);

// A size token ("140k", "800k", "32m", a block count) as blocks, or -1
int32_t wozParseSize(const char *token);

// ---- image to image ----

typedef void (*wozCopyProgress)(const char *path, uint32_t dataBytes,
                                uint32_t resourceBytes, void *ctx);

typedef struct {
  uint32_t files;
  uint32_t dirsMade;
  uint64_t bytes;
  // Files that already existed on the destination. Without force, any of
  // these stops the copy before it starts, and wozLastError() lists them.
  uint32_t collisions;
} wozCopyStats;

// Copy files between two images of the same filesystem, metadata intact.
// paths: what to copy (files or ProDOS subtrees), or npaths 0 for the
// whole volume. destDir: ProDOS only, put it all under this directory
// (NULL for none). dst changes in memory only; save it after a success.
bool wozImageCopy(wozImage *src, wozImage *dst, const char *const *paths,
                  int npaths, const char *destDir, bool force,
                  wozCopyProgress progress, void *ctx, wozCopyStats *stats);

#ifdef __cplusplus
};
#endif

#endif
