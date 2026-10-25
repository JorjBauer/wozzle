// A name for a host file on the disk, following the filesystem's rules,
// so that adding "my notes.txt" doesn't just fail. The Add dialog shows it
// for editing; a drop of several files takes it as is.

import type { FsName } from '@wasm/wozzle.mjs';

export function diskName(fsType: FsName | null, hostName: string): string {
  let n = hostName.replace(/\.(bas|txt|bin)$/i, '').toUpperCase();
  if (fsType === 'dos33') {
    // Anything printable but a comma, up to 30 characters
    return n.replace(/[^\x20-\x7e]|,/g, '.').slice(0, 30) || 'FILE';
  }
  if (fsType === 'pascal') {
    // Up to 15, and none of the characters the filer reserves
    return n.replace(/[^\x21-\x7e]|[$=?,[#:]/g, '.').slice(0, 15) || 'FILE';
  }
  // ProDOS: a letter, then letters, digits and periods, up to 15
  n = n.replace(/[^A-Z0-9.]/g, '.');
  if (!/^[A-Z]/.test(n)) n = 'F' + n;
  return n.slice(0, 15);
}

// Whether a host file is an Applesoft listing to tokenize on the way in
export function looksLikeListing(fsType: FsName | null, hostName: string): boolean {
  return fsType !== 'pascal' && /\.bas$/i.test(hostName);
}
