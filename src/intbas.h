#ifndef __INTBAS_H
#define __INTBAS_H

#include <stdint.h>
#include "basiclisting.h"

class IntegerLister {
public:
  // Detokenize a program into *out. On a malformed program, *out holds
  // the lines read before the problem (the last possibly partial) and the
  // return value is false.
  bool listFile(uint8_t *buf, uint32_t size, uint8_t skipBytes,
                BasicListing *out);
};

#endif
