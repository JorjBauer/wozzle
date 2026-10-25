// Host files in and out. Under Tauri that's the native open and save
// dialogs and real paths, so an image can be saved back where it came
// from. In a plain browser it's a file picker and a download, which is
// enough to use the app as a web page.

import { invoke, isTauri } from '@tauri-apps/api/core';
import { open, save } from '@tauri-apps/plugin-dialog';
import { readFile } from '@tauri-apps/plugin-fs';

export interface PickedFile {
  name: string;
  bytes: Uint8Array;
  // Only under Tauri: where it lives, for saving back in place
  path?: string;
}

export const IMAGE_EXTENSIONS = ['woz', 'dsk', 'do', 'po', 'nib', 'hdv', 'img', '2mg', '2img', 'd13'];

export const native = isTauri();

function baseName(path: string): string {
  return path.split(/[\\/]/).pop() || path;
}

// The browser's file picker, as a promise. The input has to be in the
// page while it's used (some browsers ignore a click on a detached one),
// and a canceled pick resolves to null where the browser says so.
function browserPick(accept: string): Promise<PickedFile | null> {
  return new Promise((resolve) => {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = accept;
    input.style.display = 'none';
    document.body.appendChild(input);
    const done = (f: PickedFile | null) => {
      input.remove();
      resolve(f);
    };
    input.addEventListener('cancel', () => done(null));
    input.onchange = async () => {
      const f = input.files?.[0];
      if (!f) return done(null);
      done({ name: f.name, bytes: new Uint8Array(await f.arrayBuffer()) });
    };
    input.click();
  });
}

export async function pickImage(): Promise<PickedFile | null> {
  if (!native) return browserPick(IMAGE_EXTENSIONS.map((e) => '.' + e).join(','));
  const path = await open({
    multiple: false,
    directory: false,
    filters: [{ name: 'Apple II disk images', extensions: IMAGE_EXTENSIONS }],
  });
  if (!path) return null;
  return { name: baseName(path), path, bytes: await readFile(path) };
}

export async function pickAnyFile(): Promise<PickedFile | null> {
  if (!native) return browserPick('');
  const path = await open({ multiple: false, directory: false });
  if (!path) return null;
  return { name: baseName(path), path, bytes: await readFile(path) };
}

// Save something where the user picks. `produce` makes the bytes once the
// name is known, since an image's extension picks its format. Returns
// where it went (a path under Tauri, the suggested name in a browser),
// or null if canceled.
export async function saveAs(suggestedName: string,
                             produce: (name: string) => Promise<Uint8Array>,
                             imageFilter = false): Promise<PickedFile | null> {
  if (!native) {
    const bytes = await produce(suggestedName);
    const url = URL.createObjectURL(new Blob([bytes as BlobPart]));
    const a = document.createElement('a');
    a.href = url;
    a.download = suggestedName;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
    return { name: suggestedName, bytes };
  }
  const path = await save({
    defaultPath: suggestedName,
    filters: imageFilter
      ? [{ name: 'Apple II disk images', extensions: IMAGE_EXTENSIONS }]
      : undefined,
  });
  if (!path) return null;
  const bytes = await produce(baseName(path));
  await saveInPlace(path, bytes);
  return { name: baseName(path), path, bytes };
}

// Write to a path we got from pickImage or saveAs (Tauri only). The app
// writes it beside the real file and renames it over, so a failed save
// leaves the old one whole.
export async function saveInPlace(path: string, bytes: Uint8Array): Promise<void> {
  await invoke('save_file', bytes, { headers: { 'x-wozzle-path': encodeURIComponent(path) } });
}
