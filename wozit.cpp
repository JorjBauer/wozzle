#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <readline/readline.h>
#include <readline/history.h>

#include <stack>
using namespace std;

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"
#include "prodosspector.h"
#include "dosspector.h"
//#include "vent.h"

uint8_t trackData[256*16];
char infoname[256] = {0};
bool verbose = false;
bool dosMode = true; // otherwise, prodos mode

Wozspector *inspector = NULL;

void usage(char *name)
{
  printf("Usage: %s -I <input image> { -d | -p }\n", name);
  printf("\n");
}

typedef void (*cmdParser)(char *cmd);

struct _cmdInfo {
  char cmdName[20];
  cmdParser fp;
};

void lsHandler(char *cmd)
{
  if (strlen(cmd)) {
    printf("Extraneous arguments\n");
    return;
  }

  Vent *tree = inspector->createTree();
  inspector->displayTree(tree);
  inspector->freeTree(tree);
}

void catHandler(char *cmd)
{
  Vent *tree = inspector->createTree();
  
  Vent *p = tree;
  while (p) {
    if (!strcmp(cmd, p->getName())) {
      char *dat = NULL;
      uint32_t s = inspector->getFileContents(p, &dat);
      if (s && dat) {
	for (int i=0; i<s; i++) {
	  printf("%c", dat[i]);
	}
	printf("\n");
	free(dat);
      }
      break;
    }
    p = p->nextEnt();
  }

  inspector->freeTree(tree);
}


void cpHandler(char *cmd)
{
  printf("in cp handler\n");
  /*
    
    if (firstTrack != 255) {
      printf("Found file at track %d sector %d\n", firstTrack, firstSector);
    }

   */
}

struct _cmdInfo commands[] = {
  {"ls", lsHandler},
  {"cat", catHandler},
  {"cp", cpHandler},
  {"", 0} };

void performCommand(char *cmd)
{
  // Find the proper command handler and give it control
  struct _cmdInfo *p = commands;
  bool handled = false;
  while (p->cmdName[0]) {
    if (!strncmp(p->cmdName, cmd, strlen(p->cmdName))) {
      p->fp(&cmd[strlen(p->cmdName)+1]);
      handled = true;
      break;
    }
    p++;
  }
  if (!handled) {
    printf("Unrecognized command\n");
  }
}

int main(int argc, char *argv[]) {

  preload_crc();

  // Parse command-line arguments
  int c;
  while ( (c=getopt(argc, argv, "dpI:h?")) != -1 ) {
    switch (c) {
    case 'd':
      dosMode = true;
      break;
    case 'p':
      dosMode = false;
      break;
    case 'I':
      strncpy(infoname, optarg, sizeof(infoname));
      break;
    case 'h':
    case '?':
      usage(argv[0]);
      exit(1);
    }
  }

  if (!infoname[0]) {
    printf("Must supply an image filename\n");
    exit(1);
  }

  if (dosMode) {
    inspector = new DosSpector(verbose, 0);
  } else {
    inspector = new ProdosSpector(verbose, 0);
  }

  if (!inspector->readFile(infoname, true)) {
    printf("Failed to read file; aborting\n");
    exit(1);
  }

  while (1) {
    char *cmd = (char *)readline("wozit> ");
    if (!cmd) break;
    performCommand(cmd);
    free(cmd);
  }
#if 0
  // Dump the file:
  // load the T/S map from firstTrack/Sector
  if (!w.decodeWozTrackToDsk(firstTrack, T_DSK, trackData)) {
    printf("Failed to read track %d; can't read T/S map\n", firstTrack);
    exit(1);
  }
  struct _dosTsList tslist;
  memcpy(&tslist, &trackData[256*firstSector], 256);

  // Dump each of the blocks
  // FIXME: nextTrack / nextSector
  // FIXME: sectorOffset - could be loaded out of order
  for (int i=0; i<122; i++) {
    if (tslist.tsPair[i].track) {
      //      printf("Next chunk of data comes from track %d sector %d\n", tslist.tsPair[i].track, tslist.tsPair[i].sector);
      if (!w.decodeWozTrackToDsk(tslist.tsPair[i].track, T_DSK, trackData)) {
	printf("Failed to read track %d; can't read data sector\n",
	       tslist.tsPair[i].track);
	exit(1);
      }
      // FIXME: actual file length, not just all the blocks
      write(fout, &trackData[tslist.tsPair[i].sector*256], 256);
    }
  }

  inspector->freeTree(tree);
#endif
  /*
  if (!w.writeFile(outname)) {
    printf("Failed to write file\n");
    exit(1);
    }*/

  return 0;
}
