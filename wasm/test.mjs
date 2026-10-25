// Tests for the WebAssembly build, through wozzle.mjs the way the GUI will
// use it. Every image it uses it makes itself, except for an optional
// real disk to read:
//
//   node wasm/test.mjs [ProDOS_2_4.po [PRODOS-as-cpout-copies-it]]

import { readFileSync } from 'node:fs';
import { loadWozzle, WozError } from './wozzle.mjs';

let passed = 0;
let failed = 0;

function check(ok, name) {
  process.stderr.write(`  ${ok ? 'PASS' : 'FAIL'}: ${name}\n`);
  if (ok) passed++; else failed++;
}

function throws(fn, needle, name) {
  try {
    fn();
    check(false, name);
  } catch (e) {
    check(e instanceof WozError && e.message.includes(needle), name);
  }
}

const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const bytesOf = (s) => new TextEncoder().encode(s);

const woz = await loadWozzle();

// ---- blank images ----
check(woz.parseSize('800k') === 1600, '800k is 1600 blocks');
let po = woz.createBlank({ name: 'new.po', fsType: 'prodos', volumeName: 'wasm' });
check(po.length === 280 * 512, 'a blank ProDOS floppy is 140K');
check(woz.detect(po, 'new.po') === 'prodos', 'and detects as ProDOS');
throws(() => woz.createBlank({ name: 'x.po', fsType: 'prodos', volumeName: 'bad name!' }),
       'bad volume name', 'a bad volume name is refused, with the reason');
check(woz.createBlank({ name: 'x.po', fsType: 'prodos', blocks: 1600 }).length === 1600 * 512,
      'an 800K .po is fine');
throws(() => woz.createBlank({ name: 'x.po', fsType: 'prodos', blocks: 100 }),
       '.hdv or .img', 'a .po smaller than a floppy is refused');

// ---- ProDOS ----
let img = woz.open(po, 'new.po');
check(img.fsType === 'prodos', 'the image opens as ProDOS');
let vi = img.volumeInfo();
check(vi.volumeName === 'WASM' && vi.totalUnits === 280 && vi.freeUnits === 273,
      'WASM, 280 blocks, 273 free');

img.makeDirectory('SUB');
const bin = new Uint8Array(1000).map((_, i) => i & 0xff);
img.write('SUB/DATA', bin, { fileType: 0x06, auxType: 0x0800 });
img.write('HELLO', bytesOf('10 PRINT "HI"\n20 GOTO 10\n'), { tokenize: true });
check(img.isDirty(), 'writing makes it dirty');

let root = img.list();
check(root.length === 2, 'the root lists two entries');
check(root.some((e) => e.name === 'SUB' && e.isDirectory), 'SUB is a directory');
const hello = root.find((e) => e.name === 'HELLO');
check(hello && hello.fileType === 0xfc && hello.auxType === 0x0801,
      'HELLO is BAS at $0801');
// ProDOS dates go to the minute, so allow for the clock ticking over
const recent = (d) => d instanceof Date && Math.abs(Date.now() - d.getTime()) < 120000;
check(hello && recent(hello.created) && recent(hello.modified), 'dated now');
check(recent(root.find((e) => e.name === 'SUB')?.created), 'and so is SUB');
const sub = img.list('SUB');
check(sub.length === 1 && sub[0].length === 1000 && sub[0].auxType === 0x0800,
      'SUB/DATA has its length and aux type');
check(same(img.read('SUB/DATA'), bin), 'SUB/DATA reads back byte for byte');
check(img.listBasic('HELLO') === '10  PRINT "HI"\n20  GOTO 10\n', 'HELLO lists');
throws(() => img.read('NOPE'), 'not found', 'a missing file throws');
throws(() => img.list('NOPE'), 'not found', 'so does a missing directory');

img.renameVolume('renamed');
check(img.volumeInfo().volumeName === 'RENAMED', 'the volume renames');
img.installBootBlocks();

// Save as WOZ, reopen, and the files are still there
const asWoz = img.save('new.woz');
check(String.fromCharCode(...asWoz.slice(0, 4)) === 'WOZ2', 'it saves as WOZ 2');
const asPo = img.save();
check(asPo.length === 280 * 512, 'and as itself');
img.close();
img = woz.open(asWoz, 'new.woz');
check(same(img.read('SUB/DATA'), bin), 'the WOZ copy reads back');
img.delete('SUB', { recursive: true });
check(img.list().length === 1, 'a directory deletes recursively');
img.close();
throws(() => img.list(), 'closed', 'a closed image says so');

// ---- image to image ----
const src = woz.open(asPo, 'src.po');
const dst = woz.open(woz.createBlank({ name: 'dst.po', fsType: 'prodos',
                                       volumeName: 'DEST' }), 'dst.po');
let stats = woz.copy(src, dst);
check(stats.files === 2 && stats.dirsMade === 1, 'the whole volume copies');
try {
  woz.copy(src, dst);
  check(false, 'copying again collides');
} catch (e) {
  check(e instanceof WozError && e.stats.collisions === 2 &&
        e.message.includes('SUB/DATA'), 'copying again collides, by name');
}
stats = woz.copy(src, dst, { paths: ['HELLO'], destDir: 'STASH', force: true });
check(stats.files === 1 && dst.stat('STASH/HELLO').fileType === 0xfc,
      'one file copies under a new directory');
src.close();
dst.close();

// ---- DOS 3.3 ----
const dsk = woz.createBlank({ name: 'd.dsk', fsType: 'dos33', dosVolume: 7 });
img = woz.open(dsk, 'd.dsk');
vi = img.volumeInfo();
check(img.fsType === 'dos33' && vi.dosVolume === 7 && vi.unitSize === 256,
      'DOS 3.3, volume 7, 256-byte sectors');
img.write('MY FILE', bin.slice(0, 300), { fileType: 0x06, auxType: 0x0300 });
const e = img.stat('MY FILE');
check(e.length === 300 && e.auxType === 0x0300, 'a B file has its length and load address');
check(same(img.read('MY FILE'), bin.slice(0, 300)), 'and reads back as it went in');
const raw = img.read('MY FILE', { raw: true });
check(raw.length === 512 && raw[1] === 0x03 && raw[2] === 300 % 256,
      'the raw read has the header');
throws(() => img.makeDirectory('DIR'), 'no subdirectories', 'DOS 3.3 has no directories');
img.close();

// ---- Pascal, and a conversion ----
const pas = woz.createBlank({ name: 'p.po', fsType: 'pascal', volumeName: 'PAS' });
img = woz.open(pas, 'p.po');
check(img.fsType === 'pascal' && img.volumeInfo().volumeName === 'PAS', 'Pascal opens');
img.write('NOTES.TEXT', new Uint8Array(1024));
img.krunch();
check(img.list().length === 1, 'and holds a file');
img.close();

const nib = woz.convert(dsk, 'd.dsk', 'd.nib');
check(nib.length === 35 * 6656, 'a DSK converts to NIB');
check(same(woz.convert(nib, 'd.nib', 'back.dsk'), dsk), 'and back, byte for byte');

// ---- a full disk says so ----
{
  const full = woz.open(woz.createBlank({ name: 'full.po', fsType: 'prodos', volumeName: 'FULL' }), 'full.po');
  throws(() => full.write('BIG', new Uint8Array(200 * 1024)), 'the disk is full',
         'a file too big for the disk fails with "the disk is full"');
  full.close();
  const dos = woz.open(woz.createBlank({ name: 'full.dsk', fsType: 'dos33' }), 'full.dsk');
  throws(() => dos.write('BIG', new Uint8Array(200 * 1024), { fileType: 0x04 }),
         'the disk is full', 'and so does DOS 3.3');
  dos.close();
}

// ---- a real disk, if we were given one ----
if (process.argv[2]) {
  const real = readFileSync(process.argv[2]);
  img = woz.open(real, process.argv[2]);
  check(img.list().some((x) => x.name === 'PRODOS'), 'a real ProDOS disk lists');
  if (process.argv[3]) {
    check(same(img.read('PRODOS'), new Uint8Array(readFileSync(process.argv[3]))),
          'PRODOS reads the same as wozit cpout');
  }
  img.close();
}

process.stderr.write(`  ${passed} passed, ${failed} failed\n`);
process.exit(failed ? 1 : 0);
