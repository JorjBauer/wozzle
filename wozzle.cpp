#include <stdio.h>
#include <getopt.h>
#include <string.h>
#define FUSE_USE_VERSION 26
#include <fuse.h>

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"
#include "vtoc.h"
#include "vmap.h"

uint8_t trackData[256*16];
bool fuseInitialized = false;

static bool initFuse(Woz *w)
{
  if (!w->decodeWozTrackToDsk(0, T_PO, trackData)) {
    printf("Failed to read track 0; can't dump VMap\n");
    exit(1);
  }
  VMap vmap;
  vmap.DecodeVMap(trackData);
  fuseInitialized = true;
}

static int getattr_callback(const char *path, struct stat *stbuf) {
  printf("getattr_callback for '%s'\n", path);
  memset(stbuf, 0, sizeof(struct stat));
  // FIXME actually read the mode from the directory

  if (strcmp(path, "/") == 0) {
    stbuf->st_mode = S_IFDIR|0755;
    stbuf->st_nlink=2;
    return 0;
  }
  
  //  stbuf->st_mode = S_IFDIR | 0755;
  stbuf->st_mode = S_IFREG | 0666;
  stbuf->st_nlink = 1;
  stbuf->st_size = 6; // FIXME actual length
  return 0;
}

static int open_callback(const char *path, struct fuse_file_info *fi) {
  printf("open_callback\n");
  return 0;
}

static const char *filecontent = "I'm the content of the only file available there\n";
size_t len = strlen(filecontent);

static int read_callback(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {

  printf("read_callback\n");fflush(stdout);
  // FIXME: if it's not a file we can find, then return -ENOENT;
  
  if (offset >= len) {
    return 0;
  }

  if (offset + size > len) {
    memcpy(buf, filecontent + offset, len - offset);
    return len - offset;
  }

  memcpy(buf, filecontent + offset, size);
  return size;
}

static int readdir_callback(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi) {
  printf("readdir_callback\n");fflush(stdout);
  (void) offset;
  (void) fi;
  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  filler(buf, "FILEname", NULL, 0);

  return 0;
}


static struct fuse_operations fuseops = {
  .getattr = getattr_callback,
  .open = open_callback,
  .read = read_callback,
  .readdir = readdir_callback,
};

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
  printf("\t-p\t\t\tDecode ProDOS information\n");
  printf("\t-s\t\t\tSmaller memory footprint\n");
  printf("\t-v\t\t\tVerbose output\n");
}


int main(int argc, char *argv[]) {
  char inname[256] = {0};
  char outname[256] = {0};
  char infoname[256] = {0};
  char mntpath[256] = {0};
  bool verbose = false;
  bool preloadTracks = true;
  bool dumpDosInfo = false;
  bool dumpProdosInfo = false;
  uint32_t dumpflags = 0; // DUMP_RAWTRACK usw., cf. woz.h

  preload_crc();

  // Parse command-line arguments
  int c;
  while ( (c=getopt(argc, argv, "dD:I:i:o:psvhF:?")) != -1 ) {
    switch (c) {
    case 'd':
      dumpDosInfo = true;
      break;
    case 'p':
      dumpProdosInfo = true;
      break;
    case 'D':
      // FIXME set endptr and check that the whole arg was consumed
      dumpflags = strtoul(optarg, NULL, 10);
      break;
    case 'F':
      strncpy(mntpath, optarg, sizeof(mntpath));
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

  if (mntpath[0] &&  (inname[0] || outname[0])) {
    printf("Invalid usage\n");
    usage(argv[0]);
    exit(1);
  }

  if (mntpath[0] && !infoname[0]) {
    printf("Must supply -I with -F\n");
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
      if (!w.decodeWozTrackToDsk(17, /* The catalog and VToC are on track 17 */
				 T_DSK,
				 trackData)) {
	printf("Failed to read track 17; can't dump VToC\n");
	exit(1);
      }
      VToC vtoc;
      vtoc.DecodeVToC(&trackData[0] /* start of sector 0 */);
    }

    if (dumpProdosInfo) {
      uint8_t trackData[256*16];
      if (!w.decodeWozTrackToDsk(0, T_PO, trackData)) { // Dumping sectors in ProDOS (block) order
	printf("Failed to read track 0; can't dump VMap\n");
	exit(1);
      }
      VMap vmap;
      vmap.DecodeVMap(trackData);
    }

    if (mntpath[0]) {
      // Use FUSE to mount the image. We need to fake fuse's command line
      // arguments though.
      printf("Trying to fuse-mount at %s\n", mntpath);
      char *newargv[4] = { "",
	"-f",
	"-s",
	"" };
      newargv[0] = argv[0];
      newargv[3] = mntpath;
      int ret = fuse_main(4, newargv, &fuseops, NULL);
      printf("ret: %d\n", ret);
      return ret;
    }
    exit(0);
  }

  if (!w.writeFile(outname)) {
    printf("Failed to write file\n");
    exit(1);
  }

  return 0;
}
