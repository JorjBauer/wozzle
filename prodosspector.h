#ifndef __PRODOSSPECTOR_H
#define __PRODOSSPECTOR_H

#include <stdint.h>
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
  virtual bool makeDirectory(const char *dirName);

  virtual void displayInfo();

  virtual void inspectFile(const char *fileName, Vent *fp);
  virtual bool probe();
protected:
  virtual Vent *createTree();

  uint16_t calculateBlocksFree();
  void printFreeBlocks();
  
  bool findFreeBlock(uint16_t *blockOut);
  bool flushFreeBlockList();
  bool readBlock(uint16_t blockNum, uint8_t dataOut[512]);
  bool writeBlock(uint16_t blockNum, uint8_t data[512]);
  bool addDirectoryEntryForFile(struct _prodosFent *e);

  // Mark a single block free in the in-RAM volume bitmap (no-op for the
  // boot blocks / out-of-range values). Call flushFreeBlockList() after.
  void markBlockFree(uint16_t block);
  // Free every data/index block belonging to a seedling or sapling entry.
  bool freeFileBlocks(const struct _prodosFent *fe);
  // Locate a directory entry by name in the root directory. On success,
  // returns the block holding it and the byte offset of the entry within
  // the flat trackData buffer.
  bool findDirEntry(const char *name, uint16_t *blockOut, uint32_t *offsetOut);
  // Tombstone the entry at trackData[offset] (within directory `block`) and
  // decrement the volume directory header's active file count.
  bool removeDirEntry(uint16_t block, uint32_t offset);
  
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
