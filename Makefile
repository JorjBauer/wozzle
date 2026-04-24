FUSELIBS=-losxfuse
MACFLAGS=-mmacosx-version-min=10.15

CFLAGS = -Wall -g -I/usr/local/include/osxfuse -I/opt/homebrew/opt/readline/include -D_FILE_OFFSET_BITS=64
CXXFLAGS = $(CFLAGS)

WOZSRCS=woz.cpp wozzle.cpp crc32.c nibutil.cpp
WOZOBJS=woz.o crc32.o nibutil.o wozzle.o

FUSESRCS=woz.cpp crc32.c nibutil.cpp dosspector.cpp prodosspector.cpp vent.cpp wozfuse.cpp
FUSEOBJS=woz.o crc32.o nibutil.o dosspector.o prodosspector.o vent.o wozfuse.o

WOZITSRCS=woz.cpp crc32.c nibutil.cpp dosspector.cpp prodosspector.cpp vent.cpp wozspector.cpp wozit.cpp applesoft.cpp intbas.cpp
WOZITOBJS=woz.o crc32.o nibutil.o dosspector.o prodosspector.o vent.o wozspector.o wozit.o applesoft.o intbas.o

.PHONY: test clean unit-tests

all: wozzle wozit

UNIT_BINS = tests/unit-intbas tests/unit-applesoft tests/unit-nib

unit-tests: $(UNIT_BINS)

tests/unit-intbas: tests/unit-intbas.cpp intbas.o
	$(CXX) $(CFLAGS) -I. tests/unit-intbas.cpp intbas.o -o $@

tests/unit-applesoft: tests/unit-applesoft.cpp applesoft.o
	$(CXX) $(CFLAGS) -I. tests/unit-applesoft.cpp applesoft.o -o $@

tests/unit-nib: tests/unit-nib.cpp nibutil.o
	$(CXX) $(CFLAGS) -I. tests/unit-nib.cpp nibutil.o -o $@

wozzle: $(WOZOBJS)
	$(CXX) $(CFLAGS) -o wozzle $(WOZOBJS) 

wozfuse: $(FUSEOBJS)
	$(CXX) $(CFLAGS) -o wozfuse $(FUSEOBJS) 

wozit: $(WOZITOBJS)
	$(CXX) $(CFLAGS) -L/opt/homebrew/opt/readline/lib -lreadline -o wozit $(WOZITOBJS)

depend: .depend

.depend: $(WOZSRCS) $(FUSESRCS) $(WOZITSRCS)
	rm -f ./.depend
	$(CC) $(CFLAGS) -MM $^ >  ./.depend;

include .depend

clean:
	rm -f $(WOZOBJS) $(FUSEOBJS) $(WOZITOBJS) wozzle wozit *~ $(UNIT_BINS)
	rm -rf *.dSYM tests/*.dSYM
	rm -f tests/nib-test tests/test-pattern tests/make-test-disk
	rm -f tests/pattern.dsk tests/pattern-out.dsk

test: all unit-tests
	./run-tests.sh

# DO NOT DELETE
