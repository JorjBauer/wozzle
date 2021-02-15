#ifndef __DOSSPECTOR_H
#define __DOSSPECTOR_H

#include "wozspector.h"
class Vent;

class DosSpector : public Wozspector {
 public:
  DosSpector(bool verbose, uint8_t dumpflags);
  ~DosSpector();

  virtual Vent *createTree();
  virtual uint32_t getFileContents(Vent *e, char **toWhere);
};

#endif
