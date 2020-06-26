#include <stdio.h>
#include <getopt.h>
#include <string.h>

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"

void usage(char *name)
{
  printf("Usage: %s { -I <input file> [-D <flags>] [-s] [-v] | { -i <input file> -o <output file> [-s] [-v] } }\n", name);
  printf("\n");
  printf("\t-h\t\t\tThis help text\n");
  printf("\t-I <input filename>\tDump information about disk image\n");
  printf("\t-D <flags>\t\tEnable specific dump flags (bitwise uint8_t)\n");
  printf("\t-i <input filename>\tName of input disk image\n");
  printf("\t-o <output filename>\tName of output (WOZ2) disk image\n");
  printf("\t-s\t\t\tSmaller memory footprint (don't preread tracks)\n");
  printf("\t-v\t\t\tVerbose output\n");
}


int main(int argc, char *argv[]) {
  char inname[256] = {0};
  char outname[256] = {0};
  char infoname[256] = {0};
  bool verbose = false;
  bool preloadTracks = true;
  uint32_t dumpflags = 0; // DUMP_RAWTRACK usw., cf. woz.h

  preload_crc();

  // Parse command-line arguments
  int c;
  while ( (c=getopt(argc, argv, "D:I:i:o:svh?")) != -1 ) {
    switch (c) {
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

  bool r = w.readFile(inname[0] ? inname : infoname, preloadTracks);
  if (!r) {
    printf("Failed to read file; aborting\n");
    exit(1);
  }

  if (infoname[0]) {
    w.dumpInfo();
    exit(0);
  }

  const char *p = strrchr(outname, '.');
  if (!p) {
    printf("Unable to determine file type of '%s'\n", outname);
    exit(1);
  }
  if (strcasecmp(p, ".woz") == 0) {
    r = w.writeFile(2, outname);
  } else if (strcasecmp(p, ".dsk") == 0 ||
	     strcasecmp(p, ".dsk") == 0 ||
	     strcasecmp(p, ".po") == 0) {
    if (verbose) {
      printf("Writing %s\n", outname);
    }
    if (w.isSynchronized()) {
      printf("WARNING: disk image has synchronized tracks; it may not work as a DSK or NIB file.\n");
    }
    FILE *out = fopen(outname, "w");
    if (!out) {
      perror("Failed to open output file");
      exit(1);
    }
    uint8_t sectorData[256*16];
    for (int track=0; track<35; track++) {
      if (!w.decodeWozTrackToDsk(track, (strcasecmp(p, ".po") == 0) ?T_PO:T_DSK, sectorData)) {
	printf("Failed to decode track %d; aborting\n", track);
	exit(1);
      }
      fwrite(sectorData, 1, 256*16, out);
    }
    fclose(out);

    r = true;
  } else if (strcasecmp(p, ".nib") == 0) {
    if (verbose) {
      printf("Writing %s\n", outname);
    }
    if (w.isSynchronized()) {
      printf("WARNING: disk image has synchronized tracks; it may not work as a DSK or NIB file.\n");
    }
    FILE *out = fopen(outname, "w");
    if (!out) {
      perror("Failed to open output file");
      exit(1);
    }
    nibSector nibData[16];
    for (int track=0; track<35; track++) {
      if (!w.decodeWozTrackToNib(track, nibData)) {
	printf("Failed to decode track %d; aborting\n", track);
	exit(1);
      }
      fwrite(nibData, 1, NIBTRACKSIZE, out);
    }
    fclose(out);
    
    r = true;
  } else {
    printf("Unable to determine file type of '%s'\n", outname);
    exit(1);
  }

  if (!r) {
    printf("Failed to write file\n");
    exit(1);
  }

  return 0;
}
