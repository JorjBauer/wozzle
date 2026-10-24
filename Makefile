MACFLAGS=-mmacosx-version-min=10.15

include wozcore.mk

CFLAGS = -Wall -g $(WOZCORE_CFLAGS) -I/opt/homebrew/opt/readline/include -D_FILE_OFFSET_BITS=64

# wozfuse needs FUSE 3 (libfuse, or macFUSE on a Mac), and is only built
# when pkg-config can find it
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell pkg-config --libs fuse3 2>/dev/null)
CXXFLAGS = $(CFLAGS)

# The disk image core (src/, listed in wozcore.mk) is built once into a
# static library that every tool links against
LIBWOZCORE = libwozcore.a

WOZSRCS=wozzle.cpp
WOZOBJS=wozzle.o

FUSESRCS=wozfuse.cpp
FUSEOBJS=wozfuse.o

WOZITSRCS=clireport.cpp wozit.cpp
WOZITOBJS=clireport.o wozit.o

WOZMOSISSRCS=wozmosis.cpp
WOZMOSISOBJS=wozmosis.o

.PHONY: test clean unit-tests

all: wozzle wozit wozmosis $(if $(FUSE_LIBS),wozfuse)

$(LIBWOZCORE): $(WOZCORE_OBJS)
	rm -f $@
	$(AR) rcs $@ $(WOZCORE_OBJS)

UNIT_BINS = tests/unit-intbas tests/unit-applesoft tests/unit-nib tests/unit-host tests/unit-capi tests/unit-dosref

unit-tests: $(UNIT_BINS)

tests/unit-intbas: tests/unit-intbas.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-intbas.cpp $(LIBWOZCORE) -o $@

tests/unit-applesoft: tests/unit-applesoft.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-applesoft.cpp $(LIBWOZCORE) -o $@

tests/unit-nib: tests/unit-nib.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-nib.cpp $(LIBWOZCORE) -o $@

tests/unit-host: tests/unit-host.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-host.cpp $(LIBWOZCORE) -o $@

# Compiled as C, to show wozlib.h works from C; linked as C++
tests/unit-capi: tests/unit-capi.c $(LIBWOZCORE)
	$(CC) $(CFLAGS) -std=c99 -c tests/unit-capi.c -o tests/unit-capi.o
	$(CXX) $(CFLAGS) tests/unit-capi.o $(LIBWOZCORE) -o $@

tests/unit-dosref: tests/unit-dosref.c $(LIBWOZCORE)
	$(CC) $(CFLAGS) -std=c99 -c tests/unit-dosref.c -o tests/unit-dosref.o
	$(CXX) $(CFLAGS) tests/unit-dosref.o $(LIBWOZCORE) -o $@

wozzle: $(WOZOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozzle $(WOZOBJS) $(LIBWOZCORE)

wozfuse.o: wozfuse.cpp
	$(CXX) $(CXXFLAGS) $(FUSE_CFLAGS) -c wozfuse.cpp -o $@

wozfuse: $(FUSEOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozfuse $(FUSEOBJS) $(LIBWOZCORE) $(FUSE_LIBS)

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
	rm -f $(WOZCORE_OBJS) $(LIBWOZCORE) $(WOZOBJS) $(FUSEOBJS) $(WOZITOBJS) $(WOZMOSISOBJS) wozzle wozit wozmosis wozfuse *~ $(UNIT_BINS)
	rm -rf *.dSYM tests/*.dSYM
	rm -f tests/nib-test tests/test-pattern tests/make-test-disk tests/unit-capi.o tests/unit-dosref.o
	rm -f tests/pattern.dsk tests/pattern-out.dsk

test: all unit-tests
	./run-tests.sh

# DO NOT DELETE
