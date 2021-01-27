#include "vent.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

Vent::Vent()
{
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
  assert(di->entryLength == 0x27);
  assert(di->entriesPerBlock == 0x0D);

  //  ...
}

Vent::Vent(struct _fent *fi)
{
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

Vent::Vent(const Vent &vi)
{
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
}

Vent::~Vent()
{
}

void Vent::Dump()
{
  if (accessFlags & 0x02) {
    printf(" ");
  } else {
    // locked
    printf("*");
  }

  printf("%15s  ", name);
  
  char auxData[16] = "";
  switch (fileType) {
  case 0:
    printf("TYP");
    break;
  case 1:
    printf("BAD");
    break;
  case 4:
    printf("TXT");
    snprintf(auxData, sizeof(auxData), "R=%5d", typeData);
    break;
  case 6:
    printf("BIN");
    snprintf(auxData, sizeof(auxData), "A=$%.4X", typeData);
    break;
  case 0x0F:
    printf("DIR");
    break;
  case 0x19:
    printf("ADB"); // appleworks database
    break;
  case 0x1A:
    printf("AWP"); // appleworks word processing
    break;
  case 0x1B:
    printf("ASP"); // appleworks spreadsheet
    break;
  case 0xEF:
    printf("PAS"); // pascal
    break;
  case 0xF0:
    printf("CMD");
    break;
  case 0xF1:
  case 0xF2:
  case 0xF3:
  case 0xF4:
  case 0xF5:
  case 0xF6:
  case 0xF7:
  case 0xF8:
    printf("US%d", fileType & 0x0F);
    break;
  case 0xFC:
    printf("BAS");
    snprintf(auxData, sizeof(auxData), "[$%.4X]", typeData);
    break;
  case 0xFD:
    printf("VAR"); // saved applesoft variables
    break;
  case 0xFE:
    printf("REL"); // relocatable EDASM object
    break;
  case 0xFF:
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

