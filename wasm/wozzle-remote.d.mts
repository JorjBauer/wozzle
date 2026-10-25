// Types for wozzle-remote.mjs: wozzle.mjs's API, with promises

import type {
  FsName, WozMessage, CopyStats, WozEntry, VolumeInfo, BlankOptions, CopyOptions,
} from './wozzle.mjs';

export { WozError } from './wozerror.mjs';

export class RemoteImage {
  readonly id: number;
  readonly fsType: FsName | null;
  probe(): Promise<boolean>;
  isDirty(): Promise<boolean>;
  volumeInfo(): Promise<VolumeInfo>;
  list(dir?: string): Promise<WozEntry[]>;
  stat(path: string): Promise<WozEntry>;
  read(path: string, opts?: { resourceFork?: boolean; raw?: boolean }): Promise<Uint8Array>;
  listBasic(path: string): Promise<string>;
  write(path: string, bytes: Uint8Array,
        opts?: { fileType?: number; auxType?: number; tokenize?: boolean }): Promise<void>;
  delete(path: string, opts?: { recursive?: boolean }): Promise<void>;
  removeDirectory(path: string): Promise<void>;
  makeDirectory(path: string): Promise<void>;
  renameVolume(name: string): Promise<void>;
  installBootBlocks(donorBytes?: Uint8Array | null, donorName?: string): Promise<void>;
  krunch(afterFile?: string | null): Promise<void>;
  resize(blocks: number, opts?: { relocate?: boolean }): Promise<void>;
  save(name?: string): Promise<Uint8Array>;
  close(): Promise<void>;
}

export class RemoteWozzle {
  messages: WozMessage[];
  detect(bytes: Uint8Array, name: string): Promise<FsName | null>;
  open(bytes: Uint8Array, name: string, fsType?: FsName): Promise<RemoteImage>;
  createBlank(opts: BlankOptions): Promise<Uint8Array>;
  convert(bytes: Uint8Array, fromName: string, toName: string): Promise<Uint8Array>;
  parseSize(token: string): Promise<number>;
  copy(src: RemoteImage, dst: RemoteImage, opts?: CopyOptions): Promise<CopyStats>;
  terminate(): void;
}

// A browser Worker, or a Node worker_threads Worker
export function connectWozzle(worker: Worker | object): RemoteWozzle;
