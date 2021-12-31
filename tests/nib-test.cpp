#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nibutil.h"

int main(int argc, char *argv[])
{
  uint8_t buf[256];
  memset(buf, 255, sizeof(buf));

  uint8_t nib[343];
  _encode62Data(nib, buf);

  uint8_t cbuf[256];
  if (!_decode62Data(nib, cbuf)) {
    printf("Failed to decode nib data\n");
    exit(1);
  }
  for (int i=0; i<256; i++) {
    printf("byte %d: orig %.2X decoded %.2X\n", i, buf[i], cbuf[i]);
    if (buf[i] != cbuf[i]) {
      printf("Data is not consistent\n");
      exit(1);
    }
  }

  printf("All tests pass\n");
  return 0;
}
