#include <stdio.h>
#include <getopt.h>
#include <string.h>

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"
#include "vtoc.h"

void usage(char *name)
{
  printf("Usage: %s { -I <input file> [-D <flags>] [-d] [-s] [-v] | { -i <input file> -o <output file> [-s] [-v] } }\n", name);
  printf("\n");
  printf("\t-h\t\t\tThis help text\n");
  printf("\t-I <input filename>\tDump information about disk image\n");
  printf("\t-d\t\t\tDecode DOS 3.3 information\n");
  printf("\t-D <flags>\t\tEnable specific dump flags (bitwise uint8_t)\n");
  printf("\t-i <input filename>\tName of input disk image\n");
  printf("\t-o <output filename>\tName of output (WOZ2) disk image\n");
  printf("\t-s\t\t\tSmaller memory footprint\n");
  printf("\t-v\t\t\tVerbose output\n");
}


int main(int argc, char *argv[]) {
  char inname[256] = {0};
  char outname[256] = {0};
  char infoname[256] = {0};
  bool verbose = false;
  bool preloadTracks = true;
  bool dumpDosInfo = false;
  uint32_t dumpflags = 0; // DUMP_RAWTRACK usw., cf. woz.h

  preload_crc();

  // Parse command-line arguments
  int c;
  while ( (c=getopt(argc, argv, "dD:I:i:o:svh?")) != -1 ) {
    switch (c) {
    case 'd':
      dumpDosInfo = true;
      break;
    case 'D':
      // FIXME set endptr and check that the whole arg was consumed
      dumpflags = strtoul(optarg, NULL, 10);
      break;
    case 'I':
      strncpy(infoname, optarg, sizeof(infoname));
      break;
    case 'i':
      strncpy(inname, optarg, sizeof(inname));
      break;
    case 'o':
      strncpy(outname, optarg, sizeof(outname));
      break;
    case 's':
      preloadTracks = false;
      break;
    case 'v':
      verbose = true;
      break;
    case 'h':
    case '?':
      usage(argv[0]);
      exit(1);
    }
  }

  if (!infoname[0] && !inname[0]) {
    printf("Must supply an input filename\n");
    usage(argv[0]);
    exit(1);
  }

  if (inname[0] && !outname[0]) {
    printf("Must supply an output filename\n");
    usage(argv[0]);
    exit(1);
  }

  if (infoname[0] && (inname[0] || outname[0])) {
    printf("Invalid usage\n");
    usage(argv[0]);
    exit(1);
  }

  Woz w(verbose, dumpflags & 0xFF);

  if (!w.readFile(inname[0] ? inname : infoname, preloadTracks)) {
    printf("Failed to read file; aborting\n");
    exit(1);
  }

  if (infoname[0]) {
    w.dumpInfo();

    if (dumpDosInfo) {
      uint8_t trackData[256*16];
      if (!w.decodeWozTrackToDsk(17, /* The catalog and VToC are on track 17 */
				 T_DSK,
				 trackData)) {
	printf("Failed to read track 17; can't dump VToC\n");
	exit(1);
      }
      VToC vtoc;
      vtoc.DecodeVToC(&trackData[0] /* start of sector 0 */);
    }
    exit(0);
  }

  if (!w.writeFile(outname)) {
    printf("Failed to write file\n");
    exit(1);
  }

  return 0;
}
