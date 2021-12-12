#ifndef __DOSSPECTOR_H
#define __DOSSPECTOR_H

#include "wozspector.h"
class Vent;

class DosSpector : public Wozspector {
 public:
  DosSpector(bool verbose, uint8_t dumpflags);
  ~DosSpector();

  virtual uint32_t getFileContents(Vent *e, char **toWhere);

protected:
  virtual Vent *createTree();

protected:
  bool trackSectorUsedMap[35][16];
};

#endif
