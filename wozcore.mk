# The wozzle disk image library, for other projects' Makefiles.
#
# If you're building wozzle into something else, include this file and
# use the variables it sets instead of naming the library's files
# yourself, so a file added here doesn't break your build:
#
#   include path/to/wozzle/wozcore.mk
#   CXXFLAGS += $(WOZCORE_CFLAGS)
#   myprogram: $(MYOBJS) $(WOZCORE_OBJS)
#
#   WOZCORE_DIR     where wozzle is
#   WOZCORE_SRCS    the library's sources (C and C++)
#   WOZCORE_OBJS    their objects, built next to the sources
#   WOZCORE_CFLAGS  the include path for the library's headers
#
# The same src/ directory is an Arduino library (see library.properties),
# which is how a Teensy build gets it.

WOZCORE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
WOZCORE_SRCS := $(addprefix $(WOZCORE_DIR)/src/,woz.cpp nibutil.cpp crc32.c wozmsg.c wozhost.c)
WOZCORE_OBJS := $(addsuffix .o,$(basename $(WOZCORE_SRCS)))
WOZCORE_CFLAGS := -I$(WOZCORE_DIR)/src
