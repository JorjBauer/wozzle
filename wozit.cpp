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
#include "intbas.h"
//#include "vent.h"

#ifndef MAXPATH
#define MAXPATH 127
#endif

uint8_t trackData[256*16];
char infoname[256] = {0};
char oneShotCommand[512] = {0};
bool verbose = false;
bool dosMode = true; // otherwise, prodos mode
bool striphi = false;
bool useAllocation = false; // cpout uses full on-disk allocation not hdr length

Wozspector *inspector = NULL;

void usage(char *name)
{
  printf("Usage: %s -I <input image> { -d | -p } [-c <command>] [-A]\n\n", name);
  printf("  -I [input filename]     input disk image to inspect/modify\n");
  printf("  -d                      DOS mode\n");
  printf("  -p                      ProDOS mode\n");
  printf("  -c [command]            run a single command non-interactively\n");
  printf("  -A                      cpout: copy full on-disk allocation,\n");
  printf("                          not the directory-reported length\n");
  printf("  -v                      verbose operation\n");
  printf("\n");
}

typedef void (*cmdParser)(char *cmd);

struct _cmdInfo {
  char cmdName[20];
  cmdParser fp;
  char helpInfo[80];
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

static void dumpFileContents(const char *cmd, bool forceStripHi)
{
  Vent *fp = findFileByName(cmd);
  if (fp) {
    char *dat = NULL;
    uint32_t s = inspector->getFileContents(fp, &dat);
    if (s && dat) {
      bool strip = forceStripHi || striphi;
      for (int i=0; i<s; i++) {
        char c = strip ? (dat[i] & 0x7F) : dat[i];
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

void catHandler(char *cmd)
{
  dumpFileContents(cmd, false);
}

void cat7Handler(char *cmd)
{
  // Always strips the high bit, regardless of the `strip` toggle.
  dumpFileContents(cmd, true);
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
  uint32_t s;
  if (useAllocation) {
    s = inspector->getFileAllocation(fp, &dat);
  } else {
    s = inspector->getFileContents(fp, &dat);
    // Warn if the directory-length version is far smaller than what the
    // disk actually allocates — classic stub-loader pattern (TAIPAN-style).
    // Threshold picked to avoid noise on normal files where the program
    // ends part-way into its last sector: flag only when the allocated
    // size exceeds the logical size by more than 1 KB.
    uint32_t allocated = inspector->getAllocatedByteCount(fp);
    if (allocated > s && allocated - s > 1024) {
      fprintf(stderr,
              "WARNING: '%s' copied as %u bytes (directory length), but "
              "%u bytes are allocated on disk. Re-run with -A to copy the "
              "full allocation.\n",
              buf, s, allocated);
    }
  }

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
  inspector->dumpInfo();
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

  char *fnp = cmd;
  if (!fnp) {
    printf("Usage[1]: cpin <filename> <dest filename>\n");
    return;
  }
  char *destfnp = strstr(fnp, " ");
  if (!destfnp) {
    printf("Usage[2]: cpin <filename> <dest filename>\n");
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
  if (!cmd || !cmd[0]) {
    printf("Usage: save <file name>\n");
    return;
  }
  if (!inspector->writeFile(cmd)) {
    printf("Failed to write file\n");
  }
}

void listHandler(char *cmd)
{
  if (cmd[0] == 0) {
    printf("Bad arguments. Usage: list <filename>\n");
    return;
  }

  Vent *fp = findFileByName(cmd);
  if (!fp) {
    printf("File '%s' not found\n", cmd);
    return;
  }

  uint8_t ft = fp->getFileType();
  if (ft != FT_BAS && ft != FT_INT) {
    printf("File is not BASIC (Applesoft or Integer)\n");
    return;
  }

  uint8_t *dat = NULL;
  uint32_t s = inspector->getFileContents(fp, (char **)&dat);
  if (!s || !dat) {
    printf("File is empty\n");
    return;
  }

  bool ok;
  if (ft == FT_BAS) {
    ApplesoftLister l;
    ok = l.listFile(dat, s, inspector->applesoftHeaderBytes());
  } else {
    IntegerLister l;
    ok = l.listFile(dat, s, inspector->applesoftHeaderBytes());
  }
  if (!ok) {
    printf("Failed to list file\n");
  }
  free(dat);
}

void inspectHandler(char *cmd)
{
  Vent *fp = findFileByName(cmd);
  if (fp) {
    printf("Entry dump for '%s':\n", cmd);
    fp->Dump(true);
    inspector->inspectFile(cmd, fp);
  } else {
    printf("File not found\n");
  }
}

void helpHandler(char *cmd); // forward decl for commands[]

struct _cmdInfo commands[] = {
  {"cat",   catHandler,   "<filename>        : Dump file contents" },
  {"cat7",  cat7Handler,  "<filename>       : Dump file contents, stripping the high bit" },
  {"cpin",  cpinHandler,  "<SOURCE> <DEST>  : copy host file <SOURCE> in to image filename <DEST>" },
  {"cpout", cpoutHandler, "<SOURCE> <DEST> : copy image file <SOURCE> out to host filesystem <DEST>" },
  {"help",  helpHandler,  "                 : this text" },
  {"info",  infoHandler,  "                 : show filesystem meta-information" },
  {"inspect", inspectHandler, "<filename>   : show meta-info about <filename>" },
  {"list",  listHandler,  "<filename>       : Applesoft basic program detokenizer" },
  {"ls",    lsHandler,   "                   : List directory" },
  {"save",  saveHandler, "<filename>       : save modified disk image as <filename>" },
  {"strip", stripHandler, "<on|off>        : turn on or off high bit strip for 'cat'"},
  {"", 0, ""} };

void helpHandler(char *cmd)
{
  // Ignoring cmd for now
  printf("List of wozit commands:\n");
  struct _cmdInfo *p = commands;
  while (p->cmdName[0]) {
    printf("%s %s\n", p->cmdName, p->helpInfo);
    p++;
  }
  printf("\n");
}




void performCommand(char *cmd)
{
  // Match the command name as a whole word (terminated by end-of-string
  // or a space) so that e.g. "cat7 FOO" binds to cat7, not to cat with
  // a garbled argument.
  struct _cmdInfo *p = commands;
  bool handled = false;
  while (p->cmdName[0]) {
    size_t nl = strlen(p->cmdName);
    if (!strncmp(p->cmdName, cmd, nl) &&
        (cmd[nl] == '\0' || cmd[nl] == ' ')) {
      // If there's no argument, pass the null terminator itself instead
      // of reading one byte past it.
      char *arg = cmd[nl] ? &cmd[nl + 1] : &cmd[nl];
      p->fp(arg);
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
  while ( (c=getopt(argc, argv, "dpI:c:Ah?v")) != -1 ) {
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
    case 'c':
      strncpy(oneShotCommand, optarg, sizeof(oneShotCommand) - 1);
      break;
    case 'A':
      useAllocation = true;
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

  if (!infoname[0]) {
    printf("Must supply an image filename\n");
    exit(1);
  }

  if (dosMode) {
    inspector = new DosSpector(verbose, verbose ? (DUMP_QTMAP | DUMP_QTCRC | DUMP_TRACK | DUMP_RAWTRACK) : 0);
  } else {
    inspector = new ProdosSpector(verbose, verbose ? (DUMP_QTMAP | DUMP_QTCRC | DUMP_TRACK | DUMP_RAWTRACK) : 0);
  }

  if (!inspector->readFile(infoname, true)) {
    printf("Failed to read file; aborting\n");
    exit(1);
  }

  // If the image doesn't smell like the filesystem we were told to use,
  // try probing the other kind. If that looks right, warn the user.
  if (!inspector->probe()) {
    Wozspector *other = dosMode
      ? (Wozspector *)new ProdosSpector(false, 0)
      : (Wozspector *)new DosSpector(false, 0);
    if (other->readFile(infoname, true) && other->probe()) {
      printf("WARNING: '%s' looks like a %s image, but wozit was invoked "
             "in %s mode. Re-run with '%s' if you meant %s.\n",
             infoname,
             dosMode ? "ProDOS" : "DOS 3.3",
             dosMode ? "DOS 3.3 (-d)" : "ProDOS (-p)",
             dosMode ? "-p" : "-d",
             dosMode ? "ProDOS" : "DOS 3.3");
    }
    delete other;
  }

  if (oneShotCommand[0]) {
    performCommand(oneShotCommand);
    return 0;
  }

  while (1) {
    char *cmd;
    if (inspector->isDirty()) {
      cmd = (char *)readline("wozit*> ");
    } else {
      cmd = (char *)readline("wozit> ");
    }
    if (!cmd) break;
    if (cmd[0]) performCommand(cmd);
    free(cmd);
  }

  return 0;
}
