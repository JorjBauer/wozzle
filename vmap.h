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
};

#endif
