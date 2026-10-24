#ifndef __WOZOPS_H
#define __WOZOPS_H

// The operations behind the command line tools, as functions any front
// end can call: opening an image as a filesystem, formatting a blank one,
// copying files in and out, installing boot code, and wozmosis's
// image-to-image copy.
//
// Nothing here prints. Errors and warnings go through wozMsg (the text is
// what the tools have always said), and anything the tools show on
// success is handed back to the caller to present. Buffers come from
// wozMalloc, and the caller wozFree()s them.
//
// None of this touches the host's filesystem except through the wozhost
// hooks, by way of Woz::readFile and writeFile. Where a tool reads or
// writes a plain host file (cpin's source, cpout's destination, a new
// blank image), the bytes are passed in or handed back, and the tool does
// the file I/O itself.

#include <stdint.h>
#include <string>
#include <vector>

#include "woztypes.h"
#include "wozspector.h"
#include "basiclisting.h"

// ---- filesystems ----

// "DOS 3.3", "ProDOS", "Pascal", or "unknown"
const char *wozFsName(wozFsType type);

// A new, empty inspector for the given filesystem (NULL for
// WOZ_FS_UNKNOWN). The caller deletes it.
Wozspector *wozNewSpector(wozFsType type, bool verbose);

// Read the image at `path` and find which filesystem it holds, trying
// ProDOS, then DOS 3.3, then Pascal. WOZ_FS_UNKNOWN if none of them
// probes as a match (or the image can't be read).
wozFsType wozDetectFs(const char *path);

// Open the image at `path` as the given filesystem; WOZ_FS_UNKNOWN
// detects it first. NULL if the image can't be read or (when detecting)
// holds nothing recognizable. A forced type isn't probed, so the tools
// can open an image the way they're told and warn instead of refusing.
Wozspector *wozOpenFs(const char *path, wozFsType type, bool verbose);

// ---- blank images ----

// A format-size token: a named preset (140k, 800k, 4m, 5m, 8m, 10m, 16m,
// 32m, any case) or a bare ProDOS block count 0-65535. -1 if unparseable.
int32_t wozParseSizeSpec(const char *tok);

// Validate and upper-case a ProDOS volume name into out[16]: 1-15
// characters, a letter first, then letters, digits or periods. Says why
// through wozMsg on failure. (Pascal's rule is
// PascalSpector::normalizeVolumeName.)
bool wozNormalizeProdosVolumeName(const char *src, char out[16]);

struct WozFormatOptions {
  wozFsType type;
  // ProDOS and Pascal: the volume name, not yet normalized (NULL means
  // BLANK). DOS 3.3 ignores it.
  const char *volumeName;
  // DOS 3.3: the volume number, 1-254. Anything else becomes 254.
  int dosVolume;
  // ProDOS and Pascal: the size in 512-byte blocks. DOS 3.3 is always 280.
  uint32_t blocks;
  // ProDOS and Pascal: install the embedded boot code
  bool bootable;
};

// Whether a new image of `blocks` blocks of the given filesystem can be
// named `fname`. The floppy loaders only take a floppy's worth of blocks,
// so other sizes need a hard-disk extension (.hdv, .img). Says why through
// wozMsg when it can't, and warns about a Pascal volume past 16MB.
bool wozCheckImageName(wozFsType type, const char *fname, uint32_t blocks);

// Build a blank image into a fresh buffer (*imgOut, *sizeOut bytes of
// DOS-order sectors for DOS 3.3, ProDOS-order blocks otherwise). The
// volume name actually used, normalized, lands in volumeNameOut, and the
// DOS volume number in *dosVolumeOut; either may be NULL.
bool wozBuildBlankImage(const WozFormatOptions *opts, uint8_t **imgOut,
                        uint32_t *sizeOut, char volumeNameOut[16],
                        int *dosVolumeOut);

// ---- boot blocks ----

// ProDOS boot code: from a donor ProDOS image if `donor` names one, else
// the embedded standard boot block (identical from ProDOS 1.1 through 2.4)
// with a zero block 1.
bool wozLoadProdosBootBlocks(const char *donor, uint8_t b0[512],
                             uint8_t b1[512]);

// p-System boot code: from a donor Pascal image if `donor` names one, else
// the embedded bootstrap that suits a volume of totalBlocks blocks (a
// 5.25" volume boots through the Disk II and needs both blocks; anything
// bigger boots through a block device and fits in block 0).
bool wozLoadPascalBootBlocks(const char *donor, uint32_t totalBlocks,
                             uint8_t b0[512], uint8_t b1[512]);

// Install boot code on an open ProDOS or Pascal volume, as wozit's
// bootblocks command does. `donor` may be NULL for the embedded code.
bool wozInstallBootBlocks(Wozspector *fs, const char *donor);

// ---- files in and out ----

struct WozImportOptions {
  // The host file is an Applesoft listing to tokenize
  bool tokenize;
  // A ProDOS file type and aux type that win over everything else.
  // Otherwise they come from AppleSingle metadata, then a .SYSTEM name
  // (SYS at $2000), then BIN at $2000.
  bool explicitType;
  uint8_t fileType;
  bool explicitAux;
  uint16_t auxType;
  // Only labels messages (the host file's name, usually); may be NULL
  const char *sourceName;
};

// Copy a host file's bytes into the image as `destName` (a ProDOS path
// may name a subdirectory), as wozit's cpin does: an AppleSingle file
// brings its type, aux type and (ProDOS only) resource fork with it, and
// an existing file of that name is replaced.
bool wozImportFile(Wozspector *fs, const uint8_t *data, uint32_t len,
                   const char *destName, const WozImportOptions *opts);

struct WozExportOptions {
  // ProDOS: the resource fork of a GS/OS extended file, not its data fork
  bool resourceFork;
  // The file's whole on-disk allocation rather than the length its
  // directory entry (or in-file header) gives
  bool fullAllocation;
};

// A file's contents, as wozit's cpout copies them. On success *out is a
// fresh buffer (NULL for an empty file) and *lenOut its length. Warns
// through wozMsg when the allocation is far bigger than the length copied.
// `label` names the file in messages (NULL means its leaf name).
bool wozExportFile(Wozspector *fs, Vent *e, const WozExportOptions *opts,
                   const char *label, uint8_t **out, uint32_t *lenOut);

// True for an Applesoft or Integer BASIC program
bool wozIsBasicFile(Vent *e);

// Detokenize a BASIC program read from `fs` (its bytes, as from
// wozExportFile). On a malformed program, *out holds the lines read before
// the problem and the return value is false.
bool wozDetokenize(Wozspector *fs, Vent *e, const uint8_t *data,
                   uint32_t len, BasicListing *out);

// A listing as text, the way LIST shows it (and cpout -B writes it):
// Integer BASIC right-aligns its line numbers.
std::string wozFormatBasicListing(const BasicListing &lines,
                                  bool integerBasic);

// ---- image to image ----

struct WozCopyOptions {
  // Files or (ProDOS) directory subtrees to copy, volume-root relative.
  // Empty copies the whole volume.
  std::vector<std::string> selections;
  // ProDOS: put everything under this directory, creating it as needed
  std::string destDir;
  // Replace files that already exist on the destination
  bool force;
  // Called after each file is copied, if set
  void (*progress)(const char *path, uint32_t dataBytes,
                   uint32_t resourceBytes, bool extended, bool replaced,
                   void *ctx);
  void *progressCtx;

  WozCopyOptions() : force(false), progress(NULL), progressCtx(NULL) {}
};

struct WozCopyResult {
  size_t files;
  size_t dirsMade;
  uint64_t bytes;
  // Files that already exist on the destination. Without force these
  // stop the copy before anything changes, and the caller decides how to
  // put the question to the user; nothing is said through wozMsg.
  std::vector<std::string> collisions;
};

// Normalize a destination directory for WozCopyOptions::destDir the way
// wozCopyFiles will: volume-root relative, no leading or trailing
// slashes, upper case. Says why through wozMsg if it isn't a usable
// ProDOS path. Lets a front end check it before opening anything.
bool wozNormalizeDestDir(std::string *dir);

// Copy files between two open images of the same filesystem with their
// metadata intact, as wozmosis does. Nothing is copied if a selection
// isn't found or a file collides (without force). A failure partway
// through leaves `dst` changed in memory, so the caller writes it out only
// on success.
bool wozCopyFiles(Wozspector *src, Wozspector *dst,
                  const WozCopyOptions *opts, WozCopyResult *result);

#endif
