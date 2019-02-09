#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NIBTRACKSIZE 0x1A00

#define         nib1(a) (((a & 0xAA) >> 1) | 0xAA)
#define         nib2(b) (((b & 0x55)     ) | 0xAA)
#define denib(a, b) ((((a) & ~0xAA) << 1) | ((b) & ~0xAA))

#define GAP 0xFF

enum nibErr {
  errorNone           = 0,
  errorMissingSectors = 1,
  errorBadData        = 2
};

uint32_t nibblizeTrack(uint8_t outputBuffer[NIBTRACKSIZE], const uint8_t rawTrackBuffer[256*16],
		       uint8_t diskType, int8_t track);

nibErr denibblizeTrack(const uint8_t input[NIBTRACKSIZE], uint8_t rawTrackBuffer[256*16],
                       uint8_t diskType, int8_t track);

//bool decodeData(const uint8_t trackBuffer[343], uint8_t output[256]);

//void encodeData(uint8_t outputBuffer[343], const uint8_t input[256]);
