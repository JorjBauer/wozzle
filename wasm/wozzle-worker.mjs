// The wozzle library in a worker of its own, so a long job (a 32MB image,
// say) doesn't stall the page. In the worker's script:
//
//   import { serveWozzle } from './wozzle-worker.mjs';
//   serveWozzle();
//
// and on the page, connectWozzle() from wozzle-remote.mjs. Under Node,
// pass worker_threads' parentPort to serveWozzle().
//
// Open images stay in the worker; the page holds them by number. Each
// request runs to the end before the next one starts, because the library
// is single-threaded and so is the worker.

import { loadWozzle, WozError } from './wozzle.mjs';

// What a remote image may call. Anything else is refused, so a message
// can't reach the plumbing.
const IMAGE_METHODS = new Set([
  'probe', 'isDirty', 'volumeInfo', 'list', 'stat', 'read', 'listBasic',
  'write', 'delete', 'removeDirectory', 'makeDirectory', 'renameVolume',
  'installBootBlocks', 'krunch', 'resize', 'save', 'close',
]);

// A browser worker's global and a Node MessagePort both have
// addEventListener; the data is on the event either way
function listen(port, fn) {
  port.addEventListener('message', (e) => fn(e.data));
  if (port.start) port.start();
}

export function serveWozzle(port = globalThis, moduleOptions = {}) {
  const ready = loadWozzle(moduleOptions);
  const images = new Map();
  let nextId = 1;

  const image = (id) => {
    const img = images.get(id);
    if (!img) throw new WozError('the image is closed', []);
    return img;
  };

  const ops = {
    detect: (woz, bytes, name) => woz.detect(bytes, name),
    open: (woz, bytes, name, fsType) => {
      const img = woz.open(bytes, name, fsType);
      const id = nextId++;
      images.set(id, img);
      return { id, fsType: img.fsType };
    },
    createBlank: (woz, opts) => woz.createBlank(opts),
    convert: (woz, bytes, fromName, toName) => woz.convert(bytes, fromName, toName),
    parseSize: (woz, token) => woz.parseSize(token),
    copy: (woz, srcId, dstId, opts) => woz.copy(image(srcId), image(dstId), opts),
    image: (woz, id, method, ...args) => {
      if (!IMAGE_METHODS.has(method)) throw new TypeError(`no image method '${method}'`);
      if (method === 'close') {
        images.get(id)?.close();
        images.delete(id);
        return undefined;
      }
      return image(id)[method](...args);
    },
  };

  listen(port, async ({ id, op, args }) => {
    let woz;
    try {
      woz = await ready;
      if (!Object.hasOwn(ops, op)) throw new TypeError(`no operation '${op}'`);
      const value = ops[op](woz, ...args);
      // Bytes made for the reply are handed over rather than copied, but
      // never the library's own heap
      const transfer = value instanceof Uint8Array &&
        value.buffer !== woz.mod.HEAPU8.buffer &&
        value.byteOffset === 0 && value.byteLength === value.buffer.byteLength
        ? [value.buffer] : [];
      port.postMessage({ id, ok: true, value, messages: woz.messages }, transfer);
    } catch (e) {
      port.postMessage({
        id,
        ok: false,
        error: {
          wozError: e instanceof WozError,
          name: e?.name,
          message: e?.message ?? String(e),
          messages: e?.messages ?? [],
          stats: e?.stats,
        },
        messages: woz?.messages ?? [],
      });
    }
  });
}
