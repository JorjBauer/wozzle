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
  virtual uint8_t applesoftHeaderBytes() { return 0; };

  virtual bool writeFileToImage(uint8_t *fileContents,
                                char *fileName,
                                uint8_t fileType,
                                uint16_t auxTypeData,
                                uint32_t fileSize);

  virtual void displayInfo();
  
  virtual void inspectFile(const char *fileName, Vent *fp);
protected:
  virtual Vent *createTree();

  uint16_t calculateBlocksFree();
  void printFreeBlocks();
  
  bool findFreeBlock(uint16_t *blockOut);
  bool flushFreeBlockList();
  bool readBlock(uint16_t blockNum, uint8_t dataOut[512]);
  bool writeBlock(uint16_t blockNum, uint8_t data[512]);
  bool addDirectoryEntryForFile(struct _prodosFent *e);
  
private:
  Vent *descendTree(uint16_t fromBlock);

private:
  uint8_t trackData[35*256*16];
  uint8_t freeBlockBitmap[512];
  uint16_t volBitmapBlock;
  uint16_t numBlocksTotal;
};

#endif
