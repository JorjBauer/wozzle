#!/bin/sh

g++ -I.. nib-test.cpp -o nib-test ../woz.o ../nibutil.o ../crc32.o &&\
    ./nib-test

