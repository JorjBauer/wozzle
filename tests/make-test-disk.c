#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* This constructs a test disk image where each sector is filled
 * with increasing bytes, starting with the value of (track+sector).
 */


int main(int argc, char *argv[])
{
  FILE *f;
  f = fopen("pattern.dsk", "w");
  if (!f) {
    printf("failed to open\n");
    exit(1);
  }

  for (int track=0; track<35; track++) {
    for (int sector=0; sector<16; sector++) {
      uint8_t c = track + sector;
      for (int b=0; b<256; b++) {
        fwrite(&c, 1, 1, f);
        c++;
      }
    }
  }
  fclose(f);
  return 0;
}

