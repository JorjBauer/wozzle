#ifndef __PRODOSSPECTOR_H
#define __PRODOSSPECTOR_H

#include <stdint.h>
#include <stddef.h>
#include "wozspector.h"
#include "vent.h"

class ProdosSpector : public Wozspector {
 public:
  ProdosSpector(bool verbose, uint8_t dumpflags);
  ~ProdosSpector();

  virtual uint32_t getFileContents(Vent *e, char **toWhere);
  virtual uint32_t getAllocatedByteCount(Vent *e) {
    return (uint32_t)e->getBlocksUsed() * 512;
  }
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
  // Free every data/index block belonging to a seedling or sapling entry.
  bool freeFileBlocks(const struct _prodosFent *fe);
  // Locate an entry by name within the directory whose key block is dirKey.
  // On success, returns the block holding it and the byte offset of the
  // entry within the flat trackData buffer.
  bool findEntryInDir(uint16_t dirKey, const char *name,
                      uint16_t *blockOut, uint32_t *offsetOut);
  // Resolve a (possibly slash-separated) path into the key block of the
  // directory that should contain its final component, plus that leaf name.
  // A leading '/' is optional and ignored (paths are volume-root relative).
  bool resolveDirAndLeaf(const char *path, uint16_t *dirKeyOut,
                         char *leaf, size_t leafSz);
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
