#ifndef __PRODOSSPECTOR_H
#define __PRODOSSPECTOR_H

#include <stdint.h>
#include <stddef.h>
#include "wozspector.h"
#include "vent.h"

// ProDOS storage type 5 is an "extended" file: the GS/OS forked file. Its
// key block is neither data nor an index but an extended key block holding
// two mini-entries - one per fork - each describing an ordinary
// seedling/sapling/tree structure of its own.
#define PRODOS_STORAGE_EXTENDED 0x05

// One fork of an extended file, as described by its mini-entry. A fork
// GS/OS never wrote is absent, and reads back with storageType 0.
struct ProdosFork {
  uint8_t storageType;  // seedling/sapling/tree, for this fork alone
  uint16_t keyBlock;
  uint16_t blocksUsed;
  uint32_t eof;
};

class ProdosSpector : public Wozspector {
 public:
  ProdosSpector(bool verbose, uint8_t dumpflags);
  ~ProdosSpector();

  virtual uint32_t getFileContents(Vent *e, char **toWhere);
  virtual uint32_t getAllocatedByteCount(Vent *e);

  // True if `e` is an extended (forked) file. Its getFileContents() and
  // getEofLength() describe the data fork only; the resource fork is
  // reachable through getForkContents().
  static bool isExtendedFile(Vent *e);

  // Contents of one fork of an extended file. For a file that isn't
  // extended, the data fork is the whole file and the resource fork is
  // empty. Malloc()s into *toWhere like getFileContents(); returns the
  // length (0, with *toWhere NULL, for an absent or empty fork).
  uint32_t getForkContents(Vent *e, bool resourceFork, char **toWhere);

  // Describe one fork of an extended file without reading its contents.
  bool getForkInfo(Vent *e, bool resourceFork, struct ProdosFork *out);

  // Copy out an extended file's raw extended key block. The bytes past the
  // two mini-entries carry GS/OS Finder information, which an image-to-
  // image copy preserves verbatim.
  bool readExtendedKeyBlock(Vent *e, uint8_t out[512]);
  virtual uint8_t applesoftHeaderBytes() { return 0; };

  virtual bool writeFileToImage(uint8_t *fileContents,
                                char *fileName,
                                uint8_t fileType,
                                uint16_t auxTypeData,
                                uint32_t fileSize);

  virtual bool removeFile(const char *fileName);
  virtual bool removeDirectory(const char *dirName);
  virtual bool removeDirectoryRecursive(const char *dirName);
  virtual bool makeDirectory(const char *dirName);
  virtual Vent *findEntry(const char *path);
  virtual void displayDirectory(const char *path);

  virtual void displayInfo();

  virtual void inspectFile(const char *fileName, Vent *fp);
  virtual bool probe();
  virtual bool writeBootBlocks(const uint8_t block0[512],
                               const uint8_t block1[512]);
  // Read this volume's boot blocks (e.g. to use it as a boot-code donor).
  bool readBootBlocks(uint8_t block0[512], uint8_t block1[512]);
  virtual bool renameVolume(const char *newName);
  // Shrink the volume (and the image file behind it) to `newBlocks`,
  // rewriting the volume directory header and bitmap. Raw block images
  // only, and shrinking only. With `relocate`, blocks still allocated at
  // or past the new end are moved into free space below it and every
  // pointer to them corrected; without it (or when there isn't enough
  // free space below), the resize is refused and nothing changes.
  bool resizeVolume(uint32_t newBlocks, bool relocate);
  // Total blocks the volume directory header currently claims.
  uint32_t volumeBlockCount() const { return numBlocksTotal; }
  // Write a file preserving the metadata (type, aux, access, dates,
  // versions) of an existing directory entry - for image-to-image copies.
  bool writeFileWithMeta(uint8_t *fileContents, const char *fileName,
                         uint32_t fileSize,
                         const struct _prodosFent *meta);
  // Create an extended (forked) file: both forks are stored with ordinary
  // seedling/sapling/tree structures and described by mini-entries in a
  // fresh extended key block. Metadata comes from `meta` exactly as in
  // writeFileWithMeta. `srcKeyBlock`, when non-NULL, is a donor extended
  // key block whose Finder-information bytes (everything outside the two
  // mini-entries) are carried over; the pointers in it are ignored.
  bool writeExtendedFileWithMeta(const uint8_t *dataFork, uint32_t dataLen,
                                 const uint8_t *resFork, uint32_t resLen,
                                 const char *fileName,
                                 const struct _prodosFent *meta,
                                 const uint8_t *srcKeyBlock);
protected:
  virtual Vent *createTree();

  uint16_t calculateBlocksFree();
  void printFreeBlocks();
  
  bool findFreeBlock(uint16_t *blockOut);
  bool flushFreeBlockList();
  bool readBlock(uint16_t blockNum, uint8_t dataOut[512]);
  bool writeBlock(uint16_t blockNum, uint8_t data[512]);
  // True iff block b lies wholly inside the loaded image buffer. Corrupt
  // directory chains and index blocks can point anywhere; every walker
  // must check before indexing trackData by block number.
  bool validBlock(uint16_t b);
  // Add an entry to the directory whose key (first) block is dirKey.
  bool addDirectoryEntryForFile(uint16_t dirKey, struct _prodosFent *e);
  // Append a fresh block to a full subdirectory's chain (whose current tail
  // is `lastBlock`) and store entry `e` in it. Updates the directory's file
  // count and its own size (blocksUsed/EOF) in the parent. Refuses to grow
  // the volume directory, which is fixed-size in ProDOS.
  bool growDirectoryForEntry(uint16_t dirKey, uint16_t lastBlock,
                             struct _prodosFent *e);
  // Re-read the volume bitmap from trackData, discarding un-flushed in-RAM
  // allocations. Used to roll back after a write fails partway through.
  void reloadFreeBlockBitmap();

  // Mark a single block free in the in-RAM volume bitmap (no-op for the
  // boot blocks / out-of-range values). Call flushFreeBlockList() after.
  void markBlockFree(uint16_t block);
  // Free every data/index block belonging to an entry, forks included.
  bool freeFileBlocks(const struct _prodosFent *fe);
  // Free the blocks of one seedling/sapling/tree structure rooted at `kp`.
  // (freeFileBlocks handles the directory-entry level on top of this.)
  bool freeForkBlocks(uint8_t storageType, uint16_t kp);
  // Read one seedling/sapling/tree structure of `len` bytes rooted at
  // `kp` into a fresh malloc()ed buffer. `name` only labels warnings about
  // pointers that leave the volume.
  uint32_t readForkBlocks(uint8_t storageType, uint16_t kp, uint32_t len,
                          const char *name, char **toWhere);
  // Store `size` bytes as a seedling, sapling, or tree, reporting which
  // shape was used, the key block naming it, and the blocks consumed
  // (data plus index). On failure the caller must reloadFreeBlockBitmap().
  bool writeForkBlocks(const uint8_t *contents, uint32_t size,
                       uint8_t *storageTypeOut, uint16_t *keyBlockOut,
                       uint16_t *blocksUsedOut);
  // Highest block number a seedling/sapling/tree (or, for storage type 5,
  // an extended file and both its forks) references, index blocks
  // included. 0 if the structure can't be read.
  uint32_t highestBlockInFork(uint8_t storageType, uint16_t kp);
  // Look for any directory or file block at or past `limit`. Returns true
  // and describes the first one found in `what`.
  bool findBlockUseAtOrAbove(uint32_t limit, char *what, size_t whatSz);
  // Move every allocated block at or past `limit` into free space below
  // it, repointing everything that named them, and report how many moved.
  // Refuses without changing anything if there isn't room below.
  bool relocateBlocksBelow(uint32_t limit, uint32_t *movedOut);
  // Index every stored block pointer in the volume, so relocation knows
  // what to correct. `refsOut` is a RefMap (an implementation detail of
  // prodosspector.cpp, hence void * here rather than dragging the STL
  // container into this header).
  void collectBlockRefs(void *refsOut);
  void collectForkRefs(void *refsOut, uint8_t storageType, uint16_t kp);
  // Read/write one mini-entry (fork descriptor) inside an extended key
  // block image held in `keyBlockData`.
  static void readForkEntry(const uint8_t keyBlockData[512], bool resourceFork,
                            struct ProdosFork *out);
  static void writeForkEntry(uint8_t keyBlockData[512], bool resourceFork,
                             const struct ProdosFork *f);
  // Locate an entry by name within the directory whose key block is dirKey.
  // On success, returns the block holding it and the byte offset of the
  // entry within the flat trackData buffer.
  bool findEntryInDir(uint16_t dirKey, const char *name,
                      uint16_t *blockOut, uint32_t *offsetOut);
  // Resolve a (possibly slash-separated) path into the key block of the
  // directory that should contain its final component, plus that leaf name.
  // A leading '/' is optional and ignored (paths are volume-root relative).
  // With quiet, a failed resolution reports nothing (for existence probes).
  bool resolveDirAndLeaf(const char *path, uint16_t *dirKeyOut,
                         char *leaf, size_t leafSz, bool quiet = false);
  // Tombstone the entry at trackData[offset] (within directory block
  // `block`) and decrement the active file count in the header of the
  // directory whose key block is dirKey.
  bool removeDirEntry(uint16_t dirKey, uint16_t block, uint32_t offset);
  
private:
  Vent *descendTree(uint16_t fromBlock);

private:
  // Flat block-indexed buffer: block N lives at trackData[N*512 ..
  // (N+1)*512 - 1]. Sized to the loaded image, so a 140 KB floppy
  // allocates 280 blocks and a 32 MB HDV allocates 65536.
  uint8_t *trackData;
  uint32_t trackDataBytes;

  // Volume bitmap. On a 140 KB floppy this fits in one 512-byte block,
  // but a 32 MB HDV needs up to 16 contiguous bitmap blocks (1 bit per
  // block, ceil(numBlocksTotal/4096) blocks total). Heap-allocated and
  // sized when we learn numBlocksTotal from the volume directory header.
  uint8_t *freeBlockBitmap;
  uint32_t freeBlockBitmapBytes;
  uint16_t volBitmapBlock;
  uint32_t numBlocksTotal;  // widened: HDVs can exceed 65535

  bool loadBlockBuffer();
};

#endif
