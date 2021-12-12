#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

class ApplesoftLister {
public:
  bool listFile(uint8_t *buf, uint32_t size, uint8_t skipBytes);
};
