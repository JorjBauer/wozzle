Wozzle GUI
==========

A desktop app for the wozzle tools, for people who'd rather not live on the
command line: open a disk image, look around it, pull files out, put files
in, and save it in whatever format you like.

It's Tauri v2 with a Svelte front end. The disk image work is the wozzle
library itself, compiled to WebAssembly (see ../wasm), so the app sees
exactly what wozit sees. The library runs in a Web Worker
(src/lib/woz.worker.ts), so a 32MB hard-disk image doesn't freeze the
window while it works. The same front end also runs as a plain web page,
where opening a file is a file picker and saving one is a download.

Building
--------

You need Node, Rust, and Emscripten (for the library). As of this writing
I've built it with Node 26, Rust 1.97, Emscripten 6.0.5 and Tauri 2.12 on
macOS 26.

```
npm install
npm run wasm          # make -C .. wasm: the library, as WebAssembly
npm run tauri dev     # the app, with the front end live-reloading
npm run tauri build   # a bundled app (and a .dmg), in build/target/release/bundle
npx tauri build --bundles app   # just the .app
```

*npm run dev* on its own serves the front end at http://localhost:5173, to
use as a web page.

From the top directory, *make dev* does the same as *npm run tauri dev*
(building the WebAssembly first if it needs to), and *make run* builds
the app and opens it.

For a release, *make app* or *make dist* in the top directory builds it
(see the top README's Releases section), and the finished Wozzle.app (and,
from *make dist*, its zip) lands in dist/. Everything built along the way
goes in build/: Vite's output in build/web, and Rust's in build/target
(src-tauri/.cargo/config.toml puts it there). The app's bundle
identifier is org.jorj.wozzle, and builds aren't signed yet, so a copy
that's been downloaded rather than built on the same Mac will need a
right-click, Open the first time.

What it does so far
-------------------

* Open a .woz, .dsk/.do, .po, .nib, .hdv, .img or .2mg image (DOS 3.3,
  ProDOS or Apple Pascal), and browse it, ProDOS subdirectories included
* Extract a file: its contents, everything allocated, or a BASIC program
  as a text listing
* View a file: a BASIC program as its listing, a text file as text, and
  anything else as a hex dump or a 65C02 disassembly, numbered from its
  load address. The disassembly is the same as aiie's debugger: its
  opcode table, //e addresses by name (F8ROM:COUT, KBD), and an Lxxxx
  label on each branch or jump target
* Volume and file details in a drawer on the right (Details turns it off)
* Add a file, with a file type and aux type, tokenizing an Applesoft
  listing on the way in
* New folder (ProDOS) and delete
* Make a blank image
* Save, and Save As in any format (which is how you convert one)
* Drag and drop: drop an image on the window to open it, drop files on the
  file list to add them (onto a folder's row to put them in that folder),
  and drag a file out of the list to the Finder to extract its contents.
  One file dropped gets the Add dialog; several go straight in, with names
  made to fit the disk ("my notes.txt" becomes MY.NOTES on ProDOS).
* Unsaved changes show in the window title, and opening another image,
  making a new one or quitting asks first

Dragging out is native under Tauri (tauri-plugin-drag), by way of a copy
in a temporary folder that's cleaned up when the app quits. In a browser,
only Chrome can drag a file out, and only once the file has been read,
which is usually done before the pointer has moved far enough to count
as a drag.

Not yet: copying between images (wozmosis), boot blocks, renaming the
volume, krunch and resize. The library and wasm/wozzle.mjs already do all
of those; they just don't have buttons.

Only one thing happens at a time: while the library is busy, the buttons
are off, and anything that takes longer than a quarter second says
"Working..." in the status line. There's no progress bar, because the
library doesn't report progress.
