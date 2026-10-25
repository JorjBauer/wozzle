// Files dragged into the window, and files dragged out of it.
//
// Under Tauri, the webview never sees a drop as an HTML drag event: Tauri
// takes it and hands us the paths, which it lets us read (see lib.rs).
// Dragging out goes through tauri-plugin-drag, which needs a real file,
// so we write one to a temporary folder first. In a plain browser, drops
// are HTML drag events, and dragging out works in Chrome by way of its
// DownloadURL drag type.

import { invoke } from '@tauri-apps/api/core';
import { getCurrentWebview } from '@tauri-apps/api/webview';
import { startDrag } from '@crabnebula/tauri-plugin-drag';
import { readFile } from '@tauri-apps/plugin-fs';
import { native, type PickedFile } from './files';

export interface DropHandlers {
  // Something's being dragged over the window, at this point (CSS pixels)
  over(x: number, y: number): void;
  leave(): void;
  drop(files: PickedFile[], x: number, y: number): void;
}

function baseName(path: string): string {
  return path.split(/[\\/]/).pop() || path;
}

// Listen for files dropped on the window. Returns a function that stops.
export async function listenForDrops(h: DropHandlers): Promise<() => void> {
  if (native) {
    return getCurrentWebview().onDragDropEvent(async (e) => {
      const p = e.payload;
      if (p.type === 'leave') return h.leave();
      // Tauri's positions are physical pixels
      const x = p.position.x / window.devicePixelRatio;
      const y = p.position.y / window.devicePixelRatio;
      if (p.type !== 'drop') return h.over(x, y);
      const files: PickedFile[] = [];
      for (const path of p.paths) {
        // A folder won't read, and there's nothing to do with one yet
        try {
          files.push({ name: baseName(path), path, bytes: await readFile(path) });
        } catch {}
      }
      h.drop(files, x, y);
    });
  }

  const hasFiles = (e: DragEvent) => !!e.dataTransfer?.types.includes('Files');
  const over = (e: DragEvent) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer!.dropEffect = 'copy';
    h.over(e.clientX, e.clientY);
  };
  // dragleave fires going from one element to another, too; only leaving
  // the window has no element to go to
  const leave = (e: DragEvent) => {
    if (!e.relatedTarget) h.leave();
  };
  const drop = async (e: DragEvent) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    const files: PickedFile[] = [];
    for (const f of e.dataTransfer!.files) {
      try {
        files.push({ name: f.name, bytes: new Uint8Array(await f.arrayBuffer()) });
      } catch {}
    }
    h.drop(files, e.clientX, e.clientY);
  };
  window.addEventListener('dragover', over);
  window.addEventListener('dragleave', leave);
  window.addEventListener('drop', drop);
  return () => {
    window.removeEventListener('dragover', over);
    window.removeEventListener('dragleave', leave);
    window.removeEventListener('drop', drop);
  };
}

// ---- dragging out ----

// The picture that follows the pointer: a page with its corner folded,
// and the file's type on it
function dragIcon(label: string): string {
  const c = document.createElement('canvas');
  c.width = c.height = 64;
  const g = c.getContext('2d')!;
  g.fillStyle = '#ffffff';
  g.strokeStyle = '#6e6e73';
  g.lineWidth = 2;
  g.beginPath();
  g.moveTo(14, 4); g.lineTo(40, 4); g.lineTo(52, 16); g.lineTo(52, 60); g.lineTo(14, 60);
  g.closePath();
  g.fill();
  g.stroke();
  g.beginPath();
  g.moveTo(40, 4); g.lineTo(40, 16); g.lineTo(52, 16);
  g.stroke();
  g.fillStyle = '#1d1d1f';
  g.font = 'bold 12px ui-monospace, Menlo, monospace';
  g.textAlign = 'center';
  g.fillText(label.slice(0, 4), 33, 44);
  return c.toDataURL('image/png');
}

// A file on its way out: made when the button goes down on its row, so
// it's ready by the time the pointer has moved far enough to be a drag
export interface DragOut {
  name: string;
  ready: Promise<string>; // a path under Tauri, a blob URL in a browser
  url?: string;
}

export function prepareDragOut(name: string, produce: () => Promise<Uint8Array>): DragOut {
  const out: DragOut = { name, ready: Promise.resolve('') };
  out.ready = produce().then(async (bytes) => {
    if (native) {
      // Apple II names are ASCII, which is all a header can carry
      const header = name.replace(/[^\x20-\x7e]/g, '_');
      return invoke<string>('stage_drag_file', bytes, { headers: { 'x-wozzle-name': header } });
    }
    out.url = URL.createObjectURL(new Blob([bytes as BlobPart]));
    return out.url;
  });
  return out;
}

export function releaseDragOut(d: DragOut | null) {
  if (d?.url) {
    const url = d.url;
    // Chrome reads the blob after the drop, so give it a moment
    setTimeout(() => URL.revokeObjectURL(url), 60000);
  }
}

// Start the native drag (Tauri). The button has to still be down.
export async function startNativeDrag(d: DragOut, typeLabel: string) {
  const path = await d.ready;
  await startDrag({ item: [path], icon: dragIcon(typeLabel) });
}

// For a browser's dragstart: Chrome turns this into a file on drop. It
// has to be set during dragstart, so it only works once the file's ready.
export function setBrowserDragData(e: DragEvent, d: DragOut): boolean {
  if (!d.url || !e.dataTransfer) return false;
  e.dataTransfer.effectAllowed = 'copy';
  e.dataTransfer.setData('DownloadURL', `application/octet-stream:${d.name}:${d.url}`);
  e.dataTransfer.setData('text/plain', d.name);
  return true;
}
