// Tests for running the library in a worker (wozzle-worker.mjs) and
// talking to it from outside (wozzle-remote.mjs), the way the GUI does.
// Node's worker_threads stand in for a browser's Web Worker.
//
//   node wasm/test-remote.mjs

import { Worker, isMainThread, parentPort } from 'node:worker_threads';

if (!isMainThread) {
  const { serveWozzle } = await import('./wozzle-worker.mjs');
  serveWozzle(parentPort);
} else {
  await main();
}

async function main() {
  const { connectWozzle, WozError } = await import('./wozzle-remote.mjs');
  const { WozError: LibWozError } = await import('./wozzle.mjs');

  let passed = 0;
  let failed = 0;
  const check = (ok, name) => {
    process.stderr.write(`  ${ok ? 'PASS' : 'FAIL'}: ${name}\n`);
    if (ok) passed++; else failed++;
  };
  const rejects = async (p, needle, name) => {
    try {
      await p;
      check(false, name);
    } catch (e) {
      check(e instanceof WozError && e.message.includes(needle), name);
    }
  };

  const woz = connectWozzle(new Worker(new URL(import.meta.url)));
  try {
    check(WozError === LibWozError, 'the remote and the library share one WozError');

    const po = await woz.createBlank({ name: 'new.po', fsType: 'prodos', volumeName: 'REMOTE' });
    check(po instanceof Uint8Array && po.length === 280 * 512, 'a blank floppy comes back as bytes');
    check(await woz.detect(po, 'new.po') === 'prodos', 'and detects as ProDOS');
    await rejects(woz.createBlank({ name: 'x.po', fsType: 'prodos', volumeName: 'bad name!' }),
                  'bad volume name', 'a library failure rejects with the reason');
    check(woz.messages.some((m) => m.text.includes('bad volume name')),
          'and the messages come back too');

    const img = await woz.open(po, 'new.po');
    check(po.length === 280 * 512, "what went in is still the caller's");
    check(img.fsType === 'prodos', 'the image opens as ProDOS');

    const bin = new Uint8Array(1000).map((_, i) => i & 0xff);
    await img.makeDirectory('SUB');
    await img.write('SUB/DATA', bin, { fileType: 0x06, auxType: 0x0800 });
    check(bin.length === 1000 && bin[999] === (999 & 0xff), 'written bytes stay usable');
    const sub = await img.list('SUB');
    check(sub.length === 1 && sub[0].name === 'DATA' && sub[0].auxType === 0x0800,
          'SUB/DATA lists, with its aux type');
    check(sub[0].modified instanceof Date &&
          Math.abs(Date.now() - sub[0].modified.getTime()) < 120000, 'dates arrive as dates');
    const back = await img.read('SUB/DATA');
    check(back.length === 1000 && back.every((v, i) => v === (i & 0xff)), 'and it reads back');
    check(await img.isDirty(), 'the image is dirty');

    // Calls made together run in the order they were made
    const [, , names] = await Promise.all([
      img.write('A1', bin), img.delete('A1'), img.list(),
    ]);
    check(names.map((e) => e.name).join() === 'SUB', 'queued calls run in order');

    await rejects(img.read('NOPE'), 'NOPE', 'reading a missing file rejects');
    try {
      await img.call('takeString', 0);
      check(false, 'plumbing is refused with a TypeError');
    } catch (e) {
      check(e instanceof TypeError && /no image method/.test(e.message),
            'plumbing is refused with a TypeError');
    }

    const woz2 = await img.save('copy.woz');
    check(await woz.detect(woz2, 'copy.woz') === 'prodos', 'Save As .woz comes back as a WOZ');
    await img.close();
    await rejects(img.list(), 'closed', 'a closed image rejects');

    // A full-size hard disk, with the page's timers still running while
    // the worker is busy with it
    const blocks = await woz.parseSize('32m');
    const hd = await woz.createBlank({ name: 'big.hdv', fsType: 'prodos', volumeName: 'BIG', blocks });
    check(hd.length === blocks * 512, '32MB image made');
    const big = await woz.open(hd, 'big.hdv');
    const meg = new Uint8Array(1 << 20).map((_, i) => (i * 7) & 0xff);
    let ticks = 0;
    const timer = setInterval(() => ticks++, 5);
    const writes = [];
    for (let i = 0; i < 20; i++) writes.push(big.write(`F${i}`, meg));
    await Promise.all(writes);
    clearInterval(timer);
    check(ticks > 5, `the caller kept running while 20MB went in (${ticks} ticks)`);
    const vi = await big.volumeInfo();
    check(vi.totalUnits === blocks && vi.freeUnits < blocks - 20 * 2048, 'and the space is used');
    const saved = await big.save();
    await big.close();
    const again = await woz.open(saved, 'big.hdv');
    const f19 = await again.read('F19');
    check(f19.length === meg.length && f19.every((v, i) => v === meg[i]),
          'it saves, reopens, and reads back');
    await again.close();
  } catch (e) {
    check(false, `unexpected ${e?.name}: ${e?.message}`);
  } finally {
    woz.terminate();
  }

  process.stderr.write(`  ${passed} passed, ${failed} failed\n`);
  process.exit(failed ? 1 : 0);
}
