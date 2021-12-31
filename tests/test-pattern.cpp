#include <stdio.h>

#include "woz.h"

/*
 * This tests takes a patterned disk, where each sector is filled with 
 * increasing bytes starting at the value of (Track+sector).
 *
 * On the first pass, it tests that the read routines find the correct
 * values for each sector - both by reading the whole track at once,
 * and by reading one sector at a time.
 * 
 * Once the first pass completes, it writes 256 bytes of 0xFF on 
 * track 13 sector 6 (arbitrarily chosen) and then loops around to 
 * do a whole-disk check again, expecting all the same data as the
 * first pass except for track 13 sector 6 which contains all 0xFF.
 */

// If we want DOS 3.3 logical sector number 1, we actually want
// physical sector 0x0D. Because that's how DOS3.3 interleaves.
const static uint8_t enphys[16] = {
  0x00, 0x0d, 0x0b, 0x09, 0x07, 0x05, 0x03, 0x01,
  0x0e, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x0f };

Woz w(0,0);

int main(int argc, char *argv[])
{
  if (!w.readFile("pattern.dsk", false, T_DSK)) {
    printf("Failed to read disk\n");
    exit(1);
  }

  bool didOnce = false;
 repeatOnce:
  for (int track=0; track<35; track++) {

    printf("Checking track %d full track decode\n", track);
    uint8_t fullTrackData[256*16];
    if (!w.decodeWozTrackToDsk(track, T_DSK, fullTrackData)) {
      printf("Failed to read track\n");
      exit(1);
    }
    for (int sector=0; sector<16; sector++) {
      if (didOnce && track == 13 && sector == 6) {
        for (int b=0; b<256; b++) {
          if (fullTrackData[sector*256+b] != 255) {
            printf("The rewritten data is wrong on t13/s6: expected 0xFF, got %.2X\n", fullTrackData[sector*256+b]);
            exit(1);
          }
        }
      } else {
        for (int b=0; b<256; b++){
          if (fullTrackData[sector*256+b] != (track+sector+b)%256) {
            printf("Mismatch in track data for sector %d byte %d: found %.2X, expected %.2X\n", sector, b, fullTrackData[sector*256+b], (track+sector+b)%256);
            exit(1);
          }
        }
      }
    }
    
    for (int sector=0; sector<16; sector++) {
      printf("Checking track %d decode of just sector %d\n", track, sector);
      uint8_t expect = track + sector;

      int physSector = enphys[sector];
      
      uint8_t dataOut[256];
      if (!w.decodeWozTrackSector(track, physSector, dataOut)) {
        printf("Failed to read track/sector\n");
        exit(1);
      }
      for (int b=0; b<256; b++) {
        if (didOnce && track == 13 && sector == 6) {
          for (int b=0; b<256; b++) {
            if (dataOut[b] != 255) {
              printf("The rewritten data is wrong on t13/s6: expected 0xFF, got %.2X\n", dataOut[b]);
              exit(1);
            }
          }
        } else {
          if (dataOut[b] != (expect + b) % 256) {
            printf("Byte %d differs: %.2X vs %.2X\n", b, dataOut[b], expect+b);
            exit(1);
          }
        }
      }
    }
  }

  if (!didOnce) {
    // Write a new sector's contents and see that we can read it back - and the rest of the disk is still ok.
    // We'll replace Track 13, DOS3.3 sector 6 with all 0xFF bytes.
    uint8_t s[256];
    memset(s, 0xFF, 256);
    if (!w.encodeWozTrackSector(13, enphys[6], s)) {
      printf("ERROR: failed to encode woz t/s\n");
      exit(1);
    }
    // For debugging - save it to out.dsk so we can inspect the damage, if any
    printf("Writing to 'pattern-out.dsk'\n");
    if (!w.writeFile("pattern-out.dsk", T_DSK)) {
      printf("ERROR: failed to write disk image\n");
      exit(1);
    }
    didOnce = true;
    goto repeatOnce;
  }
  
  printf("All tests pass\n");
  return 0;
}
