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
#include "applesoft.h"
//#include "vent.h"

uint8_t trackData[256*16];
char infoname[256] = {0};
bool verbose = false;
bool dosMode = true; // otherwise, prodos mode
bool striphi = false;

Wozspector *inspector = NULL;

void usage(char *name)
{
  printf("Usage: %s -I <input image> { -d | -p }\n\n", name);
  printf("  -I [input filename]     input disk image to inspect/modify\n");
  printf("  -d                      DOS mode\n");
  printf("  -p                      ProDOS mode\n");
  printf("\n");
}

typedef void (*cmdParser)(char *cmd);

struct _cmdInfo {
  char cmdName[20];
  cmdParser fp;
};

void lsHandler(char *cmd)
{
  if (cmd && strlen(cmd)) {
    printf("Extraneous arguments: '%s' (0x%llX)\n", cmd, (unsigned long long)cmd);
    return;
  }

  inspector->displayTree();
}

Vent *findFileByName(const char *name)
{
  Vent *tree = inspector->getTree();
  Vent *p = tree;
  while (p) {
    if (!strcmp(name, p->getName())) {
      return p;
    }
    p = p->nextEnt();
  }
  return NULL;
}

void catHandler(char *cmd)
{
  Vent *fp = findFileByName(cmd);
  if (fp) {
    char *dat = NULL;
    uint32_t s = inspector->getFileContents(fp, &dat);
    if (s && dat) {
      for (int i=0; i<s; i++) {
	char c = striphi ? (dat[i] & 0x7F) : dat[i];
	if (c == 10 || c == 13) {
	  printf("\n");
	} else {
	  printf("%c", c);
	}
      }
      printf("\n");
      free(dat);
    }
  } else {
    printf("File not found\n");
  }
}

void cpoutHandler(char *cmd)
{
  // cpout: copy from a file in the image to a file outside the image
  // cpout <image filename> <output filename>
  char buf[16];
  char *p = strstr(cmd, " ");
  if (!p) {
    printf("Error parsing arguments. Usage: cpout <SOURCE> <DEST>\n");
    return;
  }
  uint32_t len = p-cmd;
  if (len > 15) {
    printf("Invalid filename\n");
    return;
  }
  strncpy(buf, cmd, p-cmd);
  buf[p-cmd] = '\0';
  p++; // pass the space
    
  Vent *fp = findFileByName(buf);
  if (!fp) {
    printf("File '%s' not found\n", buf);
    return;
  }

  if (!*p) {
    printf("Error: output filename is blank\n");
    return;
  }
  
  char *dat = NULL;
  uint32_t s = inspector->getFileContents(fp, &dat);
  if (s && dat) {
    FILE *out = fopen(p, "w");
    fwrite(dat, 1, s, out);
    fclose(out);
    free(dat);
  } else {
    printf("Empty file; skipping\n");
    return;
  }
}

void infoHandler(char *cmd)
{
  // FIXME: handle arguments? Or throw error?
  inspector->displayInfo();
}

void cpinHandler(char *cmd)
{
  // cpin: copy a local file in to a file image
  // cpin <filename> <dest filename> <file type char> <start address in hex>

  // FIXME actually parse arguments, don't use these static things
  uint16_t fileSize = 49;
  uint16_t fileStart = 0x1000;
  char fileType = 'B';
  uint8_t *fileContents = (uint8_t *)malloc(fileSize);
  char localname[16] = "test.bin";
  char imagename[16] = "TEST";
  FILE *in = fopen(localname, "r");
  if (!in) {
    printf("ERROR: Failed to read file\n");
    return;
  }
  fread(fileContents, 1, fileSize, in);
  fclose(in);
  
  if (!inspector->writeFileToImage(fileContents, imagename,
                                   fileType, fileStart, fileSize)) {
    printf("ERROR: Failed to write file\n");
  }
  free(fileContents);

  // DEBUGGING
  inspector->writeFile("out.dsk", T_DSK);

}

void stripHandler(char *cmd)
{
  if (!strcmp(cmd, "on")) {
    striphi = true;
    printf("Strip high bit: true\n");
  } else if (!strcmp(cmd, "off")) {
    striphi = false;
    printf("Strip high bit: false\n");
  } else {
    printf("Bad arguments. Usage: strip <on|off>\n");
  }
}

void listHandler(char *cmd)
{
  if (cmd[0] == 0) {
    printf("Bad arguments. Usage: list <applesoft filename>\n");
    return;
  }

  Vent *fp = findFileByName(cmd);
  if (!fp) {
    printf("File '%s' not found\n", cmd);
    return;
  }

  if (fp->getFileType() != FT_BAS) {
    printf("File is not Applesoft BASIC\n");
    return;
  }
  
  uint8_t *dat = NULL;
  uint32_t s = inspector->getFileContents(fp, (char **)&dat);
  if (!s || !dat) {
    printf("File is empty\n");
    return;
  }
  
  ApplesoftLister l;
  if (!l.listFile(dat, s, inspector->applesoftHeaderBytes())) {
    printf("Failed to list file\n");
    // fall through to free the alloc'd file contents
  }
  free(dat);
}

struct _cmdInfo commands[] = {
  {"ls", lsHandler},
  {"cat", catHandler},
  {"cpout", cpoutHandler},
  {"strip", stripHandler},
  {"list", listHandler},
  {"cpin", cpinHandler},
  {"info", infoHandler},
  {"", 0} };

void performCommand(char *cmd)
{
  // Find the proper command handler and give it control
  struct _cmdInfo *p = commands;
  bool handled = false;
  while (p->cmdName[0]) {
    if (!strncmp(p->cmdName, cmd, strlen(p->cmdName))) {
      p->fp(&cmd[strlen(p->cmdName)]);
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
  
  return 0;
}
