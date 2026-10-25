<script lang="ts">
  import type { RemoteWozzle, RemoteImage } from '@wasm/wozzle-remote.mjs';
  import type { WozEntry, VolumeInfo, FsName } from '@wasm/wozzle.mjs';
  import { getWozzle, typeName, hex, appleText } from './lib/woz';
  import { native, pickImage, pickAnyFile, saveAs, saveInPlace, type PickedFile } from './lib/files';
  import { diskName, looksLikeListing } from './lib/names';
  import { loadAddress, hexDump, disassemble, type AsmLine } from './lib/dump';
  import {
    listenForDrops, prepareDragOut, releaseDragOut, startNativeDrag, setBrowserDragData,
    type DragOut,
  } from './lib/dragdrop';
  import { getCurrentWindow } from '@tauri-apps/api/window';
  import { onMount, untrack } from 'svelte';
  import Modal from './lib/Modal.svelte';
  import DiskArt, { diskKind } from './lib/DiskArt.svelte';

  let woz: RemoteWozzle | null = $state(null);
  let img: RemoteImage | null = $state(null);
  let imageName = $state('');
  let imagePath: string | undefined = $state(undefined);
  let dir = $state('');
  let entries: WozEntry[] = $state([]);
  let info: VolumeInfo | null = $state(null);
  let selected: string | null = $state(null);
  // Unsaved changes: the library says the image changed, or it's a new
  // image that has never been saved anywhere
  let changed = $state(false);
  let isNew = $state(false);
  const dirty = $derived(!!img && (changed || isNew));
  let status = $state('');
  let statusIsError = $state(false);
  // While the library is working, the controls are off, so nothing
  // starts on an image that's halfway through changing
  let busy = $state(false);
  // Waiting on a file picker or a question isn't "Working…"
  let waitingOnUser = 0;

  async function fromUser<T>(p: Promise<T>): Promise<T> {
    waitingOnUser++;
    if (status === 'Working…') say('');
    try {
      return await p;
    } finally {
      waitingOnUser--;
    }
  }

  // Which dialog is up, if any
  type DialogKind = 'new' | 'saveAs' | 'add' | 'extract' | 'view' | 'mkdir' | 'delete' | null;
  let dialog: DialogKind = $state(null);

  getWozzle().then((w) => (woz = w)).catch((e) => fail(e));

  const selectedEntry = $derived(entries.find((e) => e.name === selected) ?? null);
  const fsLabel: Record<string, string> = { dos33: 'DOS 3.3', prodos: 'ProDOS', pascal: 'Pascal' };
  // A DOS disk with 13 sectors a track is DOS 3.2
  const fsName = (v: VolumeInfo | null) =>
    v?.fsType === 'dos33' && v.sectorsPerTrack === 13 ? 'DOS 3.2' : fsLabel[v?.fsType ?? ''] ?? 'unknown';

  // The detail drawer on the right: the volume, and the selected file.
  // Whether it's open is remembered, where the webview lets us.
  let showDetails = $state(readPref('details', true));
  function readPref(key: string, fallback: boolean): boolean {
    try {
      const v = localStorage.getItem(key);
      return v === null ? fallback : v === '1';
    } catch { return fallback; }
  }
  function toggleDetails() {
    showDetails = !showDetails;
    try { localStorage.setItem('details', showDetails ? '1' : '0'); } catch {}
  }

  function say(text: string) {
    status = text;
    statusIsError = false;
  }

  function fail(e: unknown) {
    status = e instanceof Error ? e.message : String(e);
    statusIsError = true;
  }

  // Run something that uses the library: one at a time, with a failure
  // reported in the status line. A quick one shows nothing, but one that
  // takes a while says so.
  async function run(what: () => Promise<void>) {
    if (busy) return;
    busy = true;
    const slow = setTimeout(() => { if (!waitingOnUser) say('Working…'); }, 250);
    try {
      await what();
    } catch (e) {
      fail(e);
    } finally {
      clearTimeout(slow);
      if (status === 'Working…') say('');
      busy = false;
    }
  }

  async function refresh() {
    if (!img) return;
    entries = await img.list(dir);
    info = await img.volumeInfo();
    changed = await img.isDirty();
    if (selected && !entries.some((e) => e.name === selected)) selected = null;
  }

  function pathOf(name: string) {
    return dir ? `${dir}/${name}` : name;
  }

  async function show(bytes: Uint8Array, name: string, path?: string) {
    if (!woz) return;
    const opened = await woz.open(bytes, name);
    const old = img;
    img = opened;
    imageName = name;
    imagePath = path;
    isNew = false;
    dir = '';
    selected = null;
    await old?.close();
    await refresh();
    say(`Opened ${name} (${fsName(info)})`);
  }

  // ---- unsaved changes ----
  // The question, while it's up: what's about to happen, and where the
  // answer goes
  type Unsaved = 'save' | 'discard' | 'cancel';
  let unsavedPrompt: { action: string; answer: (a: Unsaved) => void } | null = $state(null);

  function askUnsaved(action: string): Promise<Unsaved> {
    return fromUser(new Promise((resolve) => {
      unsavedPrompt = { action, answer: (a) => { unsavedPrompt = null; resolve(a); } };
    }));
  }

  // Save without any more questions than it takes: back where it came
  // from if we can, or wherever the user says. False if it didn't happen.
  async function saveNow(): Promise<boolean> {
    if (!img) return true;
    if (native && imagePath) {
      const bytes = await img.save();
      await saveInPlace(imagePath, bytes);
      await show(bytes, imageName, imagePath);
      say(`Saved ${imageName}`);
      return true;
    }
    const f = await fromUser(saveAs(imageName, (name) => img!.save(name), true));
    if (!f) return false;
    await show(f.bytes, f.name, f.path);
    say(`Saved ${f.name}`);
    return true;
  }

  // Whether it's all right to let the current image go: nothing unsaved,
  // or the user saved it or said to throw the changes away
  async function okToLeave(action: string): Promise<boolean> {
    if (!dirty) return true;
    const a = await askUnsaved(action);
    if (a === 'cancel') return false;
    if (a === 'discard') return true;
    try {
      return await saveNow();
    } catch (e) {
      fail(e);
      return false;
    }
  }

  // The window title shows what's open and whether it's changed
  $effect(() => {
    const title = img ? `${imageName}${dirty ? ' (edited)' : ''}` : 'Wozzle';
    document.title = title;
    if (native) getCurrentWindow().setTitle(title).catch(() => {});
  });

  onMount(() => {
    const stops: (() => void)[] = [];
    if (native) {
      // Closing the window, or quitting (lib.rs turns Quit into a close)
      getCurrentWindow().onCloseRequested(async (event) => {
        if (!dirty) return;
        event.preventDefault();
        if (await okToLeave('quitting')) await getCurrentWindow().destroy();
      }).then((stop) => stops.push(stop));
    } else {
      // A browser can only ask its own question, with its own wording
      const unload = (e: BeforeUnloadEvent) => {
        if (dirty) e.preventDefault();
      };
      window.addEventListener('beforeunload', unload);
      stops.push(() => window.removeEventListener('beforeunload', unload));
    }
    listenForDrops({ over: dragOver, leave: dragLeave, drop: dropped })
      .then((stop) => stops.push(stop));
    return () => stops.forEach((stop) => stop());
  });

  const openImage = () => run(async () => {
    if (!(await okToLeave('opening another image'))) return;
    const f = await fromUser(pickImage());
    if (f) await show(f.bytes, f.name, f.path);
  });

  const enter = (e: WozEntry) => run(async () => {
    if (!e.isDirectory) return;
    dir = pathOf(e.name);
    selected = null;
    await refresh();
  });

  const up = (to: number) => run(async () => {
    dir = dir.split('/').slice(0, to).join('/');
    selected = null;
    await refresh();
  });

  const save = () => {
    if (!img) return;
    if (!native || !imagePath) {
      dialog = 'saveAs';
      return;
    }
    // Saving reopens what was written, so what's on screen is what's on disk
    return run(async () => { await saveNow(); });
  };

  // ---- Save As ----
  const formats = [
    { ext: 'woz', label: 'WOZ 2' },
    { ext: 'dsk', label: 'DSK (DOS order)' },
    { ext: 'po', label: 'PO (ProDOS order)' },
    { ext: 'nib', label: 'NIB' },
    { ext: 'hdv', label: 'HDV (hard disk)' },
    { ext: '2mg', label: '2MG' },
    { ext: 'd13', label: 'D13 (13-sector)' },
  ];
  let saveExt = $state('woz');

  const doSaveAs = () => run(async () => {
    if (!img) return;
    dialog = null;
    const stem = imageName.replace(/\.[^.]*$/, '') || 'image';
    const f = await fromUser(saveAs(`${stem}.${saveExt}`, (name) => img!.save(name), true));
    if (!f) return;
    // What was saved is what's open now, whatever format it went out in
    await show(f.bytes, f.name, f.path);
    say(`Saved ${f.name}`);
  });

  // ---- New image ----
  let newFs: FsName = $state('prodos');
  let newName = $state('blank.po');
  let newVolume = $state('BLANK');
  let newDosVolume = $state(254);
  let newSize = $state('140k');
  let newBootable = $state(false);

  function startNew() {
    run(async () => {
      if (await okToLeave('making a new image')) dialog = 'new';
    });
  }

  const doNew = () => run(async () => {
    if (!woz) return;
    const blocks = newFs === 'dos33' ? 280 : await woz.parseSize(newSize);
    const bytes = await woz.createBlank({
      name: newName, fsType: newFs, volumeName: newVolume, dosVolume: newDosVolume,
      blocks, bootable: newBootable,
    });
    dialog = null;
    await show(bytes, newName);
    // It exists only in memory until it's saved somewhere
    isNew = true;
  });

  $effect(() => {
    // A sensible extension for what's being made, when the filesystem or
    // size changes (and only then, so a name typed by hand stays put): .po
    // for the floppy sizes and .hdv for a hard disk, though the library
    // takes .po at any of these sizes.
    const floppy = newSize === '140k' || newSize === '800k';
    const ext = newFs === 'dos33' ? 'dsk' : floppy ? 'po' : 'hdv';
    newName = untrack(() => newName).replace(/\.[^.]*$/, '') + '.' + ext;
  });

  // ---- Add a file ----
  let addBytes: Uint8Array | null = null;
  // The directory it goes into: where we are, or a folder it was dropped on
  let addDir = $state('');
  let addSource = $state('');
  let addName = $state('');
  let addType = $state('');
  let addAux = $state('');
  let addTokenize = $state(false);

  function showAdd(f: PickedFile, into: string) {
    addBytes = f.bytes;
    addDir = into;
    addSource = f.name;
    addName = diskName(img?.fsType ?? null, f.name);
    addType = '';
    addAux = '';
    addTokenize = looksLikeListing(img?.fsType ?? null, f.name);
    dialog = 'add';
  }

  async function startAdd() {
    try {
      const f = await fromUser(pickAnyFile());
      if (f) showAdd(f, dir);
    } catch (e) {
      fail(e);
    }
  }

  const doAdd = () => run(async () => {
    if (!img || !addBytes) return;
    const t = addType.trim() ? parseInt(addType.replace(/^\$|^0x/i, ''), 16) : -1;
    const a = addAux.trim() ? parseInt(addAux.replace(/^\$|^0x/i, ''), 16) : -1;
    const into = addDir ? `${addDir}/${addName}` : addName;
    await img.write(into, addBytes, { fileType: t, auxType: a, tokenize: addTokenize });
    dialog = null;
    await refresh();
    say(`Added ${addName}`);
  });

  // ---- Extract ----
  let extractMode: 'contents' | 'raw' | 'basic' = $state('contents');

  const doExtract = () => run(async () => {
    if (!img || !selectedEntry) return;
    dialog = null;
    const e = selectedEntry;
    const path = pathOf(e.name);
    const f = await fromUser(saveAs(extractMode === 'basic' ? `${e.name}.bas` : e.name, async () =>
      extractMode === 'basic'
        ? new TextEncoder().encode(await img!.listBasic(path))
        : img!.read(path, { raw: extractMode === 'raw' })));
    if (f) say(`Extracted ${e.name}`);
  });

  // ---- View ----
  // A BASIC program as its listing, text as text, and anything else as a
  // hex dump; each of them can be switched to the other views.
  type ViewMode = 'listing' | 'text' | 'hex' | 'asm';
  const viewModeNames: Record<ViewMode, string> = {
    listing: 'Listing', text: 'Text', hex: 'Hex', asm: '65C02',
  };
  // A dump is about 6 characters of text a byte, so a big file is cut
  // short rather than handed to the window whole
  const VIEW_LIMIT = 256 * 1024;
  let viewTitle = $state('');
  let viewBytes: Uint8Array = $state(new Uint8Array());
  let viewBase = $state(0);
  let viewListing: string | null = $state(null);
  let viewMode: ViewMode = $state('hex');
  const viewModes = $derived<ViewMode[]>(viewListing !== null
    ? ['listing', 'hex', 'asm', 'text'] : ['hex', 'asm', 'text']);
  const viewShown = $derived(viewMode === 'listing' || viewMode === 'text'
    ? viewBytes : viewBytes.subarray(0, VIEW_LIMIT));
  const viewText = $derived(
    viewMode === 'listing' ? viewListing ?? ''
    : viewMode === 'text' ? appleText(viewShown)
    : viewMode === 'hex' ? hexDump(viewShown, viewBase) : '');
  // The disassembly is laid out in columns like aiie's debugger: address,
  // bytes, label, instruction, with the 65C02's own instructions marked.
  // It's built as markup because a big file has tens of thousands of
  // lines, which is slow as separate components.
  const viewAsm = $derived(viewMode === 'asm' ? asmHtml(disassemble(viewShown, viewBase)) : '');

  function asmHtml(lines: AsmLine[]): string {
    const esc = (t: string) => t.replace(/&/g, '&amp;').replace(/</g, '&lt;');
    return lines.map((l) =>
      `<div class="aline"><span class="dim">${l.addr.toString(16).toUpperCase().padStart(4, '0')}</span>` +
      `<span class="dim">${l.bytes}</span><span class="label">${esc(l.label)}</span>` +
      `<span${l.c02 ? ' class="c02" title="65C02 only"' : ''}>${esc(l.text)}</span></div>`).join('');
  }

  function isTextFile(fsType: string | null, fileType: number) {
    return fsType === 'pascal' ? fileType === 3 : fileType === 0x04;
  }

  const view = () => run(async () => {
    if (!img || !selectedEntry) return;
    const e = selectedEntry;
    const path = pathOf(e.name);
    const isBasic = img.fsType !== 'pascal' && (e.fileType === 0xfc || e.fileType === 0xfa);
    viewBytes = await img.read(path);
    viewListing = isBasic ? await img.listBasic(path) : null;
    viewBase = loadAddress(img.fsType, e.fileType, e.auxType);
    viewMode = isBasic ? 'listing' : isTextFile(img.fsType, e.fileType) ? 'text' : 'hex';
    viewTitle = e.name;
    dialog = 'view';
  });

  // ---- New folder, delete ----
  let folderName = $state('');

  const doMkdir = () => run(async () => {
    if (!img) return;
    await img.makeDirectory(pathOf(folderName));
    dialog = null;
    await refresh();
    say(`Made ${folderName}`);
  });

  const doDelete = () => run(async () => {
    if (!img || !selectedEntry) return;
    const e = selectedEntry;
    await img.delete(pathOf(e.name), { recursive: e.isDirectory });
    dialog = null;
    await refresh();
    say(`Deleted ${e.name}`);
  });

  // ---- drag and drop ----
  // What a drop at the pointer would do, shown while something's dragged
  // over the window: add to the image (into a folder, if it's over one),
  // or open it as the image
  type DropTarget = { kind: 'add'; into: string } | { kind: 'open' };
  let dropTarget: DropTarget | null = $state(null);

  function targetAt(x: number, y: number): DropTarget {
    const el = document.elementFromPoint(x, y);
    if (!img || !el?.closest('.files')) return { kind: 'open' };
    const row = el.closest('tr[data-dir]') as HTMLElement | null;
    return { kind: 'add', into: row ? pathOf(row.dataset.dir!) : dir };
  }

  function dragOver(x: number, y: number) {
    dropTarget = busy || dialog || unsavedPrompt ? null : targetAt(x, y);
  }

  function dragLeave() {
    dropTarget = null;
  }

  function dropped(files: PickedFile[], x: number, y: number) {
    dropTarget = null;
    if (busy || dialog || unsavedPrompt || !woz || !files.length) return;
    const t = targetAt(x, y);
    if (t.kind === 'open') {
      run(async () => {
        if (files.length > 1) say(`Opening ${files[0].name}; drop one image at a time to open it`);
        if (!(await okToLeave('opening another image'))) return;
        await show(files[0].bytes, files[0].name, files[0].path);
      });
      return;
    }
    // One file gets the Add dialog, to set its name and type; several go
    // straight in, named for the disk
    if (files.length === 1) {
      showAdd(files[0], t.into);
      return;
    }
    run(async () => {
      const failed: string[] = [];
      for (const f of files) {
        const name = diskName(img!.fsType, f.name);
        try {
          await img!.write(t.into ? `${t.into}/${name}` : name, f.bytes,
                           { tokenize: looksLikeListing(img!.fsType, f.name) });
        } catch (e) {
          failed.push(`${f.name}: ${e instanceof Error ? e.message : e}`);
        }
      }
      await refresh();
      if (failed.length) fail(`Added ${files.length - failed.length} of ${files.length}. ${failed.join('; ')}`);
      else say(`Added ${files.length} files${t.into ? ` to ${t.into}` : ''}`);
    });
  }

  // Dragging a file out of the image, to the desktop or a folder. The file
  // is read as soon as the button goes down on it, and the drag starts
  // once the pointer has moved a few pixels.
  let dragOut: { d: DragOut; entry: WozEntry; x: number; y: number } | null = null;

  function rowDown(e: MouseEvent, entry: WozEntry) {
    if (e.button !== 0 || entry.isDirectory || busy || !img) return;
    releaseDragOut(dragOut?.d ?? null);
    const image = img;
    const path = pathOf(entry.name);
    const d = prepareDragOut(entry.name, () => image.read(path));
    d.ready.catch(() => {}); // a failure shows if the drag happens
    dragOut = { d, entry, x: e.clientX, y: e.clientY };
  }

  function pointerMove(e: MouseEvent) {
    if (!dragOut || !native) return;
    if (!(e.buttons & 1)) {
      dragOut = null;
      return;
    }
    if (Math.hypot(e.clientX - dragOut.x, e.clientY - dragOut.y) < 5) return;
    const { d, entry } = dragOut;
    dragOut = null;
    startNativeDrag(d, typeName(img?.fsType ?? null, entry.fileType)).catch(fail);
  }

  // A browser's own drag, for Chrome's DownloadURL
  function rowDragStart(e: DragEvent) {
    if (!dragOut || !setBrowserDragData(e, dragOut.d)) e.preventDefault();
  }

  function formatDate(d: Date | null) {
    return d ? d.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }) : '';
  }
</script>

<svelte:window onmousemove={pointerMove} onmouseup={() => (dragOut = null)} />

<div class="app">
  <header class="toolbar">
    <button onclick={openImage} disabled={!woz || busy}>Open…</button>
    <button onclick={startNew} disabled={!woz || busy}>New…</button>
    <button onclick={save} disabled={!img || !imagePath || busy}
            title={img && !imagePath ? "It hasn't been saved yet; use Save As" : undefined}>Save</button>
    <button onclick={() => (dialog = 'saveAs')} disabled={!img || busy}>Save As…</button>
    <span class="sep"></span>
    <button onclick={() => (dialog = 'extract')} disabled={busy || !selectedEntry || selectedEntry.isDirectory}>Extract…</button>
    <button onclick={view} disabled={busy || !selectedEntry || selectedEntry.isDirectory}>View</button>
    <button onclick={startAdd} disabled={!img || busy}>Add File…</button>
    <button onclick={() => { folderName = ''; dialog = 'mkdir'; }} disabled={!img || busy || img.fsType !== 'prodos'}>New Folder…</button>
    <button onclick={() => (dialog = 'delete')} disabled={busy || !selectedEntry}>Delete</button>
    <span class="grow"></span>
    <!-- The chevron points the way the drawer will move -->
    <button class:on={showDetails} aria-pressed={showDetails} onclick={toggleDetails}
            title={showDetails ? 'Hide the details' : 'Show the details'}>{showDetails ? 'Details ›' : '‹ Details'}</button>
  </header>

  <main class="body" class:busy aria-busy={busy}>
    {#if img && info}
      <section class="files" class:drop-here={dropTarget?.kind === 'add' && dropTarget.into === dir}>
        <nav class="crumbs">
          <button class="link" onclick={() => up(0)}>/</button>
          {#each dir ? dir.split('/') : [] as part, i}
            <span>›</span><button class="link" onclick={() => up(i + 1)}>{part}</button>
          {/each}
        </nav>
        <DiskArt kind={diskKind(info.totalUnits * info.unitSize)} class="files-art" />
        <div class="table-wrap">
        <table>
          <thead>
            <tr><th>Name</th><th>Type</th><th>Aux</th><th class="num">Length</th>
              <th class="num">{info.unitSize === 256 ? 'Sectors' : 'Blocks'}</th><th>Modified</th><th></th></tr>
          </thead>
          <tbody>
            {#each entries as e (e.name)}
              <tr class:selected={e.name === selected}
                  class:drop-into={dropTarget?.kind === 'add' && e.isDirectory && dropTarget.into === pathOf(e.name)}
                  data-dir={e.isDirectory ? e.name : undefined}
                  draggable={!native && !e.isDirectory}
                  onmousedown={(ev) => rowDown(ev, e)}
                  ondragstart={rowDragStart}
                  onclick={() => (selected = e.name)}
                  ondblclick={() => (e.isDirectory ? enter(e) : view())}>
                <td>{e.name}{e.isDirectory ? '/' : ''}</td>
                <td>{typeName(img.fsType, e.fileType)}</td>
                <td>{img.fsType === 'pascal' || e.isDirectory ? '' : hex(e.auxType, 4)}</td>
                <td class="num">{e.isDirectory ? '' : e.length}</td>
                <td class="num">{e.unitsUsed}</td>
                <td>{formatDate(e.modified)}</td>
                <td>{e.locked ? 'Locked' : ''}</td>
              </tr>
            {:else}
              <tr><td colspan="7" class="empty">Empty</td></tr>
            {/each}
          </tbody>
        </table>
        </div>
      </section>
    {:else}
      <div class="welcome" class:dragging={dropTarget?.kind === 'open'}>
        <DiskArt kind="floppy525" arrow class="welcome-art" />
        <p>Drop a disk image here</p>
        <p class="hint">or use Open or New. DOS 3.3, ProDOS and Pascal volumes,<br />in .woz, .dsk, .do, .po, .nib, .hdv, .img or .2mg files.</p>
      </div>
    {/if}
    {#if showDetails}
      <aside class="details">
        {#if img && info}
          <h2>{info.volumeName || `Volume ${info.dosVolume}`}{dirty ? ' •' : ''}</h2>
          <dl>
            <dt>Image</dt><dd>{imageName}</dd>
            <dt>Filesystem</dt><dd>{fsName(info)}</dd>
            <dt>Size</dt><dd>{info.totalUnits} {info.unitSize === 256 ? 'sectors' : 'blocks'}</dd>
            <dt>Free</dt><dd>{info.freeUnits}</dd>
            {#if info.creator}<dt>Creator</dt><dd>{info.creator}</dd>{/if}
          </dl>
          {#if selectedEntry}
            {@const e = selectedEntry}
            <h2>{e.name}{e.isDirectory ? '/' : ''}</h2>
            <dl>
              <dt>Type</dt><dd>{typeName(img.fsType, e.fileType)}</dd>
              {#if img.fsType !== 'pascal' && !e.isDirectory}
                <dt>{e.fileType === 0x06 ? 'Loads at' : 'Aux'}</dt><dd>{hex(e.auxType, 4)}</dd>
              {/if}
              {#if !e.isDirectory}<dt>Length</dt><dd>{e.length} bytes</dd>{/if}
              <dt>Uses</dt><dd>{e.unitsUsed} {info.unitSize === 256 ? 'sectors' : 'blocks'}</dd>
              {#if e.created}<dt>Created</dt><dd>{formatDate(e.created)}</dd>{/if}
              {#if e.modified}<dt>Modified</dt><dd>{formatDate(e.modified)}</dd>{/if}
              {#if e.locked}<dt>Locked</dt><dd>Yes</dd>{/if}
              {#if e.hasResourceFork}<dt>Fork</dt><dd>Has a resource fork</dd>{/if}
            </dl>
          {/if}
        {:else}
          <p class="none">No image open</p>
        {/if}
      </aside>
    {/if}
  </main>

  <footer class="status" class:error={statusIsError}>
    {#if dropTarget}
      {dropTarget.kind === 'open' ? 'Drop to open it as the image'
        : `Drop to add to ${dropTarget.into ? dropTarget.into : 'the top of the volume'}`}
    {:else}{status}{/if}
  </footer>
  {#if dropTarget?.kind === 'open'}<div class="drop-open"></div>{/if}
</div>

{#if dialog === 'new'}
  <Modal title="New image" onclose={() => (dialog = null)}>
    <label>Filesystem
      <select bind:value={newFs}>
        <option value="prodos">ProDOS</option>
        <option value="dos33">DOS 3.3</option>
        <option value="pascal">Apple Pascal</option>
      </select>
    </label>
    {#if newFs === 'dos33'}
      <label>Volume number <input type="number" min="1" max="254" bind:value={newDosVolume} /></label>
    {:else}
      <label>Volume name <input bind:value={newVolume} maxlength={newFs === 'pascal' ? 7 : 15} /></label>
      <label>Size
        <select bind:value={newSize}>
          <option value="140k">140K (5.25")</option>
          <option value="800k">800K (3.5")</option>
          <option value="5m">5MB (ProFile)</option>
          <option value="10m">10MB (ProFile)</option>
          <option value="32m">32MB</option>
        </select>
      </label>
      <label class="check"><input type="checkbox" bind:checked={newBootable} /> Bootable (install boot blocks)</label>
    {/if}
    <label>File name <input bind:value={newName} /></label>
    {#snippet actions()}
      <button onclick={() => (dialog = null)}>Cancel</button>
      <button class="primary" onclick={doNew} disabled={busy}>Create</button>
    {/snippet}
  </Modal>
{/if}

{#if dialog === 'saveAs'}
  <Modal title="Save as" onclose={() => (dialog = null)}>
    {#each formats as f}
      <label class="check"><input type="radio" bind:group={saveExt} value={f.ext} /> {f.label}</label>
    {/each}
    {#snippet actions()}
      <button onclick={() => (dialog = null)}>Cancel</button>
      <button class="primary" onclick={doSaveAs}>Save…</button>
    {/snippet}
  </Modal>
{/if}

{#if dialog === 'add'}
  <Modal title={`Add ${addSource}`} onclose={() => (dialog = null)}>
    {#if addDir}<p>Into {addDir}</p>{/if}
    <label>Name on the disk <input bind:value={addName} /></label>
    <label>File type (hex, optional) <input bind:value={addType} placeholder="from the file, or BIN" /></label>
    <label>Aux type (hex, optional) <input bind:value={addAux} placeholder="$2000" /></label>
    <label class="check"><input type="checkbox" bind:checked={addTokenize} /> It's an Applesoft listing: tokenize it</label>
    {#snippet actions()}
      <button onclick={() => (dialog = null)}>Cancel</button>
      <button class="primary" onclick={doAdd} disabled={busy || !addName.trim()}>Add</button>
    {/snippet}
  </Modal>
{/if}

{#if dialog === 'extract' && selectedEntry}
  <Modal title={`Extract ${selectedEntry.name}`} onclose={() => (dialog = null)}>
    <label class="check"><input type="radio" bind:group={extractMode} value="contents" /> Contents</label>
    <label class="check"><input type="radio" bind:group={extractMode} value="raw" /> Everything allocated, as it lies on disk</label>
    {#if img?.fsType !== 'pascal' && (selectedEntry.fileType === 0xfc || selectedEntry.fileType === 0xfa)}
      <label class="check"><input type="radio" bind:group={extractMode} value="basic" /> A BASIC listing, as text</label>
    {/if}
    {#snippet actions()}
      <button onclick={() => (dialog = null)}>Cancel</button>
      <button class="primary" onclick={doExtract}>Extract…</button>
    {/snippet}
  </Modal>
{/if}

{#if dialog === 'view'}
  <Modal title={viewTitle} wide onclose={() => (dialog = null)}>
    <div class="segmented" role="group" aria-label="Show as">
      {#each viewModes as m}
        <button class:on={viewMode === m} aria-pressed={viewMode === m}
                onclick={() => (viewMode = m)}>{viewModeNames[m]}</button>
      {/each}
    </div>
    {#if viewMode === 'asm'}
      <div class="viewer dump asm">{@html viewAsm}</div>
    {:else}
      <pre class="viewer" class:dump={viewMode === 'hex'}>{viewText}</pre>
    {/if}
    {#if viewShown.length < viewBytes.length}
      <p class="hint">The first {VIEW_LIMIT / 1024}K of {viewBytes.length} bytes. Extract the file to see the rest.</p>
    {/if}
    {#snippet actions()}
      <button class="primary" onclick={() => (dialog = null)}>Close</button>
    {/snippet}
  </Modal>
{/if}

{#if dialog === 'mkdir'}
  <Modal title="New folder" onclose={() => (dialog = null)}>
    <label>Name <input bind:value={folderName} maxlength="15" /></label>
    {#snippet actions()}
      <button onclick={() => (dialog = null)}>Cancel</button>
      <button class="primary" onclick={doMkdir} disabled={busy || !folderName.trim()}>Create</button>
    {/snippet}
  </Modal>
{/if}

{#if dialog === 'delete' && selectedEntry}
  <Modal title="Delete" onclose={() => (dialog = null)}>
    <p>Delete {selectedEntry.isDirectory ? 'the folder' : ''} <strong>{selectedEntry.name}</strong>{selectedEntry.isDirectory ? ' and everything in it' : ''}?
      It's gone from the image once you save.</p>
    {#snippet actions()}
      <button onclick={() => (dialog = null)}>Cancel</button>
      <button class="danger" onclick={doDelete} disabled={busy}>Delete</button>
    {/snippet}
  </Modal>
{/if}

<!-- Last, so it sits over any other dialog (it can come up during one, on quit) -->
{#if unsavedPrompt}
  <Modal title="Unsaved changes" onclose={() => unsavedPrompt?.answer('cancel')}>
    <p><strong>{imageName}</strong> has changes that haven't been saved. Save them before {unsavedPrompt.action}?</p>
    {#snippet actions()}
      <button onclick={() => unsavedPrompt?.answer('discard')}>Don't Save</button>
      <span class="spacer"></span>
      <button onclick={() => unsavedPrompt?.answer('cancel')}>Cancel</button>
      <button class="primary" onclick={() => unsavedPrompt?.answer('save')}>Save…</button>
    {/snippet}
  </Modal>
{/if}
