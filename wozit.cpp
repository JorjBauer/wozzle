#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#if defined(__APPLE__)
  #include <machine/endian.h>
#else
  #include <endian.h>
#endif

#include <readline/readline.h>
#include <readline/history.h>

#include "applesingle.h"

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"
#include "prodosspector.h"
#include "dosspector.h"
#include "applesoft.h"
//#include "vent.h"

#ifndef MAXPATH
#define MAXPATH 127
#endif

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
  // cpin: copy a local file in to a file image.
  // cpin <filename> <dest filename>
  // If it's an AppleSingle file, then the type comes from the AppleSingle
  //   ProDOS file format data; if it's not an AppleSingle file, then it's
  //   assumed it's an as-is binary file

  // Parse arguments
  char fn[MAXPATH+1];
  char destfn[30+1];
  uint8_t filetype = 0x06; // ProDOS type numbers (more expressive than DOS)
  uint16_t fileSize = 0;
  uint16_t auxTypeData = 0x2000;
  memset(fn, 0, sizeof(fn));
  memset(destfn, 0, sizeof(destfn));

  char *fnp = strstr(cmd, " ");
  if (!fnp) {
    printf("Usage: cpin <filename> <dest filename>\n");
    return;
  }
  char *destfnp = strstr(++fnp, " ");
  if (!destfnp) {
    printf("Usage: cpin <filename> <dest filename>\n");
    return;
  }
  strncpy(fn, fnp, destfnp-fnp < MAXPATH ? destfnp-fnp : MAXPATH);
  destfnp++;
  strncpy(destfn, destfnp, 30);
  
  // stat the file so we get a length
  struct stat s;
  if (lstat(fn, &s)) {
    printf("Unable to stat source file '%s'\n", fn);
    return;
  }
  fileSize = s.st_size; // assume file size to copy in is the 

  // Read the file contents to a buffer
  uint8_t *dataToCopy = NULL;
  uint8_t *fileContents = (uint8_t *)malloc(fileSize);
  FILE *in = fopen(fn, "r");
  if (!in) {
    printf("ERROR: Failed to read file\n");
    free(fileContents);
    return;
  }
  fread(fileContents, 1, fileSize, in);
  fclose(in);

  // Look for an AppleSingle header
  bool foundAppleSingle = false;
  bool foundMetadata = false;
  if (fileSize >= sizeof(applesingle)) {
    applesingle *as = (applesingle *)fileContents;
    if (ntohl(as->magic) == 0x51600 && ntohl(as->version) == 0x20000) {
      foundAppleSingle = true;
      for (int i=0; i<16; i++) {
        if (as->filler[i] != 0) {
          printf("ERROR: Filler is NOT BLANK\n");
          foundAppleSingle = false;
          break;
        }
      }
      if (foundAppleSingle) {
        for (int i=0; i<ntohs(as->num_entries); i++) {
          printf("ENTRY %d:\n", i);
          printf("  entryID: %d\n", ntohs(as->entry[i].entryID));
          printf("  offset: %d\n", ntohs(as->entry[i].offset));
          printf("  length: %d\n", ntohs(as->entry[i].length));

          if (ntohs(as->entry[i].entryID) == as_prodosFileInfo) {
            type11prodos *p = (type11prodos *)(&fileContents[ntohs(as->entry[i].offset)]);
            printf("    Access: 0x%X\n", ntohs(p->access));
            printf("    FileType: 0x%X\n", ntohs(p->filetype));
            printf("    AuxType: 0x%X\n", ntohl(p->auxtype));
            /* The access field is bitwise ProDOS access permissions
             * FileType is a ProDOS file type (0x06 == BIN)
             * AuxType carries the start address for BIN files
             *   (cf. https://prodos8.com/docs/technote/19/)
             */
            foundMetadata = true;
            filetype = ntohs(p->filetype);
            auxTypeData = ntohl(p->auxtype);
          }
          else if (ntohs(as->entry[i].entryID) == as_dataFork) {
            fileSize = ntohs(as->entry[i].length);
            if (dataToCopy) {
              // dunno why we'd get 2 of them but...
              free(dataToCopy);
            }
            dataToCopy = (uint8_t *)malloc(fileSize);
            memcpy(dataToCopy, &fileContents[ntohs(as->entry[i].offset)], fileSize);
          } else {
            printf("ERROR: unknown entry ID type\n");
            foundAppleSingle = false;
            break;
          }
        }
        //asEntry entries[1]; // and continue off the end of the array
        // .entryID, .offset, .length
        // then _type11prodos (as_prodosFileInfo)
        // .access, .filetype, .auxtype
        // and as_dataFork

      }
    }
  }
  
  // Write the file into the image
  if (!inspector->writeFileToImage(dataToCopy ? dataToCopy : fileContents, destfn,
                                   filetype, auxTypeData, fileSize)) {
    printf("ERROR: Failed to write file\n");
  }
  if (dataToCopy) free(dataToCopy);
  free(fileContents);
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

void saveHandler(char *cmd)
{
  char *fnp = strstr(cmd, " ");
  if (!fnp) {
    printf("Usage: save <file name>\n");
    return;
  }
  if (!inspector->writeFile(++fnp)) {
    printf("Failed to write file\n");
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
  {"save", saveHandler},
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
