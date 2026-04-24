#!/bin/sh

gcc make-test-disk.c -o make-test-disk &&\
./make-test-disk &&\
g++ -I.. test-pattern.cpp -o test-pattern ../woz.o ../nibutil.o ../crc32.o &&\
./test-pattern
