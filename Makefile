FUSELIBS=-losxfuse
MACFLAGS=-mmacosx-version-min=10.15

include wozcore.mk

CFLAGS = -Wall -g $(WOZCORE_CFLAGS) -I/usr/local/include/osxfuse -I/opt/homebrew/opt/readline/include -D_FILE_OFFSET_BITS=64
CXXFLAGS = $(CFLAGS)

# The disk image core (src/, listed in wozcore.mk) is built once into a
# static library that every tool links against
LIBWOZCORE = libwozcore.a

WOZSRCS=wozzle.cpp
WOZOBJS=wozzle.o

FUSESRCS=dosspector.cpp prodosspector.cpp pascalspector.cpp vent.cpp wozspector.cpp clireport.cpp wozfuse.cpp
FUSEOBJS=dosspector.o prodosspector.o pascalspector.o vent.o wozspector.o clireport.o wozfuse.o

WOZITSRCS=dosspector.cpp prodosspector.cpp pascalspector.cpp vent.cpp wozspector.cpp clireport.cpp wozit.cpp applesoft.cpp intbas.cpp
WOZITOBJS=dosspector.o prodosspector.o pascalspector.o vent.o wozspector.o clireport.o wozit.o applesoft.o intbas.o

WOZMOSISSRCS=dosspector.cpp prodosspector.cpp pascalspector.cpp vent.cpp wozspector.cpp wozmosis.cpp
WOZMOSISOBJS=dosspector.o prodosspector.o pascalspector.o vent.o wozspector.o wozmosis.o

.PHONY: test clean unit-tests

all: wozzle wozit wozmosis

$(LIBWOZCORE): $(WOZCORE_OBJS)
	rm -f $@
	$(AR) rcs $@ $(WOZCORE_OBJS)

UNIT_BINS = tests/unit-intbas tests/unit-applesoft tests/unit-nib tests/unit-host

unit-tests: $(UNIT_BINS)

tests/unit-intbas: tests/unit-intbas.cpp intbas.o $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-intbas.cpp intbas.o $(LIBWOZCORE) -o $@

tests/unit-applesoft: tests/unit-applesoft.cpp applesoft.o $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-applesoft.cpp applesoft.o $(LIBWOZCORE) -o $@

tests/unit-nib: tests/unit-nib.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-nib.cpp $(LIBWOZCORE) -o $@

tests/unit-host: tests/unit-host.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-host.cpp $(LIBWOZCORE) -o $@

wozzle: $(WOZOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozzle $(WOZOBJS) $(LIBWOZCORE)

wozfuse: $(FUSEOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozfuse $(FUSEOBJS) $(LIBWOZCORE)

wozit: $(WOZITOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -L/opt/homebrew/opt/readline/lib -o wozit $(WOZITOBJS) $(LIBWOZCORE) -lreadline -lncurses

wozmosis: $(WOZMOSISOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozmosis $(WOZMOSISOBJS) $(LIBWOZCORE)

depend: .depend

.depend: $(WOZCORE_SRCS) $(WOZSRCS) $(FUSESRCS) $(WOZITSRCS) $(WOZMOSISSRCS)
	rm -f ./.depend
	for f in $^; do $(CC) $(CFLAGS) -MM -MT $${f%.*}.o $$f; done > ./.depend

include .depend

clean:
	rm -f $(WOZCORE_OBJS) $(LIBWOZCORE) $(WOZOBJS) $(FUSEOBJS) $(WOZITOBJS) $(WOZMOSISOBJS) pascalspector.o wozzle wozit wozmosis *~ $(UNIT_BINS)
	rm -rf *.dSYM tests/*.dSYM
	rm -f tests/nib-test tests/test-pattern tests/make-test-disk
	rm -f tests/pattern.dsk tests/pattern-out.dsk

test: all unit-tests
	./run-tests.sh

# DO NOT DELETE
