// The library runs in a worker (woz.worker.ts), so a big image doesn't
// freeze the window. There's a single worker, and the whole app shares it,
// because the library is single-threaded.

import { connectWozzle, type RemoteWozzle } from '@wasm/wozzle-remote.mjs';

let woz: RemoteWozzle | null = null;

export async function getWozzle(): Promise<RemoteWozzle> {
  if (!woz) {
    woz = connectWozzle(new Worker(new URL('./woz.worker.ts', import.meta.url),
                                   { type: 'module' }));
    // A first call, so a library that fails to load says so now rather
    // than on the first click
    await woz.parseSize('140k');
  }
  return woz;
}

// How a file type reads in each filesystem's own terms
const prodosTypes: Record<number, string> = {
  0x04: 'TXT', 0x06: 'BIN', 0x0f: 'DIR', 0x19: 'ADB', 0x1a: 'AWP', 0x1b: 'ASP',
  0xef: 'PAS', 0xf0: 'CMD', 0xfa: 'INT', 0xfb: 'IVR', 0xfc: 'BAS', 0xfd: 'VAR',
  0xfe: 'REL', 0xff: 'SYS',
};
const dosTypes: Record<number, string> = {
  0x04: 'T', 0x06: 'B', 0xfa: 'I', 0xfc: 'A', 0xfe: 'R', 0xff: 'S',
};
const pascalKinds = ['UNTYPED', 'BAD', 'CODE', 'TEXT', 'INFO', 'DATA', 'GRAF',
                     'FOTO', 'SECDIR'];

export function typeName(fsType: string | null, fileType: number): string {
  const hex = '$' + fileType.toString(16).toUpperCase().padStart(2, '0');
  if (fsType === 'pascal') return pascalKinds[fileType] ?? hex;
  if (fsType === 'dos33') return dosTypes[fileType] ?? hex;
  return prodosTypes[fileType] ?? hex;
}

export function hex(n: number, digits: number): string {
  return '$' + n.toString(16).toUpperCase().padStart(digits, '0');
}

// Apple II text: high bits off, carriage returns as line ends
export function appleText(bytes: Uint8Array): string {
  let s = '';
  for (const b of bytes) {
    const c = b & 0x7f;
    s += c === 0x0d ? '\n' : String.fromCharCode(c);
  }
  return s;
}
