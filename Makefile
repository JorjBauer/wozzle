FUSELIBS=-losxfuse
MACFLAGS=-mmacosx-version-min=10.15

CFLAGS = -Wall -g -I/usr/local/include/osxfuse -D_FILE_OFFSET_BITS=64
CXXFLAGS = $(CFLAGS)

WOZSRCS=woz.cpp wozzle.cpp crc32.c nibutil.cpp
WOZOBJS=woz.o crc32.o nibutil.o wozzle.o

FUSESRCS=woz.cpp crc32.c nibutil.cpp dosspector.cpp prodosspector.cpp vent.cpp wozfuse.cpp
FUSEOBJS=woz.o crc32.o nibutil.o dosspector.o prodosspector.o vent.o wozfuse.o

WOZITSRCS=woz.cpp crc32.c nibutil.cpp dosspector.cpp prodosspector.cpp vent.cpp wozspector.cpp wozit.cpp
WOZITOBJS=woz.o crc32.o nibutil.o dosspector.o prodosspector.o vent.o wozspector.o wozit.o

.PHONY: test clean

all: wozzle

wozzle: $(WOZOBJS)
	$(CXX) $(CFLAGS) -o wozzle $(WOZOBJS) 

wozfuse: $(FUSEOBJS)
	$(CXX) $(CFLAGS) -o wozfuse $(FUSEOBJS) 

wozit: $(WOZITOBJS)
	$(CXX) $(CFLAGS) -lreadline -o wozit $(WOZITOBJS)

depend: .depend

.depend: $(WOZSRCS) $(FUSESRCS) $(WOZITSRCS)
	rm -f ./.depend
	$(CC) $(CFLAGS) -MM $^ >  ./.depend;

include .depend

clean:
	rm -f $(OBJS) wozzle *~

test: all
	if [ -d "woz test images" ]; then \
		./tests/1-basic.sh && \
		./tests/2-halftrack.sh && \
		./tests/3-small-basic.sh && \
		./tests/4-small-halftrack.sh; \
		./tests/5-dos.sh; \
	else \
		echo "You'll need to download the WOZ reference images to run tests." ; \
	fi

# DO NOT DELETE
