CFLAGS = -Wall -g
CXXFLAGS = $(CFLAGS)

SRCS=woz.cpp wozzle.cpp crc32.c nibutil.cpp

OBJS=woz.o crc32.o nibutil.o wozzle.o

.PHONY: test clean

all: $(OBJS)
	$(CXX) $(CFLAGS) -o wozzle $(OBJS)

depend: .depend

.depend: $(SRCS)
	rm -f ./.depend
	$(CC) $(CFLAGS) -MM $^ >  ./.depend;

include .depend

clean:
	rm -f $(OBJS) wozzle *~

test: all
	if [ -d "woz test images" ]; then \
		./tests/1-basic.sh; \
		./tests/2-halftrack.sh; \
	else \
		echo "You'll need to download the WOZ reference images to run tests." ; \
	fi

# DO NOT DELETE
