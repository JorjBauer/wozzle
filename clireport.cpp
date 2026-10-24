#include "clireport.h"

#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include "nibutil.h"
#include "disktypes.h"
#include "dosspector.h"
#include "prodosspector.h"
#include "pascalspector.h"

// "    0x0010 : XX XX ..." rows, 16 bytes to a row
static void hexRows(const char *indent, const uint8_t *p, uint32_t len)
{
  for (uint32_t k=0; k<len; k+=16) {
    printf("%s0x%.4lX :", indent, (unsigned long)k);
    for (uint32_t j=0; j<16 && k+j < len; j++) {
      printf(" %.2X", p[k+j]);
    }
    printf("\n");
  }
}

static void printSectorFields(uint8_t volume, uint8_t track, uint8_t sector,
                              uint8_t checksum)
{
  printf("      Volume ID: %d\n", volume);
  printf("      Track ID: %d\n", track);
  printf("      Sector: %d\n", sector);
  printf("      Cksum: %d\n", checksum);
}

void printWozInfo(Woz *w, uint8_t dumpflags)
{
  const diskInfo *di = w->getDiskInfo();

  printf("WOZ image version %d\n", di->version);
  printf("Disk type: %s\n", di->diskType == 1 ? "5.25\"" : "3.5\"");
  printf("Write protected: %s\n", di->writeProtected ? "yes" : "no");
  printf("Synchronized: %s\n", di->synchronized ? "yes" : "no");
  printf("Cleaned: %s\n", di->cleaned ? "yes" : "no");
  printf("Creator: %s\n", di->creator);
  printf("Disk sides: %d\n", di->diskSides);
  printf("Boot sector format: ");
  switch (di->bootSectorFormat) {
  case 0:
  default:
    printf("unknown\n");
    break;
  case 1:
    printf("16 sector\n");
    break;
  case 2:
    printf("13 sector\n");
    break;
  case 3:
    printf("Both 16 and 13 sector\n");
    break;
  }
  printf("Optimal bit timing: %d ns\n", di->optimalBitTiming * 125);
  printf("Hardware compatability flags: 0x%X\n", di->compatHardware);
  printf("Required RAM: %d K\n", di->requiredRam);
  printf("Largest track: %d bytes\n", di->largestTrack * 512);
  printf("\n");

  if (w->hasMetadata()) {
    printf("Metadata:\n");
    char key[256], value[1024];
    for (uint32_t i=0; w->getMetadataEntry(i, key, sizeof(key), value, sizeof(value)); i++) {
      if (strlen(value)) {
        printf("   %s: %s\n", key, value);
      }
    }
    printf("\n");
  }

  if (dumpflags & DUMP_QTMAP) {
    printf("Quarter-track map:\n");
    for (int i=0; i<140; i+=4) {
      printf("%2d     %3d => %3d     %3d => %3d     %3d => %3d     %3d => %3d\n",
             i/4,
             i, w->dataTrackNumberForQuarterTrack(i),
             i+1, w->dataTrackNumberForQuarterTrack(i+1),
             i+2, w->dataTrackNumberForQuarterTrack(i+2),
             i+3, w->dataTrackNumberForQuarterTrack(i+3));
    }
  }

  if (dumpflags & DUMP_QTCRC) {
    printf("Woz internal quarter-track CRCs:\n");
    // Dump the CRC32 for each Woz quarter-track
    for (int i=0; i<160; i++) {
      if (w->getTrackInfo(i)->trackData) {
        uint32_t crc=0;
        w->checksumWozDataTrack(i, &crc);
        printf("Woz track %d CRC32: 0x%lX\n", i, (unsigned long)crc);
      }
    }
  }

  if (!(dumpflags & DUMP_TRACK))
    return;

  for (int i=0; i<40; i++) {
    const trackInfo *t = w->getTrackInfo(i);
    printf("Track %d:\n", i);
    if (di->version == 1) {
      printf("  Starts at byte %lu\n", (unsigned long)t->startingByte);
    } else {
      printf("  Starts at block %d\n", t->startingBlock);
    }
    printf("  Number of blocks: %d\n", t->blockCount);
    printf("  Number of bits: %lu\n", (unsigned long)t->bitCount);
    if (t->bitCount && t->trackData) {
      if (dumpflags & DUMP_RAWTRACK) {
        printf("    Raw track data:\n");
        hexRows("    ", t->trackData, (t->bitCount/8)+((t->bitCount%8)?1:0));
      }

      if (dumpflags & DUMP_SECTOR)  {
        printf("    Sector dump:\n");
        // Look at the sectors in numerical order
        // FIXME: 13-sector support
        nibSector sectorData;
        for (int sector=0; sector<16; sector++) {
          if (w->readNibSectorDataFromDataTrack(w->dataTrackNumberForQuarterTrack(i*4),
                                                sector, &sectorData)) {
            printSectorFields(de44(sectorData.volume44), de44(sectorData.track44),
                              de44(sectorData.sector44), de44(sectorData.checksum44));
            printf("      Sector Data:\n");
            hexRows("      ", sectorData.data62, 342);
          }
        }
      }
    }

    if (dumpflags & DUMP_TOFILE) {
      // Dump each sector to a file for analysis
      uint8_t sectorData[256*16];
      if (w->decodeWozTrackToDsk(i, T_DSK, sectorData)) {
        for (int j=0; j<16; j++) {
          char buf[25];
          snprintf(buf, sizeof(buf), "t%ds%d", i, j);
          int fdout = open(buf, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR);
          write(fdout, &sectorData[256*j], 256);
          close(fdout);
        }
      } else {
        fprintf(stderr, "Unable to read track %d\n", i);
      }
    }

    if (dumpflags & DUMP_ORDEREDSECTOR) {
      printf("    Track-ordered sector dump:\n");
      static trackSectorDump found[64];
      uint8_t count = w->scanTrackOrderedSectors(i, found, 64);
      if (count > 64)
        count = 64;
      for (uint8_t s=0; s<count; s++) {
        printSectorFields(found[s].volume, found[s].track,
                          found[s].sector, found[s].checksum);
        if (found[s].haveData) {
          printf("      Sector Data:\n");
          hexRows("      ", found[s].data62, 342);
        }
      }
    }
  }
}

// Format a date for the ls listing, padded to the width of a real date so
// the columns stay aligned.
static void formatDate(time_t when, char *buf, size_t len)
{
  if (when == NO_DATE) {
    snprintf(buf, len, "%-15s", "<NO DATE>");
    return;
  }
  struct tm ts = *localtime(&when);
  // FIXME uc() that
  strftime(buf, len, "%d-%b-%y %H:%M", &ts);
}

static void printPascalEntry(Vent *e, bool verbose)
{
  static const char *kindName[] = {
    "     ", ".BAD ", ".CODE", ".TEXT", ".INFO",
    ".DATA", ".GRAF", ".FOTO", "SECDR"
  };
  uint8_t k = e->getPascalKind();
  uint16_t start = e->getPascalStartBlock();
  uint16_t next = e->getPascalNextBlock();
  printf(" %s  %5u blk  %-15s  (blocks %u..%u, %u bytes)\n",
         (k < 9) ? kindName[k] : "  ?  ",
         e->getBlocksUsed(), e->getName(), start,
         next ? (next - 1) : start,
         e->getEofLength());
  if (verbose) {
    const uint8_t *md = e->getPascalFent()->modDate;
    printf("  startBlock: %u\n", start);
    printf("  nextBlock:  %u\n", next);
    printf("  fileKind:   %u\n", k);
    printf("  bytesInLastBlock: %u\n", e->getPascalByteCount());
    printf("  modDate (raw): 0x%04X\n", md[0] | (md[1] << 8));
  }
}

static void printDosEntry(Vent *e)
{
  switch (e->getFileType()) {
  case FT_TXT:
    printf(" T ");
    break;
  case FT_BAS:
    printf(" A ");
    break;
  case FT_INT:
    printf(" I ");
    break;
  case FT_BIN:
    printf(" B ");
    break;
  case FT_SYS:
    printf(" S ");
    break;
  default:
    printf(" ? ");
    break;
  }
  printf("%.3d ", e->getBlocksUsed());
  printf("%s\n", e->getName());
}

static void printProdosEntry(Vent *e)
{
  if (e->isHeader()) {
    printf("\n  %s\n\n", e->getName());
    return;
  }

  // '*' marks a locked file (one that can't be written)
  printf("%s", (e->getAccessFlags() & 0x02) ? " " : "*");

  // A '+' after the name marks a GS/OS extended file - one that has a
  // resource fork as well as a data fork. Its listed byte size is the
  // 512 bytes of its extended key block, which is all the directory
  // entry records; 'inspect' shows what each fork really holds.
  printf("%15s%s ", e->getName(),
         e->getStorageType() == PRODOS_STORAGE_EXTENDED ? "+" : " ");

  uint8_t fileType = e->getFileType();
  uint16_t aux = e->getAuxType();
  char auxData[16] = "";
  switch (fileType) {
  case FT_TYP:
    printf("TYP");
    break;
  case FT_BAD:
    printf("BAD");
    break;
  case FT_TXT:
    printf("TXT");
    snprintf(auxData, sizeof(auxData), "R=%5d", aux);
    break;
  case FT_BIN:
    printf("BIN");
    snprintf(auxData, sizeof(auxData), "A=$%.4X", aux);
    break;
  case FT_DIR:
    printf("DIR");
    snprintf(auxData, sizeof(auxData), "[$%.4X]", e->keyPointerVal()); // pointer to first block
    break;
  case FT_ADB:
    printf("ADB"); // appleworks database
    break;
  case FT_AWP:
    printf("AWP"); // appleworks word processing
    break;
  case FT_ASP:
    printf("ASP"); // appleworks spreadsheet
    break;
  case FT_PAS:
    printf("PAS"); // pascal
    break;
  case FT_CMD:
    printf("CMD");
    break;
  case FT_US1:
  case FT_US2:
  case FT_US3:
  case FT_US4:
  case FT_US5:
  case FT_US6:
  case FT_US7:
  case FT_US8:
    printf("US%d", fileType & 0x0F);
    break;
  case FT_INT:
    printf("INT");
    break;
  case FT_IVR:
    printf("IVR"); // saved integer basic variables
    break;
  case FT_BAS:
    printf("BAS");
    snprintf(auxData, sizeof(auxData), "[$%.4X]", aux);
    break;
  case FT_VAR:
    printf("VAR"); // saved applesoft variables
    break;
  case FT_REL:
    printf("REL"); // relocatable EDASM object
    break;
  case FT_SYS:
    printf("SYS");
    snprintf(auxData, sizeof(auxData), "[$%.4X]", aux);
    break;
  default:
    printf("???"); // check appendix E of "Beneath ProDOS" to see what we missed
    break;
  }

  printf("  %6d  ", e->getBlocksUsed());
  char buf[255];
  formatDate(e->getLastModified(), buf, sizeof(buf));
  printf("%s   ", buf);
  formatDate(e->getCreationDate(), buf, sizeof(buf));
  printf("%s  %5d %s\n", buf, e->getEofLength(), auxData);
}

void printEntry(Vent *e, bool verbose)
{
  if (e->getIsPascal()) {
    printPascalEntry(e, verbose);
    return;
  }

  if (e->getIsDos33())
    printDosEntry(e);
  else
    printProdosEntry(e);

  if (!verbose)
    return;

  printf("Is directory header: %s\n", e->isHeader() ? "true" : "false");
  printf("Is DOS 3.3: %s\n", e->getIsDos33() ? "true" : "false");
  printf("EntryType: 0x%.2X\n", e->getStorageType());
  printf("FileType: 0x%.2X\n", e->getFileType());
  printf("keyPointer: 0x%.4X\n", e->keyPointerVal());
  printf("blocksUsed: 0x%.4X\n", e->getBlocksUsed());
  printf("eofLength: 0x%.8X\n", e->getEofLength());
  printf("typeData: 0x%.4X\n", e->getAuxType());

  if (!e->getIsDos33()) {
    const struct _prodosFent *pd = e->getProdosFent();
    printf("ProdosData dump:\n");

    printf("  type/len: 0x%.2X\n", pd->typelen);
    printf("  fileType: 0x%.2X\n", pd->fileType);
    printf("  keyPointer: 0x%.2X%.2X\n", pd->keyPointer[1], pd->keyPointer[0]);
    printf("  blocksUsed: 0x%.2X%.2X\n", pd->blocksUsed[1], pd->blocksUsed[0]);
    printf("  eofLength: 0x%.2X%.2X%.2X\n",
           pd->eofLength[2], pd->eofLength[1], pd->eofLength[0]);
    printf("  creatorVersion: 0x%.2X\n", pd->creatorVersion);
    printf("  minRequiredVersion: 0x%.2X\n", pd->minRequiredVersion);
    printf("  accessFlags: 0x%.2X\n", pd->accessFlags);
    printf("  typeData: 0x%.2X%.2X\n", pd->typeData[1], pd->typeData[0]);
    printf("  headerPointer: 0x%.2X%.2X\n", pd->headerPointer[1], pd->headerPointer[0]);
  } else {
    const struct _dosFdEntry *dd = e->getDosFent();
    printf("dosData dump:\n");
    printf("  first track/sector: %d/%d\n", dd->firstTrack, dd->firstSector);
    printf("  fileTypeAndFlags: 0x%.2X\n", dd->fileTypeAndFlags);
    printf("  fileLength: 0x%.2X%.2X\n", dd->fileLength[1], dd->fileLength[0]);
  }
}

void printListing(Vent *list)
{
  for (; list; list = list->nextEnt()) {
    printEntry(list, false);
  }
}

static void printDosVolumeInfo(DosSpector *fs)
{
  bool used[35][16];
  if (!fs->getSectorUsage(used))
    return;

  printf("Free sector map:\n");
  for (int i=0; i<35; i++) {
    printf("Track %.2d  ", i);
    for (int j=0; j<16; j++) {
      printf("%c", used[i][j] ? 'U' : 'f');
    }
    printf("\n");
  }
}

static void printProdosVolumeInfo(ProdosSpector *fs)
{
  uint32_t total = fs->volumeBlockCount();

  if (total <= 280) {
    // A floppy-sized volume gets the familiar track/sector grid
    bool trackSectorFree[35][16];
    memset(trackSectorFree, 0, sizeof(trackSectorFree));
    for (uint32_t i = 0; i < total; i++) {
      bool f = fs->isBlockFree(i);
      uint8_t t, sa, sb;
      ProdosSpector::blockToTrackSectors(i, &t, &sa, &sb);
      trackSectorFree[t][sa] = f;
      trackSectorFree[t][sb] = f;
    }

    printf("\n");
    char col[16] = { 'S','E','C','T','O','R',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
    printf("   TRACK           1               2  \n");
    printf("   0123456789ABCDEF0123456789ABCDEF012\n\n");
    for (int i = 0; i < 16; i++) {
      printf("%c%X ", col[i], i);
      for (int t = 0; t < 35; t++) {
        printf("%c", trackSectorFree[t][i] ? '.' : '*');
      }
      printf("\n");
    }
    printf("\n");
  } else {
    // Anything larger would need a screenful per hundred blocks as a
    // grid, so it gets a linear map, 64 blocks to a row
    printf("\nBlock free map (64 blocks/row):\n");
    for (uint32_t i = 0; i < total; i += 64) {
      printf("%6u  ", i);
      for (uint32_t j = 0; j < 64 && (i + j) < total; j++) {
        printf("%c", fs->isBlockFree(i + j) ? '.' : '*');
      }
      printf("\n");
    }
    printf("\n");
  }

  printf("\nBlocks total: %u\nBlocks free: %u\n",
         total, fs->calculateBlocksFree());
}

static void printPascalVolumeInfo(PascalSpector *fs)
{
  struct PascalVolumeInfo vi;
  if (!fs->getVolumeInfo(&vi))
    return;

  printf("Pascal volume: %s\n", vi.name);
  printf("Total blocks:  %u\n", vi.totalBlocks);
  printf("Files:         %u\n", vi.files);
  printf("Blocks used:   %u (files) + %u (boot+dir)\n",
         vi.fileBlocks, vi.reservedBlocks);
  printf("Blocks free:   %u\n", vi.freeBlocks);
  printf("Largest free contiguous region: %u blocks at block %u\n",
         vi.largestGap, vi.largestGapStart);
}

void printVolumeInfo(Wozspector *fs)
{
  if (DosSpector *d = dynamic_cast<DosSpector *>(fs))
    printDosVolumeInfo(d);
  else if (ProdosSpector *p = dynamic_cast<ProdosSpector *>(fs))
    printProdosVolumeInfo(p);
  else if (PascalSpector *pa = dynamic_cast<PascalSpector *>(fs))
    printPascalVolumeInfo(pa);
}

static void printDosFileDetails(DosSpector *fs, Vent *fp)
{
  struct DosFileAnalysis a;
  fs->analyzeFile(fp, &a);

  printf("  Catalog sector count: %u (= %u bytes allocated)\n",
         a.catalogSectors, (uint32_t)a.catalogSectors * 256);
  if (!a.hasTsList) {
    printf("  No T/S list pointer - cannot walk\n");
    return;
  }

  uint32_t walkTotal = a.tsListSectors + a.dataSectors;
  printf("  T/S walk: %u T/S list sector(s), %u data sector(s)",
         a.tsListSectors, a.dataSectors);
  if (a.sparseGaps) {
    printf(", %u sparse/trailing 0/0 entr%s",
           a.sparseGaps, a.sparseGaps == 1 ? "y" : "ies");
  }
  printf("\n");
  printf("  T/S walk total: %u sectors (= %u bytes)\n",
         walkTotal, walkTotal * 256);

  if (a.catalogSectors != walkTotal) {
    printf("  ** MISMATCH: catalog says %u sectors, walk found %u\n",
           a.catalogSectors, walkTotal);
  }

  if (!a.haveFirstData) {
    printf("  (no data sectors to read in-file header from)\n");
    return;
  }

  printf("  First data sector: %d/%d\n", a.firstDataTrack, a.firstDataSector);
  switch (fp->getFileType()) {
  case FT_BAS:
    printf("  Applesoft length header: %u bytes\n", a.logicalLength);
    break;
  case FT_INT:
    printf("  Integer BASIC length header: %u bytes\n", a.logicalLength);
    break;
  case FT_BIN:
    printf("  Binary header: load=$%.4X, length=%u bytes\n",
           a.loadAddress, a.logicalLength);
    break;
  case FT_TXT:
    printf("  Text file: no length header (0x00-terminated, possibly sparse)\n");
    break;
  default:
    printf("  File type 0x%.2X: no standard length header known\n",
           fp->getFileType());
    break;
  }

  if (a.lengthFarBelowAllocation) {
    printf("  ** Logical length (%u) is far smaller than data allocation (%u)\n",
           a.logicalLength, a.dataBytes);
    printf("     - possible stub loader with hidden payload, or deleted-tail padding\n");
  }
}

static void printProdosFileDetails(ProdosSpector *fs, Vent *fp)
{
  if (ProdosSpector::isExtendedFile(fp)) {
    // The entry describes the extended key block; the forks describe
    // themselves, in mini-entries inside it.
    printf("extended (GS/OS forked file)\n");
    printf("extended key block: %d (0x%X)\n", fp->keyPointerVal(),
           fp->keyPointerVal());
    for (int pass = 0; pass < 2; pass++) {
      bool resourceFork = (pass == 1);
      struct ProdosFork f;
      if (!fs->getForkInfo(fp, resourceFork, &f))
        return;
      const char *which = resourceFork ? "resource" : "data";
      if (!f.storageType) {
        printf("  %s fork: absent\n", which);
        continue;
      }
      const char *shape = ProdosSpector::storageTypeName(f.storageType);
      printf("  %s fork: %s, %u bytes in %u block%s, key block %u (0x%X)\n",
             which, shape ? shape : "unknown storage type", f.eof, f.blocksUsed,
             f.blocksUsed == 1 ? "" : "s", f.keyBlock, f.keyBlock);
    }
    return;
  }

  uint8_t expected = ProdosSpector::storageTypeForSize(fp->getEofLength());
  const char *shape = ProdosSpector::storageTypeName(expected);
  printf("%s\n", shape);
  if (fp->getStorageType() != expected) {
    printf("WARNING: type in dir says 0x%X, %s should be 0x%X\n",
           fp->getStorageType(), shape, expected);
  }

  if (expected == PRODOS_STORAGE_SAPLING) {
    // A sapling's key block is its index block
    uint16_t kpv = fp->keyPointerVal();
    printf("looking up key pointer %d (0x%X)\n", kpv, kpv);
    uint8_t blockData[512];
    if (!fs->readBlock(kpv, blockData)) {
      printf("ERROR: unable to read block\n");
      return;
    }
    printf("Sapling index block data:\n");
    for (int i=0; i<512; i+=16) {
      printf("%.4X  ", i);
      for (int j=0; j<16; j++) {
        printf("%.2X ", blockData[i+j]);
      }
      printf("\n");
    }
  } else if (expected == PRODOS_STORAGE_TREE) {
    printf("(Tree entries are not coded yet; cannot inspect.)\n");
  }
}

static void printPascalFileDetails(Vent *fp)
{
  if (!fp || !fp->getIsPascal()) {
    printf("Not a Pascal file entry\n");
    return;
  }
  uint16_t start = fp->getPascalStartBlock();
  uint16_t next  = fp->getPascalNextBlock();
  printf("Pascal file '%s':\n", fp->getName());
  printf("  kind:            %u (%s)\n", fp->getPascalKind(),
         PascalSpector::kindSuffix(fp->getPascalKind()));
  printf("  start block:     %u\n", start);
  printf("  next block:      %u\n", next);
  printf("  block count:     %u\n", (next > start) ? next - start : 0);
  printf("  bytes/last block:%u\n", fp->getPascalByteCount());
  printf("  logical length:  %u bytes\n", fp->getEofLength());
}

void printFileDetails(Wozspector *fs, Vent *fp)
{
  if (DosSpector *d = dynamic_cast<DosSpector *>(fs))
    printDosFileDetails(d, fp);
  else if (ProdosSpector *p = dynamic_cast<ProdosSpector *>(fs))
    printProdosFileDetails(p, fp);
  else if (dynamic_cast<PascalSpector *>(fs))
    printPascalFileDetails(fp);
}

void printBasicListing(FILE *out, const BasicListing &lines, bool integerBasic)
{
  for (size_t i = 0; i < lines.size(); i++) {
    if (integerBasic)
      fprintf(out, "%5u %s\n", lines[i].lineNumber, lines[i].text.c_str());
    else
      fprintf(out, "%d %s\n", lines[i].lineNumber, lines[i].text.c_str());
  }
}
