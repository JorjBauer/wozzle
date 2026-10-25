// The wozzle library in JavaScript, by way of WebAssembly.
//
//   import { loadWozzle } from './wozzle.mjs';
//   const woz = await loadWozzle();
//   const img = woz.open(bytes, 'disk.po');      // a Uint8Array and its name
//   for (const e of img.list()) console.log(e.name, e.length);
//   const data = img.read('PRODOS');
//   img.write('HELLO', bytes, { fileType: 0x06, auxType: 0x2000 });
//   const out = img.save('disk.woz');            // the format follows the name
//   img.close();
//
// Images live in Emscripten's in-memory filesystem while they're open, so
// nothing here touches real files: bytes go in, bytes come out. The name
// matters only for its extension, which picks the image format (.woz,
// .dsk/.do, .po, .nib, .hdv/.img).
//
// A call that fails throws a WozError carrying the library's reason. The
// messages a call produced (warnings, say) are in woz.messages afterward.
//
// The library is single-threaded, so keep one Wozzle to a thread. To keep
// it off a page's main thread, run it in a worker with wozzle-worker.mjs
// and talk to it with wozzle-remote.mjs, which has this same API with
// every call returning a promise.

import createWozModule from './build/wozcore.mjs';
import { WozError } from './wozerror.mjs';

export { WozError };

export const FS_UNKNOWN = 0;
export const FS_DOS33 = 1;
export const FS_PRODOS = 2;
export const FS_PASCAL = 3;

const fsNames = { 1: 'dos33', 2: 'prodos', 3: 'pascal' };
const fsCodes = { dos33: FS_DOS33, prodos: FS_PRODOS, pascal: FS_PASCAL };

export const MSG_DEBUG = 0;
export const MSG_INFO = 1;
export const MSG_WARNING = 2;
export const MSG_ERROR = 3;

function fsCode(t) {
  if (t === undefined || t === null) return FS_UNKNOWN;
  if (typeof t === 'number') return t;
  if (!(t in fsCodes)) throw new TypeError(`unknown filesystem '${t}'`);
  return fsCodes[t];
}

function baseName(name) {
  return String(name).split(/[\\/]/).pop() || 'image';
}

// A date from the library: seconds since 1970, or -1 for none
function toDate(secs) {
  return secs < 0 ? null : new Date(secs * 1000);
}

function entryFromJson(e) {
  e.created = toDate(e.created);
  e.modified = toDate(e.modified);
  return e;
}

export async function loadWozzle(moduleOptions = {}) {
  const mod = await createWozModule(moduleOptions);
  return new Wozzle(mod);
}

export class Wozzle {
  constructor(mod) {
    this.mod = mod;
    this.messages = [];
    this.nextId = 1;
    mod.wozOnMessage = (level, text) => this.messages.push({ level, text });
    mod.ccall('wozJsInit', null, [], []);
    mod.FS.mkdir('/woz');
  }

  // ---- plumbing ----

  begin() {
    this.messages = [];
    this.mod.ccall('wozClearError', null, [], []);
  }

  fail() {
    const why = this.mod.ccall('wozLastError', 'string', [], []);
    throw new WozError(why, this.messages);
  }

  // A fresh MEMFS directory for one image, so names never collide
  scratchDir() {
    const dir = `/woz/${this.nextId++}`;
    this.mod.FS.mkdir(dir);
    return dir;
  }

  removeTree(dir) {
    const FS = this.mod.FS;
    for (const f of FS.readdir(dir)) {
      if (f !== '.' && f !== '..') FS.unlink(`${dir}/${f}`);
    }
    FS.rmdir(dir);
  }

  // A C string the library handed back (JSON, here), freed after reading
  takeString(ptr) {
    if (!ptr) this.fail();
    const s = this.mod.UTF8ToString(ptr);
    this.mod._wozFree(ptr);
    return s;
  }

  // Call a function that fills a uint8_t ** and a uint32_t *, and return
  // the bytes as a Uint8Array of our own
  takeBuffer(call) {
    const mod = this.mod;
    const outs = mod._wozMalloc(8);
    mod.HEAPU32[outs >> 2] = 0;
    mod.HEAPU32[(outs >> 2) + 1] = 0;
    try {
      const ok = call(outs, outs + 4);
      const ptr = mod.HEAPU32[outs >> 2];
      const len = mod.HEAPU32[(outs >> 2) + 1];
      const bytes = ptr ? mod.HEAPU8.slice(ptr, ptr + len) : new Uint8Array(0);
      if (ptr) mod._wozFree(ptr);
      return { ok, bytes };
    } finally {
      mod._wozFree(outs);
    }
  }

  // Copy bytes into the heap for the length of `use`
  withBytes(bytes, use) {
    const mod = this.mod;
    const ptr = mod._wozMalloc(bytes.length || 1);
    mod.HEAPU8.set(bytes, ptr);
    try {
      return use(ptr, bytes.length);
    } finally {
      mod._wozFree(ptr);
    }
  }

  // ---- images ----

  // Which filesystem an image holds: 'dos33', 'prodos', 'pascal' or null
  detect(bytes, name) {
    const dir = this.scratchDir();
    const path = `${dir}/${baseName(name)}`;
    try {
      this.mod.FS.writeFile(path, bytes);
      const t = this.mod.ccall('wozImageDetect', 'number', ['string'], [path]);
      return fsNames[t] || null;
    } finally {
      this.removeTree(dir);
    }
  }

  // Open an image. fsType ('dos33', 'prodos', 'pascal') forces one;
  // without it, the filesystem is worked out from the image.
  open(bytes, name, fsType) {
    this.begin();
    const dir = this.scratchDir();
    const path = `${dir}/${baseName(name)}`;
    this.mod.FS.writeFile(path, bytes);
    const handle = this.mod.ccall('wozImageOpen', 'number', ['string', 'number'],
                                  [path, fsCode(fsType)]);
    if (!handle) {
      this.removeTree(dir);
      this.fail();
    }
    return new WozImage(this, handle, dir, path);
  }

  // A blank image, as bytes. name's extension picks the format; sizes
  // other than a floppy's need .hdv or .img.
  createBlank({ name, fsType, volumeName = null, dosVolume = 254,
                blocks = 280, bootable = false }) {
    this.begin();
    const dir = this.scratchDir();
    const path = `${dir}/${baseName(name)}`;
    try {
      const ok = this.mod.ccall('wozCreateBlankImage', 'boolean',
        ['string', 'number', 'string', 'number', 'number', 'boolean'],
        [path, fsCode(fsType), volumeName, dosVolume, blocks, bootable]);
      if (!ok) this.fail();
      return this.mod.FS.readFile(path);
    } finally {
      this.removeTree(dir);
    }
  }

  // Convert an image between formats (by the names' extensions). This
  // works below the filesystem, so copy-protected disks convert too.
  convert(bytes, fromName, toName) {
    this.begin();
    const dir = this.scratchDir();
    const inPath = `${dir}/in-${baseName(fromName)}`;
    const outPath = `${dir}/out-${baseName(toName)}`;
    try {
      this.mod.FS.writeFile(inPath, bytes);
      const ok = this.mod.ccall('wozConvertImage', 'boolean',
                                ['string', 'string'], [inPath, outPath]);
      if (!ok) this.fail();
      return this.mod.FS.readFile(outPath);
    } finally {
      this.removeTree(dir);
    }
  }

  // "140k", "800k", "32m" or a block count, as blocks (-1 if it isn't one)
  parseSize(token) {
    return this.mod.ccall('wozParseSize', 'number', ['string'], [String(token)]);
  }

  // Copy files between two open images of the same filesystem, as
  // wozmosis does. paths: files or ProDOS subtrees (all of it if empty).
  // destDir: ProDOS, put it all under this directory. Throws if anything
  // collides (unless force) or fails; dst changes only in memory until
  // it's saved.
  copy(src, dst, { paths = [], destDir = null, force = false } = {}) {
    this.begin();
    const mod = this.mod;
    const strs = paths.map((p) => mod.stringToNewUTF8(String(p)));
    const array = mod._wozMalloc(4 * (strs.length || 1));
    strs.forEach((p, i) => { mod.HEAPU32[(array >> 2) + i] = p; });
    try {
      const json = this.takeString(mod.ccall('wozJsCopy', 'number',
        ['number', 'number', 'number', 'number', 'string', 'boolean'],
        [src.handle, dst.handle, array, strs.length, destDir, force]));
      const stats = JSON.parse(json);
      if (!stats.ok) {
        const err = new WozError(mod.ccall('wozLastError', 'string', [], []),
                                 this.messages);
        err.stats = stats;
        throw err;
      }
      delete stats.ok;
      return stats;
    } finally {
      strs.forEach((p) => mod._free(p));
      mod._wozFree(array);
    }
  }
}

export class WozImage {
  constructor(woz, handle, dir, path) {
    this.woz = woz;
    this.handle = handle;
    this.dir = dir;
    this.path = path;
    this.fsType = fsNames[woz.mod.ccall('wozImageFsType', 'number',
                                        ['number'], [handle])] || null;
  }

  call(name, ret, types, args) {
    if (!this.handle) throw new WozError('the image is closed', []);
    this.woz.begin();
    return this.woz.mod.ccall(name, ret, ['number', ...types],
                              [this.handle, ...args]);
  }

  check(ok) {
    if (!ok) this.woz.fail();
  }

  // Whether it really looks like the filesystem it was opened as
  probe() {
    return this.call('wozImageProbe', 'boolean', [], []);
  }

  isDirty() {
    return this.call('wozImageIsDirty', 'boolean', [], []);
  }

  volumeInfo() {
    const ptr = this.call('wozJsVolumeInfo', 'number', [], []);
    const v = JSON.parse(this.woz.takeString(ptr));
    v.fsType = fsNames[v.fsType] || null;
    return v;
  }

  // The entries of a directory ('' or omitted for the root)
  list(dir = '') {
    const ptr = this.call('wozJsList', 'number', ['string'], [dir]);
    return JSON.parse(this.woz.takeString(ptr)).map(entryFromJson);
  }

  stat(path) {
    const ptr = this.call('wozJsStat', 'number', ['string'], [path]);
    return entryFromJson(JSON.parse(this.woz.takeString(ptr)));
  }

  // A file's contents. resourceFork: an extended ProDOS file's other fork.
  // raw: everything allocated, as it lies on disk (DOS 3.3 headers too).
  read(path, { resourceFork = false, raw = false } = {}) {
    const { ok, bytes } = this.woz.takeBuffer((data, len) =>
      this.call('wozImageReadFile', 'boolean',
                ['string', 'boolean', 'boolean', 'number', 'number'],
                [path, resourceFork, raw, data, len]));
    this.check(ok);
    return bytes;
  }

  // A BASIC program as text, the way LIST shows it
  listBasic(path) {
    const { ok, bytes } = this.woz.takeBuffer((text, len) =>
      this.call('wozImageListBasic', 'boolean', ['string', 'number', 'number'],
                [path, text, len]));
    this.check(ok);
    return new TextDecoder('latin1').decode(bytes);
  }

  // Write a file, replacing one of the same name. fileType and auxType are
  // ProDOS values; left out, they come from AppleSingle metadata or the
  // name, else BIN at $2000. tokenize: bytes are an Applesoft listing.
  write(path, bytes, { fileType = -1, auxType = -1, tokenize = false } = {}) {
    this.woz.withBytes(bytes, (ptr, len) => {
      this.check(this.call('wozImageWriteFile', 'boolean',
        ['string', 'number', 'number', 'number', 'number', 'boolean'],
        [path, ptr, len, fileType, auxType, tokenize]));
    });
  }

  delete(path, { recursive = false } = {}) {
    this.check(this.call('wozImageDelete', 'boolean', ['string', 'boolean'],
                         [path, recursive]));
  }

  removeDirectory(path) {
    this.check(this.call('wozImageRemoveDirectory', 'boolean', ['string'],
                         [path]));
  }

  makeDirectory(path) {
    this.check(this.call('wozImageMakeDirectory', 'boolean', ['string'],
                         [path]));
  }

  renameVolume(name) {
    this.check(this.call('wozImageRenameVolume', 'boolean', ['string'],
                         [name]));
  }

  // Boot code: the embedded blocks, or a donor image's (bytes and name)
  installBootBlocks(donorBytes = null, donorName = 'donor.po') {
    if (!donorBytes) {
      this.check(this.call('wozImageInstallBootBlocks', 'boolean', ['string'],
                           [null]));
      return;
    }
    const path = `${this.dir}/donor-${baseName(donorName)}`;
    this.woz.mod.FS.writeFile(path, donorBytes);
    try {
      this.check(this.call('wozImageInstallBootBlocks', 'boolean', ['string'],
                           [path]));
    } finally {
      this.woz.mod.FS.unlink(path);
    }
  }

  // Pascal: gather the free space at the end, or right after afterFile
  krunch(afterFile = null) {
    this.check(this.call('wozImageKrunch', 'boolean', ['string'],
                         [afterFile]));
  }

  // ProDOS: shrink a hard-disk volume to `blocks`
  resize(blocks, { relocate = true } = {}) {
    this.check(this.call('wozImageResize', 'boolean', ['number', 'boolean'],
                         [blocks, relocate]));
  }

  // The image as bytes. A name saves in the format its extension picks;
  // without one, it's the format it was opened in.
  save(name) {
    const path = name ? `${this.dir}/save-${baseName(name)}` : this.path;
    this.check(this.call('wozImageSave', 'boolean', ['string'], [path]));
    const bytes = this.woz.mod.FS.readFile(path);
    if (name) this.woz.mod.FS.unlink(path);
    return bytes;
  }

  close() {
    if (!this.handle) return;
    this.woz.mod.ccall('wozImageClose', null, ['number'], [this.handle]);
    this.handle = 0;
    this.woz.removeTree(this.dir);
  }
}
