// The page's end of a Wozzle running in a worker (wozzle-worker.mjs). It's
// the same API as wozzle.mjs, except that every call returns a promise:
//
//   import { connectWozzle } from './wozzle-remote.mjs';
//   const woz = connectWozzle(new Worker(workerUrl, { type: 'module' }));
//   const img = await woz.open(bytes, 'disk.po');
//   const files = await img.list();
//   const out = await img.save('disk.woz');
//   await img.close();
//
// Bytes going in are copied, so the caller's arrays stay usable. Bytes
// coming back are the caller's to keep. A failure rejects with a WozError,
// just as the library throws one, and woz.messages has what the last call
// said. Calls run in the order they're made.

import { WozError } from './wozerror.mjs';

export { WozError };

function listen(port, fn) {
  if (port.addEventListener) port.addEventListener('message', (e) => fn(e.data));
  else port.on('message', fn); // a Node Worker
}

// The worker's error, rebuilt as the same kind of error on this side
const standardErrors = { TypeError, RangeError, SyntaxError, ReferenceError };

function toError(err) {
  if (!err.wozError) {
    const e = new (standardErrors[err.name] ?? Error)(err.message);
    if (err.name) e.name = err.name;
    return e;
  }
  const e = new WozError(err.message, err.messages);
  if (err.stats) e.stats = err.stats;
  return e;
}

export function connectWozzle(worker) {
  return new RemoteWozzle(worker);
}

export class RemoteWozzle {
  constructor(worker) {
    this.worker = worker;
    this.messages = [];
    this.nextId = 1;
    this.pending = new Map();
    listen(worker, ({ id, ok, value, error, messages }) => {
      const p = this.pending.get(id);
      if (!p) return;
      this.pending.delete(id);
      this.messages = messages;
      if (ok) p.resolve(value);
      else p.reject(toError(error));
    });
    const lost = (why) => {
      for (const p of this.pending.values()) p.reject(new WozError(why, []));
      this.pending.clear();
    };
    if (worker.addEventListener) {
      worker.addEventListener('error', (e) => lost(e.message || 'the wozzle worker failed'));
    } else {
      worker.on('error', (e) => lost(e.message));
    }
  }

  call(op, ...args) {
    return new Promise((resolve, reject) => {
      const id = this.nextId++;
      this.pending.set(id, { resolve, reject });
      this.worker.postMessage({ id, op, args });
    });
  }

  detect(bytes, name) {
    return this.call('detect', bytes, name);
  }

  async open(bytes, name, fsType) {
    const { id, fsType: t } = await this.call('open', bytes, name, fsType);
    return new RemoteImage(this, id, t);
  }

  createBlank(opts) {
    return this.call('createBlank', opts);
  }

  convert(bytes, fromName, toName) {
    return this.call('convert', bytes, fromName, toName);
  }

  parseSize(token) {
    return this.call('parseSize', token);
  }

  copy(src, dst, opts = {}) {
    return this.call('copy', src.id, dst.id, opts);
  }

  terminate() {
    this.worker.terminate();
  }
}

export class RemoteImage {
  constructor(woz, id, fsType) {
    this.woz = woz;
    this.id = id;
    this.fsType = fsType;
  }

  call(method, ...args) {
    return this.woz.call('image', this.id, method, ...args);
  }

  probe() { return this.call('probe'); }
  isDirty() { return this.call('isDirty'); }
  volumeInfo() { return this.call('volumeInfo'); }
  list(dir = '') { return this.call('list', dir); }
  stat(path) { return this.call('stat', path); }
  read(path, opts = {}) { return this.call('read', path, opts); }
  listBasic(path) { return this.call('listBasic', path); }
  write(path, bytes, opts = {}) { return this.call('write', path, bytes, opts); }
  delete(path, opts = {}) { return this.call('delete', path, opts); }
  removeDirectory(path) { return this.call('removeDirectory', path); }
  makeDirectory(path) { return this.call('makeDirectory', path); }
  renameVolume(name) { return this.call('renameVolume', name); }
  installBootBlocks(donorBytes = null, donorName = 'donor.po') {
    return this.call('installBootBlocks', donorBytes, donorName);
  }
  krunch(afterFile = null) { return this.call('krunch', afterFile); }
  resize(blocks, opts = {}) { return this.call('resize', blocks, opts); }
  save(name) { return this.call('save', name); }
  close() { return this.call('close'); }
}
