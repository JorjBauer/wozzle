#ifndef __VMAP_H
#define __VMAP_H

#include <stdint.h>

class Vent;

class VMap {
 public:
  VMap();
  ~VMap();

  Vent *createTree(uint8_t *trackData, int masterBlock);
  void freeTree(Vent *tree);
  void displayTree(Vent *tree);

  // getFileContents will will malloc(); caller must call free()
  // return value is the length
  uint32_t getFileContents(Vent *e, char **toWhere);
};

#endif
