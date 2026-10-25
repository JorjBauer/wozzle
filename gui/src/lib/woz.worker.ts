// The library's worker: everything the app asks of wozzle runs here, off
// the window's main thread
import { serveWozzle } from '@wasm/wozzle-worker.mjs';

serveWozzle();
