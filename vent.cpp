#include "vent.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

Vent::Vent()
{
  isDos33 = false;
  children = next = NULL;
}

time_t prodosDateToEpoch(uint8_t prodosDate[4])
{
  uint8_t year = prodosDate[1] >> 1;
  uint8_t month = ((prodosDate[1] & 0x01) << 3) | ((prodosDate[0] & 0xE0) >> 5);
  uint8_t day = prodosDate[0] & 0x1F;
  uint8_t hour = prodosDate[3] & 0x1F;
  uint8_t minute = prodosDate[2] & 0x3F;

  struct tm t;
  t.tm_year = year; // 1900 + whatever. FIXME.
  t.tm_mon = month - 1; // 0 for Jan
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = 0;
  
  return mktime(&t);
}

Vent::Vent(struct _subdirent *di)
{
  isDirectoryHeader = true;
  isDos33 = false;
  
  this->entryType = (di->typelen & 0xF0) >> 4;
  memcpy(this->name, di->name, 15);
  this->name[15] = '\0';
  this->name[di->typelen & 0x0F] = '\0';

  this->creatorVersion = di->creatorVersion;
  this->minRequiredVersion = di->minRequiredVersion;
  this->accessFlags = di->accessFlags;
  assert(di->entryLength == 0x27);
  assert(di->entriesPerBlock == 0x0D);
  this->activeFileCount = di->fileCount[1] * 256 + di->fileCount[0];

  this->children = this->next = NULL;

  //  ...
}

Vent::Vent(struct _prodosFent *fi)
{
  isDirectoryHeader = false;
  isDos33 = false;
  
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
  for (int i=0; i<30; i++) {
    name[i] = fe->fileName[i] ^ 0x80;
    if (name[i] == ' ') {
      name[i] = '\0';
    }
  }
  name[30] = '\0';
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
    printf("Unhandled file type %d\n", fe->fileTypeAndFlags & 0x7F);
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

void Vent::Dump()
{
  if (isDos33) {
    switch (fileType) {
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
    printf("%.3d ", blocksUsed);
    printf("%s\n", name);
  } else {
    if (isDirectoryHeader) {
      printf("\n  %s\n\n", name);
    } else {
      if (accessFlags & 0x02) {
	printf(" ");
      } else {
	// locked
	printf("*");
      }
      
      printf("%15s  ", name);
      
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
	snprintf(auxData, sizeof(auxData), "R=%5d", typeData);
	break;
      case FT_BIN:
	printf("BIN");
	snprintf(auxData, sizeof(auxData), "A=$%.4X", typeData);
	break;
      case FT_DIR:
	printf("DIR");
	snprintf(auxData, sizeof(auxData), "[$%.4X]", keyPointer); // pointer to first block
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
	snprintf(auxData, sizeof(auxData), "[$%.4X]", typeData);
	break;
      case FT_VAR:
	printf("VAR"); // saved applesoft variables
	break;
      case FT_REL:
	printf("REL"); // relocatable EDASM object
	break;
      case FT_SYS:
	printf("SYS");
	snprintf(auxData, sizeof(auxData), "[$%.4X]", typeData);
	break;
      default:
	printf("???"); // check appendix E of "Beneath ProDOS" to see what we missed
	break;
      }
      
      printf("  %6d  ", blocksUsed);
      struct tm ts;
      ts = *localtime(&lastModified);
      char buf[255];
      strftime(buf, sizeof(buf), "%d-%b-%y %H:%M", &ts);
      // FIXME uc() that 
      printf("%s   ", buf);
      ts = *localtime(&creationDate);
      strftime(buf, sizeof(buf), "%d-%b-%y %H:%M", &ts);
      // FIXME uc() that 
      printf("%s  %5d %s\n", buf, eofLength, auxData);
    }
  }
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

uint16_t Vent::keyPointerVal()
{
  return keyPointer;
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

