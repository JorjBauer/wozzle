#!/bin/bash


echo 1: Decoding DOS 3.3 info from Woz1 image
SUM=`./wozzle -I woz\ test\ images/WOZ\ 1.0/DOS\ 3.3\ System\ Master.woz -d|md5sum|cut -f1 -d' '`
if [ "$SUM" != "326bdabd05e9d20e4d481eaee35e25bc" ]; then
    echo "Woz1 DOS3.3 decode ($SUM) doesn't match expected sum; inspect the output by hand change checksum if necessary"
    exit 1
fi


exit 0
