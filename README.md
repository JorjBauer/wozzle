Wozzle v1.1-alpha
===========

©2019 Jorj Bauer. MIT license.

Wozzle is a Woz disk image swizzler - a utility to swish around the bits to make them conform to some other standard.

Wozzle converts between Woz 1.0/1.1, Woz 2.0, DSK, and NIB formats.

Building
--------

*$ make*

Wozzle is written in C++, and expects to find g++ for compilation.

Using the library in another program
------------------------------------

Everything but the command line front ends lives in src/. That's the disk image code (woz, nibutil, crc32, wozmsg, wozhost), the DOS 3.3, ProDOS and Pascal filesystems (the *spector classes and vent), the BASIC listers and tokenizer, and the operations the tools are built from (wozops: format, cpin, cpout, boot blocks, and wozmosis's image-to-image copy). The tools here link it as libwozcore.a, and I also use it in my Aiie emulator, on both the Mac and a Teensy 4.1. That's why it's laid out the way it is.

There are two ways in. C++ programs can use the classes and wozops.h directly. Anything else (a GUI in another language, a WebAssembly build) can use wozlib.h, which is plain C: open an image, list a directory, read and write files, make a blank image, copy between images. tests/unit-capi.c is written in C and runs through most of it, if you want an example.

If you're building with make, include wozcore.mk and use its variables instead of naming the files, so you don't have to chase my file list:

```
include wozzle/wozcore.mk
CXXFLAGS += $(WOZCORE_CFLAGS)
myprogram: $(MYOBJS) $(WOZCORE_OBJS)
```

If you're building with Arduino, the top of this repository is an Arduino library (library.properties plus src/). Hand it to arduino-cli with `--library path/to/wozzle`.

The library never touches files, memory or the terminal directly. File I/O and allocation go through hooks in wozhost.h, and messages go through wozmsg.h. On a POSIX system the defaults just work. Arduino has no POSIX file layer, so there you have to install file hooks (wozSetHost) before you open an image. wozhost.h also has two settings for slow storage: keepFilesOpen, and skipChecksum (which writes a 0 CRC, which the WOZ spec allows). There's a clock hook too (now), for dating new Pascal files; on Arduino it defaults to "don't know", and they get no date.

There's also a WebAssembly build, which is what the GUI is going to use. *make wasm* builds it with Emscripten (I've used 6.0.5) into wasm/build/, and wasm/wozzle.mjs wraps it in JavaScript that deals in byte arrays:

```
import { loadWozzle } from './wasm/wozzle.mjs';
const woz = await loadWozzle();
const img = woz.open(bytes, 'disk.po');
for (const e of img.list()) console.log(e.name, e.length);
const woz2 = img.save('disk.woz');
```

Images sit in Emscripten's in-memory filesystem while they're open, so the library's default file hooks work unchanged and nothing touches real files. wasm/test.mjs runs through most of it under node, and the test suite runs that if the build is there.

Buffers the library hands back (file contents, for instance) come from the allocator hook, so give them back with wozFree, not free. On a POSIX system with the default hooks it's the same thing, but it isn't on a Teensy with its allocator pointed at PSRAM.

None of this is thread safe. The hooks and the message sink are global, and nothing is locked. If you're calling it from a GUI, do all of it from one thread.

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
* Only supports 16-sector disks.


Wozit v1.0
===========

©2021 Jorj Bauer. MIT license.

Wozit ("What's it?") is a disk inspection/modification tool. It's
built from the same make command as Wozzle.

Usage
-----

*$ wozzle -h*
```
Usage: ./wozit -I <input image> { -d | -p | -P }

  -I [input filename]     input disk image to inspect/modify
  -d                      DOS 3.3 mode
  -p                      ProDOS mode
  -P                      UCSD p-System / Apple Pascal mode
```

In Pascal mode wozit reads and writes UCSD p-System / Apple Pascal
volumes (140K floppies and ProDOS-order .po/.woz images). Files are
stored on contiguous blocks, so a `cpin` may fail if no single free
region is large enough even when the total free space would fit. Two
commands defragment the volume (like the Pascal Filer's "K)runch"):

* *krunch* - slides every file toward the front so the free space
  becomes one contiguous region at the end of the volume.
* *krunchafter &lt;file&gt;* - puts the free space immediately after the
  named file instead (files up to and including it pack toward the front;
  the rest pack toward the end). Handy to leave room after a file you
  expect to grow.

Both refuse to run on a volume whose directory is damaged/overlapping or
that contains a .BAD (bad-block) file, and both need a `save` afterward.

`wozmosis -P` copies Pascal files between two Pascal images, preserving
each file's type and modification date.

`wozmosis -p -D <dir>` puts the whole copy under a directory on the
destination, creating it (and any missing parents) if it isn't there: the
source's `FOO/BAR` lands at `<dir>/FOO/BAR`. Besides filing a disk's worth
of software away in one place, this is the way past a full volume
directory. A ProDOS root holds a fixed 51 entries and cannot grow, so a
copy that overflows it still fits under a subdirectory, which can. `-D`
needs `-p`; neither DOS 3.3 nor Pascal has directories.

Wozit is an interactive disk inspection tool. Once started, it expects one of these commands:

Arguments are split at spaces, the way a shell does it, so a name with a
space in it (DOS 3.3 allows them) needs quoting: *cpout "BAS BIG" out.bin*.
Single quotes, double quotes and a backslash all work. A command's last
argument is still the rest of the line if you don't quote any of it, so
*cat BAS BIG* and *cpout FOO my file.bin* do what they look like. Inside a
*-c*, that means quotes within quotes: *-c 'cpout "BAS BIG" out.bin'*.

*ls*

Lists the directory of the disk image.

*cat <filename>*

Dumps the contents of the given file to stdout. Note that this is affected by the state of 'strip'.

*strip <on|off>*

Turns on or off high-bit stripping when using *cat* on a file. Useful for inspecting TXT files.

*cpout [-r|-B] <inputname> <outputname>*

Copies a file from inside the image to a file outside the image. With
*-r*, it copies the resource fork of a GS/OS extended file instead of its
data fork. With *-B*, it de-tokenizes an Applesoft or Integer BASIC
program into a text listing instead of copying the raw bytes.

What comes out is the file's contents, and that means the same thing on
every filesystem: what *cpin* put in comes back out. DOS 3.3 stores a
header in front of I, A and B files (the length, and for B the load
address before it); that isn't part of the contents, so it's left off,
and the header's length says where the file ends. A DOS 3.3 T file ends
at its first $00, the way a sequential text file does.

Start wozit with *-A* and *cpout* copies everything the file has
allocated instead, read as it lies on disk: DOS 3.3 headers included, and
past whatever length the directory or the header gives. That's the way to
recover a file whose length is a stub hiding a bigger payload, and the way
to get at a random-access text file, which can have $00s anywhere. Sparse
sectors come out as zeros, so the records stay where they were written.

GS/OS extended (forked) files, ProDOS storage type 5, are supported in
ProDOS mode: *ls* marks them with a `+` after the name, *inspect* reports
the shape and size of each fork, and *rm* frees both forks along with the
extended key block. *cpin* creates one when the host file is an
AppleSingle file carrying a resource fork, taking the ProDOS type and aux
type from the AppleSingle metadata. `wozmosis -p` copies extended files
between images with both forks and their GS/OS Finder information intact.

*cpin [-B] <inputname> <outputname> [type [aux]]*

Copies a host file into the image. *type* and *aux* are ProDOS values in
hex; without them the type comes from AppleSingle metadata if the host
file has any, then from a *.SYSTEM* name heuristic, then defaults to
BIN/$2000.

With *-B*, the host file is read as an Applesoft text listing and
tokenized on the way in, producing a BAS file that loads and runs on a
real Apple II. This works in both DOS 3.3 and ProDOS mode; the 2-byte
length header a DOS 3.3 'A' file carries is added by the filesystem layer.
Line numbers must be in ascending order with no duplicates, and no larger
than 63999; anything else is an error naming the offending source line.
Blank lines and lines starting with `#` are ignored, so a listing can
carry comments that aren't part of the program.

Tokenizing follows the ROM's own parser: whitespace outside quotes is
discarded, the longest matching keyword wins (so `ONERR` beats `ON`),
keywords may be split by spaces (which is the only reason `PR #3` works),
and the text after *REM* - or after *DATA*, up to the next unquoted colon
- is left alone rather than having keywords substituted into it. `?` is
accepted as shorthand for *PRINT*. Because whitespace is discarded,
*cpout -B* followed by *cpin -B* is a fixed point: a program can make the
round trip repeatedly without drifting.

Integer BASIC can be listed with *cpout -B* but not yet tokenized with
*cpin -B*: its tokens are context-dependent (three different *PRINT*
tokens, a dozen different commas), so writing them means reimplementing
the interpreter's grammar rather than looking keywords up in a table.

*list <filename>*

De-tokenize and show the given Applesoft or Integer BASIC program.

*resize [-n] <blocks>*

Shrinks a ProDOS volume in place - the volume directory header, the volume
bitmap, and the image file itself - so an oversized image can be cut down
to the size a device or partition actually offers, without formatting a
new volume and copying everything across. The size is a block count or one
of the *format* presets (140k, 800k, 4m, 5m, 8m, 10m, 16m, 32m).

Anything still allocated at or past the new end is moved down into free
space below it: the blocks are copied and every pointer that named them is
corrected - directory entries' key pointers, sapling and tree index
blocks, the mini-entries of an extended file's key block, directory chain
links, subdirectory parent pointers, and each entry's header pointer. Free
space is counted first, so if the volume's contents simply won't fit in
the smaller size it says how many more blocks would be needed and changes
nothing. A block marked allocated that no file or directory claims is
released, with a warning. *-n* skips relocation entirely: the resize is
then refused whenever the tail is occupied, which is the way to ask
whether a volume can be shrunk without rewriting anything.

Bitmap blocks the smaller volume no longer needs become free space. Raw
block images only (.img/.hdv, or a .po larger than a floppy), and it only
shrinks: growing a volume still means formatting a bigger image and
copying into it. Takes effect on *save*.


Limitations
-----------

* All of the limitations of Wozzle
* Only supports normal RWTS disks
* There's no way to differentiate between files of different types with the same name
* In ProDOS mode, it only references files by filename - not by directory. So two files
  with the same name in different directories won't be readable (it will pick the first
  one it finds).
  


Wozfuse
=======

Wozfuse mounts a disk image as a read-only filesystem, so you can browse
it and copy files out with whatever you normally use (Finder, Explorer, a
shell).

```
wozfuse [-d | -p | -P] [-c] [-v] <image> <mountpoint>
```

With none of *-d*, *-p* or *-P*, it works out the filesystem from the
image. It runs in the foreground; Ctrl-C or *umount* unmounts it.

Files read as their contents, the same bytes *cpout* copies out. The
Apple II file type, aux type, and (on ProDOS) access bits come along as
extended attributes: *user.apple2.filetype*, *user.apple2.auxtype* and
*user.apple2.access*, as hex. On a Mac, *xattr -l* shows them; on Linux,
*getfattr -d*. With *-c*, every directory also gets a generated
*_CATALOG.TXT* that lists them, for anywhere attributes are awkward to
get at.

DOS 3.3 names can hold characters a host filename can't (a '/', say), so
those turn into %XX: *A/B* shows up as *A%2FB*.

It's built on FUSE 3, and make only builds it when pkg-config can find
fuse3. As of this writing I've run it with macFUSE 5.1.2 on macOS 26.
Linux's libfuse3 uses the same interface. On Windows it would be WinFsp's
FUSE layer, which I haven't tried; the code has a few lines for WinFsp's
type names, but that's as far as it's been.

Limitations
-----------

* Read-only. Writing is planned.
* A GS/OS extended file shows only its data fork.
