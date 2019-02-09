Wozzle v1.0
===========

©2019 Jorj Bauer. MIT license.

Wozzle is a Woz disk image swizzler - a utility to swish around the bits to make them conform to some other standard.

Wozzle converts between Woz 1.0/1.1, Woz 2.0, DSK, and NIB formats.

Building
--------

*$ make*

Wozzle is written in C++, and expects to find g++ for compilation.

Usage
-----

*$ wozzle -h*
```
Usage: ./wozzle { -I <input file> | { -i <input file> -o <output file> } }

	-h			This help text
	-I [input filename]	Dump information about disk image
	-i [input filename]	Name of input disk image
	-o [output filename]	Name of output (WOZ2) disk image
```

Wozzle has two modes of operation. Its primary use is converting disk images from one disk format to another, like so:

*$ wozzle -i input.dsk -o output.woz*

It's possible to convert between _.dsk_ (including _.po_ or _.do_), _.nib_, and _.woz_ formats. Yes, that means you can use this to convert from a DSK to a NIB, if that's what you need...

*$ wozzle -i input.dsk -o output.nib*

Its other mode of operation is to tell you information about a Woz image. (Using this on a non-woz image will implicitly convert it to a woz image in memory, and then give you information as if it were a woz 2.0 image.)

*$ wozzle -I dos33.woz*
```
WOZ image version 2
Disk type: 5.25"
Write protected: yes
Synchronized: no
Cleaned: yes
Creator: Applesauce v1.1                 
Disk sides: 1
Boot sector format: 16 sector
Optimal bit timing: 4000 ns
Hardware compatability flags: 0x0
Required RAM: 0 K
Largest track: 6656 bytes

Quarter-track map:
 0       0 =>   0       1 =>   0       2 => 255       3 =>   1
 1       4 =>   1       5 =>   1       6 => 255       7 =>   2
...
33     132 =>  33     133 =>  33     134 => 255     135 =>  34
34     136 =>  34     137 =>  34     138 => 255     139 => 255
Track 0:
  Starts at block 3
  Number of blocks: 13
  Number of bits: 50304
    Raw track data:
    0x0000 : FF 3F CF F3 FC FF 3F CF F3 FC FF 3F CF F3 FC FF
    0x0010 : 3F CF F3 FC D5 AA 96 FF FE AA AA AA AA FF FE DE
...
```

and so on.

Limitations
-----------

* Only 5.25" WOZ images are supported.
* No support for writing Woz 1.x images; all Woz writes are 2.0.
* WRIT data is unsupported.
* The code still suffers from early development messiness.
* The -I flag has three different compilable behaviors with respect to raw track data, but there's no way built in for users to select one or another.
* The tests are fairly fragile; they expect to find the woz project's test disks and perform conversions on the DOS 3.3 system master.
