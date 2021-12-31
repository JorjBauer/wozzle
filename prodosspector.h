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
                                char fileType,
                                uint16_t fileStart,
                                uint16_t fileSize) { return false; };

  virtual void displayInfo() {printf("Info not implemented for prodos\n"); };
protected:
  virtual Vent *createTree();

private:
  Vent *descendTree(uint16_t fromBlock);

private:
  uint8_t trackData[35*256*16];
};

#endif
