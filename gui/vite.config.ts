import { svelte } from '@sveltejs/vite-plugin-svelte'
import { defineConfig } from 'vite'
import { fileURLToPath } from 'node:url'

// The library's WebAssembly build lives outside the app, in ../wasm
const wasmDir = fileURLToPath(new URL('../wasm', import.meta.url))

export default defineConfig({
  plugins: [svelte()],
  resolve: {
    alias: { '@wasm': wasmDir },
  },
  // Tauri expects a fixed port, and its own output shouldn't be cleared
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
    fs: { allow: ['.', wasmDir] },
  },
  // Built files are transient, so they go in build/ with Rust's; dist/ is
  // for the finished app
  build: {
    target: 'es2022',
    outDir: 'build/web',
    emptyOutDir: true,
  },
  // The library's worker imports the Emscripten module, which needs an ES
  // module worker rather than the default classic one
  worker: {
    format: 'es',
  },
})
