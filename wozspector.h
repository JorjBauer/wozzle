#ifndef __WOZSPECTOR_H
#define __WOZSPECTOR_H

#include "woz.h"
#include "vent.h"

class Wozspector : public Woz {
public:
  Wozspector(bool verbose, uint8_t dumpflags);
  ~Wozspector();

  Vent *getTree();
  void displayTree();
  
  // getFileContents will will malloc(); caller must call free()
  // return value is the length
  virtual uint32_t getFileContents(Vent *e, char **toWhere) = 0;

  // Dos and ProDOS have different size applesoft headers, which affects listing
  virtual uint8_t applesoftHeaderBytes() = 0;

protected:
  virtual Vent *createTree() = 0;
  virtual void displayTree(Vent *tree);
  virtual void freeTree(Vent *tree);
  
  void appendTree(Vent *a);

protected:
  Vent *tree;
};

#endif
