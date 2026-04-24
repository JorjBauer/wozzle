#ifndef __INTBAS_H
#define __INTBAS_H

#include <stdint.h>

class IntegerLister {
public:
  bool listFile(uint8_t *buf, uint32_t size, uint8_t skipBytes);
};

#endif
