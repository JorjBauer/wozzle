#ifndef __VMAP_H
#define __VMAP_H

#include <stdint.h>

class VMap {
 public:
  VMap();
  ~VMap();

  void DecodeVMap(uint8_t trackData[256*16]);
};

#endif
