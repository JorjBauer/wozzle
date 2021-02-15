#ifndef __PRODOSSPECTOR_H
#define __PRODOSSPECTOR_H

#include <stdint.h>
#include "wozspector.h"
#include "vent.h"

class ProdosSpector : public Wozspector {
 public:
  ProdosSpector(bool verbose, uint8_t dumpflags);
  ~ProdosSpector();

  virtual Vent *createTree();
  
  virtual uint32_t getFileContents(Vent *e, char **toWhere);

private:
  Vent *descendTree(uint16_t fromBlock);

private:
  uint8_t trackData[35*256*16];
  uint16_t cwdMasterBlock;
};

#endif
