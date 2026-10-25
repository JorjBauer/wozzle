#include "vent.h"
#include "wozmsg.h"
#include "wozhost.h"

#include <stdio.h>
#include <string.h>

Vent::Vent()
{
  isDos33 = false;
  isPascal = false;
  children = next = NULL;

  memset(&prodosData, 0, sizeof(prodosData));
  memset(&dosData, 0, sizeof(dosData));
  memset(&pascalData, 0, sizeof(pascalData));
}

// Little-endian 16-bit read from a raw on-disk field.
static inline uint16_t rd16(const uint8_t p[2])
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

Vent::Vent(struct _pascalFent *fp)
{
  isDirectoryHeader = false;
  isDos33 = false;
  isPascal = true;

  memcpy(&pascalData, fp, sizeof(pascalData));
  memset(&prodosData, 0, sizeof(prodosData));
  memset(&dosData, 0, sizeof(dosData));

  pascalStartBlock = rd16(fp->firstBlock);
  pascalNextBlock  = rd16(fp->nextBlock);
  pascalType       = rd16(fp->fileType);
  pascalByteCount  = rd16(fp->byteCount);

  uint8_t nl = fp->name[0];
  if (nl > 15) nl = 15;
  memcpy(this->name, &fp->name[1], nl);
  this->name[nl] = '\0';

  // Present the file kind through the generic fileType field; it never
  // collides with 0x0F (ProDOS DIR), so isDirectory() stays false.
  this->fileType = (uint8_t)(pascalType & 0x0F);
  this->keyPointer = pascalStartBlock;
  this->entryType = 0;
  // A file spans (next-start) blocks; the last one holds byteCount bytes.
  this->blocksUsed = (pascalNextBlock > pascalStartBlock)
                       ? (pascalNextBlock - pascalStartBlock) : 0;
  this->eofLength = (blocksUsed ? (blocksUsed - 1) * 512 : 0) + pascalByteCount;
  this->creationDate = 0;
  this->lastModified = 0;
  this->accessFlags = 0;
  this->typeData = 0;
  this->activeFileCount = 0;
  this->children = this->next = NULL;
}

void dosCatalogName(const struct _dosFdEntry *fe, char out[31])
{
  for (int i = 0; i < 30; i++)
    out[i] = fe->fileName[i] & 0x7F;
  out[30] = '\0';
  for (int i = 29; i >= 0 && out[i] == ' '; i--)
    out[i] = '\0';
}

time_t prodosDateToEpoch(uint8_t prodosDate[4])
{
  // ProDOS treats a zero date word as "no date" (CAT shows <NO DATE>)
  if (prodosDate[0] == 0 && prodosDate[1] == 0)
    return NO_DATE;

  uint8_t year = prodosDate[1] >> 1;
  uint8_t month = ((prodosDate[1] & 0x01) << 3) | ((prodosDate[0] & 0xE0) >> 5);
  uint8_t day = prodosDate[0] & 0x1F;
  uint8_t hour = prodosDate[3] & 0x1F;
  uint8_t minute = prodosDate[2] & 0x3F;

  // Zero the struct and let mktime() work out DST itself. Leaving tm_isdst
  // uninitialized makes mktime() read stack garbage and shift some dates by
  // an hour when ls converts them back with localtime().
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_isdst = -1;
  // Tech Note #28's two-digit years. Some newer tools store 100-127 for
  // 2000-2027, which reads the same way here.
  t.tm_year = year < 40 ? year + 100 : year;
  t.tm_mon = month - 1; // 0 for Jan
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = 0;
  
  return mktime(&t);
}

bool epochToProdosDate(time_t t, uint8_t prodosDate[4])
{
  memset(prodosDate, 0, 4);
  if (t == NO_DATE)
    return false;
  struct tm *lt = localtime(&t);
  // Only 1940-2039 has a two-digit year that reads back the same
  if (!lt || lt->tm_year < 40 || lt->tm_year >= 140)
    return false;
  uint8_t year = lt->tm_year % 100;
  uint8_t month = lt->tm_mon + 1;
  prodosDate[0] = (uint8_t)(((month & 0x07) << 5) | (lt->tm_mday & 0x1F));
  prodosDate[1] = (uint8_t)((year << 1) | (month >> 3));
  prodosDate[2] = (uint8_t)lt->tm_min;
  prodosDate[3] = (uint8_t)lt->tm_hour;
  return true;
}

bool prodosDateNow(uint8_t prodosDate[4])
{
  return epochToProdosDate(wozNow(), prodosDate);
}

Vent::Vent(struct _subdirent *di)
{
  isDirectoryHeader = true;
  isDos33 = false;
  isPascal = false;

  this->entryType = (di->typelen & 0xF0) >> 4;
  memcpy(this->name, di->name, 15);
  this->name[15] = '\0';
  this->name[di->typelen & 0x0F] = '\0';

  this->creatorVersion = di->creatorVersion;
  this->minRequiredVersion = di->minRequiredVersion;
  this->accessFlags = di->accessFlags;
  // A valid ProDOS directory header always has 0x27-byte entries and
  // 0x0D of them per block. Callers (e.g. descendTree) are expected to
  // validate this before constructing us, but don't abort the process if
  // something slips through with a malformed header - just warn and treat
  // it as an empty directory so the tool stays usable on damaged images.
  if (di->entryLength != 0x27 || di->entriesPerBlock != 0x0D) {
    wozMsg(WOZ_MSG_WARNING,
           "WARNING: directory header has unexpected geometry "
           "(entryLength=0x%02X, entriesPerBlock=0x%02X); "
           "treating it as empty.",
           di->entryLength, di->entriesPerBlock);
    this->activeFileCount = 0;
    this->children = this->next = NULL;
    return;
  }
  this->activeFileCount = di->fileCount[1] * 256 + di->fileCount[0];

  this->children = this->next = NULL;

  //  ...
}

Vent::Vent(struct _prodosFent *fi)
{
  isDirectoryHeader = false;
  isDos33 = false;
  isPascal = false;

  memcpy(&prodosData, fi, sizeof(prodosData));
  
  this->entryType = (fi->typelen & 0xF0) >> 4;
  memcpy(this->name, fi->name, 15);
  this->name[15] = '\0';
  this->name[fi->typelen & 0x0F] = '\0';
  this->fileType = fi->fileType;
  this->keyPointer = fi->keyPointer[1] * 256 + fi->keyPointer[0];
  this->blocksUsed = fi->blocksUsed[1] * 256 + fi->blocksUsed[0];
  this->eofLength = (fi->eofLength[2] << 16) | (fi->eofLength[1] << 8) | (fi->eofLength[0]);
  this->creationDate = prodosDateToEpoch(fi->creationDate);
  this->creatorVersion = fi->creatorVersion;
  this->minRequiredVersion = fi->minRequiredVersion;
  this->accessFlags = fi->accessFlags;
  this->typeData = fi->typeData[1] * 256 + fi->typeData[0];
  this->lastModified = prodosDateToEpoch(fi->lastModified);
  this->headerPointer = fi->headerPointer[1] * 256 + fi->headerPointer[0];
  this->children = NULL;
  this->next = NULL;
}

Vent::Vent(struct _dosFdEntry *fe)
{
  isDirectoryHeader = false;
  isDos33 = true;
  isPascal = false;
  memcpy(&dosData, fe, sizeof(dosData));
  memset(&prodosData, 0, sizeof(prodosData));
  memset(&pascalData, 0, sizeof(pascalData));

  // A DOS 3.3 catalog entry carries none of these, but a caller asking
  // for them should get something defined
  entryType = 0;
  keyPointer = 0;
  eofLength = 0;
  creationDate = lastModified = NO_DATE;
  creatorVersion = minRequiredVersion = 0;
  accessFlags = 0;
  typeData = 0;
  headerPointer = 0;
  activeFileCount = 0;
  pascalStartBlock = pascalNextBlock = pascalByteCount = pascalType = 0;

  dosCatalogName(fe, name);
  switch (fe->fileTypeAndFlags & 0x7F) {
  case 0:
    fileType = FT_TXT;
    break;
  case 1:
    fileType = FT_INT;
    break;
  case 2:
    fileType = FT_BAS;
    break;
  case 4:
    fileType = FT_BIN;
    break;
  case 8:
    // Not sure how to represent this here. SYS?
    fileType = FT_SYS;
    break;
  case 16:
    fileType = FT_REL;
    break;
  case 32: // 'A'
  case 64: // 'B'
  default:
    fileType = FT_TYP;
    wozMsg(WOZ_MSG_WARNING, "Unhandled file type %d", fe->fileTypeAndFlags & 0x7F);
  }
  blocksUsed = fe->fileLength[0] + fe->fileLength[1] * 256;
  firstTrack = fe->firstTrack;
  firstSector = fe->firstSector;
  // FIXME: what about file flags? Locked, at least? in fileTypeAndFlags
  this->children = this->next = NULL;
}

Vent::Vent(const Vent &vi)
{
  this->isDirectoryHeader = vi.isDirectoryHeader;
  this->isDos33 = vi.isDos33;
  this->isPascal = vi.isPascal;
  this->pascalStartBlock = vi.pascalStartBlock;
  this->pascalNextBlock = vi.pascalNextBlock;
  this->pascalByteCount = vi.pascalByteCount;
  this->pascalType = vi.pascalType;
  memcpy(&this->prodosData, &vi.prodosData, sizeof(prodosData));
  memcpy(&this->dosData, &vi.dosData, sizeof(dosData));
  memcpy(&this->pascalData, &vi.pascalData, sizeof(pascalData));
				      
  this->entryType = vi.entryType;
  memcpy(this->name, vi.name, 16);
  this->fileType = vi.fileType;
  this->keyPointer = vi.keyPointer;
  this->blocksUsed = vi.blocksUsed;
  this->eofLength = vi.eofLength;
  this->creationDate = vi.creationDate;
  this->creatorVersion = vi.creatorVersion;
  this->minRequiredVersion = vi.minRequiredVersion;
  this->accessFlags = vi.accessFlags;
  this->typeData = vi.typeData;
  this->lastModified = vi.lastModified;
  this->headerPointer = vi.headerPointer;
  this->children = vi.children;
  this->next = vi.next;

  this->activeFileCount = vi.activeFileCount;
}

Vent::~Vent()
{
}

Vent *Vent::nextEnt()
{
  return next;
}

void Vent::nextEnt(Vent *n)
{
  next = n;
}

Vent *Vent::childrenEnt()
{
  return children;
}

void Vent::childrenEnt(Vent *c)
{
  children = c;
}

bool Vent::isDirectory()
{
  return (fileType == 0x0F);
}

bool Vent::isHeader()
{
  return isDirectoryHeader;
}

uint16_t Vent::keyPointerVal()
{
  return keyPointer;
}

void Vent::keyPointerVal(uint16_t kp)
{
  keyPointer = kp;
}

const char *Vent::getName()
{
  return name;
}

uint8_t Vent::getFirstTrack()
{
  // For DOS 3.3 images
  return firstTrack;
}

uint8_t Vent::getFirstSector()
{
  return firstSector;
}

uint8_t Vent::getStorageType()
{
  return entryType;
}

uint32_t Vent::getEofLength()
{
  return eofLength;
}

uint8_t Vent::getFileType()
{
  return fileType;
}

uint16_t Vent::getBlocksUsed()
{
  return blocksUsed;
}

uint16_t Vent::getActiveFileCount()
{
  return activeFileCount;
}

