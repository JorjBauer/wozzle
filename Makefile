CFLAGS = -Wall -g

SRCS=Makefile woz.cpp wozzle.cpp crc32.c nibutil.cpp

OBJS=woz.o crc32.o nibutil.o wozzle.o

all: $(OBJS)
	$(CXX) $(CFLAGS) -o wozzle $(OBJS)

%.o: %.cpp %.c
	$(CXX) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) wozzle *~

test: all
	if [ -d "woz test images" ]; then \
		./tests/1-basic.sh; \
	else \
		echo "You'll need to download the WOZ reference images to run tests." ; \
	fi

