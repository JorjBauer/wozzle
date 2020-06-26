#!/bin/bash

runcmd() {
    #    open -a ~/Applications/Virtual\ \]\[/Virtual\ \]\[.app $1
    echo 'hi'
}

# Start of tests
BTMD5=`md5sum woz\ test\ images/WOZ\ 2.0/The\ Bilestoad\ -\ Disk\ 1,\ Side\ A.woz|cut -f1 -d' '`


echo 1: Testing Woz1 quarter-track image to Woz2
./wozzle -i woz\ test\ images/WOZ\ 1.0/The\ Bilestoad\ -\ Disk\ 1,\ Side\ A.woz -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "c3b4a44d2085d8f0bb5ae5b77c15fec0" ]; then
    echo Woz1 to Woz2 conversion failed \[$SUM\]
    echo Check to see if this image boots, and update test 1 if so
    runcmd /tmp/out.nib
    exit 1
fi

echo 2: Testing Woz2 quarter-track image to Woz2
./wozzle -i woz\ test\ images/WOZ\ 2.0/The\ Bilestoad\ -\ Disk\ 1,\ Side\ A.woz -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "$BTMD5" ]; then
    echo Woz2 to Woz2 conversion failed \[$SUM\]
    echo Check to see if this image boots, and update test 2 if so
    runcmd /tmp/out.dsk
    exit 1
fi


exit 0
