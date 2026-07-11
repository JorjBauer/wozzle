#ifndef __PASCALSPECTOR_H
#define __PASCALSPECTOR_H

#include <stdint.h>
#include <stddef.h>
#include "wozspector.h"
#include "vent.h"

// UCSD p-System / Apple Pascal filesystem inspector.
//
// Pascal disks use 512-byte blocks in the same interleave as ProDOS, so
// the block buffer is loaded exactly the way ProdosSpector does it. The
// filesystem itself is much simpler: a single flat directory occupying
// blocks 2..5 (2048 bytes), every file stored on a run of contiguous
// blocks with a "bytes used in the last block" count. There is no
// allocation bitmap - free space is just the gaps between files.
class PascalSpector : public Wozspector {
 public:
  PascalSpector(bool verbose, uint8_t dumpflags);
  ~PascalSpector();

  virtual uint32_t getFileContents(Vent *e, char **toWhere);
  virtual uint32_t getAllocatedByteCount(Vent *e);
  virtual uint32_t getFileAllocation(Vent *e, char **toWhere);
  virtual uint8_t applesoftHeaderBytes() { return 0; }

  virtual bool writeFileToImage(uint8_t *fileContents,
                                char *fileName,
                                uint8_t fileType, // ProDOS file type
                                uint16_t auxTypeData,
                                uint32_t fileSize);

  virtual bool removeFile(const char *fileName);
  virtual bool renameVolume(const char *newName);
  virtual bool krunch(const char *afterFile);

  // Case-insensitive leaf-name lookup (Pascal is case-insensitive and
  // stores names upper-cased).
  virtual Vent *findEntry(const char *path);

  virtual void displayInfo();
  virtual void inspectFile(const char *fileName, Vent *fp);
  virtual bool probe();

  // Write a file preserving the source entry's Pascal file kind and
  // modification date - for image-to-image copies (wozmosis).
  bool writeFileWithMeta(uint8_t *contents, const char *fileName,
                         uint32_t fileSize, const struct _pascalFent *meta);

 protected:
  virtual Vent *createTree();

 private:
  // Load the flat block buffer from the underlying Woz (nibble-decode each
  // track for a floppy; raw copy for an HDV). Idempotent per open.
  bool ensureLoaded();
  bool loadBlockBuffer();
  bool readBlock(uint16_t blockNum, uint8_t out[512]);
  bool writeBlock(uint16_t blockNum, const uint8_t data[512]);
  bool validBlock(uint16_t b);

  // Read/validate the volume directory header (block 2). Populates
  // volBlockCount and dirNextBlock. Returns false if it doesn't look like
  // a Pascal volume.
  bool readVolHeader(struct _pascalVolHdr *hdrOut);

  // Load / store the whole directory (blocks 2..dirNextBlock-1) as one
  // flat byte buffer of dirBytes() length.
  uint32_t dirBytes() const { return (uint32_t)(dirNextBlock - 2) * 512; }
  bool readDirectory(uint8_t *dirBuf);
  bool writeDirectory(const uint8_t *dirBuf);

  // Shared create path used by both writeFileToImage and
  // writeFileWithMeta: place fileSize bytes on a contiguous run of free
  // blocks, insert a sorted directory entry with the given kind and date.
  bool createFile(const uint8_t *contents, const char *fileName,
                  uint32_t fileSize, uint8_t pascalKind, uint16_t modDate);

  // Copy `count` blocks from srcBlock to dstBlock. Chooses copy direction
  // so overlapping source/destination ranges move correctly (ascending
  // when sliding toward the front, descending when sliding toward the end).
  bool moveFileBlocks(uint16_t srcBlock, uint16_t dstBlock, uint16_t count);

  uint8_t *trackData;
  uint32_t trackDataBytes;

  uint16_t volBlockCount; // total blocks in the volume (from header)
  uint16_t dirNextBlock;  // first data block past the directory (usually 6)
  bool loaded;            // trackData + header valid for this open
};

#endif
