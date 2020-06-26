#!/bin/bash

# I do this testing under macOS; the best test I have at the moment is whether or not Virtual ][
# will run the images...
runcmd() {
    open -a ~/Applications/Virtual\ \]\[/Virtual\ \]\[.app $1
}


# Start of tests
NIBMD5=a9712db993c5e761fe4bc400d9debe81
DSKMD5=a76ee417f260d3238ad1191412f8a5e5
WOZ2DSKMD5=1ed48d059865e922f981a51df1f71ff3
WOZ2NIBMD5=79d7fd2814867afc1a3c21ddb96ca862

echo 1: Testing Woz1 to NIB
./wozzle -s -i woz\ test\ images/WOZ\ 1.0/DOS\ 3.3\ System\ Master.woz -o /tmp/out.nib 
SUM=`md5sum /tmp/out.nib|cut -f1 -d' '`
if [ "$SUM" != "$NIBMD5" ]; then
    echo Woz1 to NIB conversion failed [$SUM]
    echo Check to see if this image boots, and update test 1 if so
    runcmd /tmp/out.nib
    exit 1
fi

echo 2: Testing Woz1 to NIB to DSK
./wozzle -s -i /tmp/out.nib -o /tmp/out.dsk
SUM=`md5sum /tmp/out.dsk|cut -f1 -d' '`
if [ "$SUM" != "$DSKMD5" ]; then
    echo Woz1 to NIB to DSK conversion failed [$SUM]
    echo Check to see if this image boots, and update test 2 if so
    runcmd /tmp/out.dsk
    exit 1
fi

echo 3: Testing Woz1 to NIB to DSK to Woz2
./wozzle -s -i /tmp/out.dsk -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "$WOZ2DSKMD5" ]; then
    echo Woz1 to NIB to DSK to Woz2 conversion failed [$SUM]
    echo Check to see if this image boots, and update test 3 if so
    runcmd /tmp/out.woz
    exit 1
fi

echo 4: Testing Woz1 to DSK
./wozzle -s -i woz\ test\ images/WOZ\ 1.0/DOS\ 3.3\ System\ Master.woz -o /tmp/out.dsk
SUM=`md5sum /tmp/out.dsk|cut -f1 -d' '`
if [ "$SUM" != "$DSKMD5" ]; then
    echo Woz1 to DSK conversion failed [$SUM]
    echo Check to see if this image boots, and update test 4 if so
    runcmd /tmp/out.dsk
    exit 1
fi

echo 5: Testing Woz1 to DSK to NIB
./wozzle -s -i /tmp/out.dsk -o /tmp/out.nib
SUM=`md5sum /tmp/out.nib|cut -f1 -d' '`
if [ "$SUM" != "$NIBMD5" ]; then
    echo Woz1 to DSK to NIB conversion failed [$SUM]
    echo Check to see if this image boots, and update test 5 if so
    runcmd /tmp/out.nib
    exit 1
fi

echo 6: Testing Woz1 to DSK to NIB to Woz2
./wozzle -s -i /tmp/out.nib -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "$WOZ2NIBMD5" ]; then
    echo Woz1 to DSK to NIB to Woz2 conversion failed [$SUM]
    echo Check to see if this image boots, and update test 6 if so
    runcmd /tmp/out.woz
    exit 1
fi

echo 7: Testing Woz2 to NIB
./wozzle -s -i woz\ test\ images/WOZ\ 2.0/DOS\ 3.3\ System\ Master.woz -o /tmp/out.nib
SUM=`md5sum /tmp/out.nib|cut -f1 -d' '`
if [ "$SUM" != "$NIBMD5" ]; then
    echo Woz2 to NIB conversion failed [$SUM]
    echo Check to see if this image boots, and update test 7 if so
    runcmd /tmp/out.nib
    exit 1
fi

echo 8: Testing Woz2 to NIB to DSK
./wozzle -s -i /tmp/out.nib -o /tmp/out.dsk
SUM=`md5sum /tmp/out.dsk|cut -f1 -d' '`
if [ "$SUM" != "$DSKMD5" ]; then
    echo Woz2 to NIB to DSK conversion failed [$SUM]
    echo Check to see if this image boots, and update test 8 if so
    runcmd /tmp/out.dsk
    exit 1
fi

echo 9: Testing Woz2 to NIB to DSK to Woz2
./wozzle -s -i /tmp/out.dsk -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "1ed48d059865e922f981a51df1f71ff3" ]; then
    echo Woz2 to NIB to DSK to Woz2 conversion failed [$SUM]
    echo Check to see if this image boots, and update test 9 if so
    runcmd /tmp/out.woz
    exit 1
fi

echo 10: Testing Woz2 to DSK
./wozzle -s -i woz\ test\ images/WOZ\ 2.0/DOS\ 3.3\ System\ Master.woz -o /tmp/out.dsk
SUM=`md5sum /tmp/out.dsk|cut -f1 -d' '`
if [ "$SUM" != "$DSKMD5" ]; then
    echo Woz2 to DSK conversion failed [$SUM]
    echo Check to see if this image boots, and update test 10 if so
    runcmd /tmp/out.dsk
    exit 1
fi

echo 11: Testing Woz2 to DSK to NIB
./wozzle -s -i /tmp/out.dsk -o /tmp/out.nib
SUM=`md5sum /tmp/out.nib|cut -f1 -d' '`
if [ "$SUM" != "$NIBMD5" ]; then
    echo Woz2 to DSK to NIB conversion failed [$SUM]
    echo Check to see if this image boots, and update test 11 if so
    runcmd /tmp/out.nib
    exit 1
fi

echo 12: Testing Woz2 to DSK to NIB to Woz2
./wozzle -s -i /tmp/out.nib -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "$WOZ2NIBMD5" ]; then
    echo Woz2 to DSK to NIB to Woz2 conversion failed [$SUM]
    echo Check to see if this image boots, and update test 12 if so
    runcmd /tmp/out.woz
    exit 1
fi

echo 13: Testing Woz1 to Woz2
./wozzle -s -i woz\ test\ images/WOZ\ 1.0/DOS\ 3.3\ System\ Master.woz -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "cc5f5df2afec5de51955cbd4e7f1d6d5" ]; then
    echo Woz1 to Woz2 conversion failed [$SUM]
    echo Check to see if this image boots, and update test 13 if so
    runcmd "/tmp/out.woz"
    exit 1
fi

echo 14: Testing Woz2 to Woz2
./wozzle -s -i woz\ test\ images/WOZ\ 2.0/DOS\ 3.3\ System\ Master.woz -o /tmp/out.woz
SUM=`md5sum /tmp/out.woz|cut -f1 -d' '`
if [ "$SUM" != "3671582d4b0a62c31b78e2a4e047161a" ]; then
    echo Woz1 to Woz2 conversion failed [$SUM]
    echo Check to see if this image boots, and update test 13 if so
    runcmd "/tmp/out.woz"
    exit 1
fi


exit 0
