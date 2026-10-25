// Types for wozzle.mjs. See that file for what each call does.

export type FsName = 'dos33' | 'prodos' | 'pascal';

export const FS_UNKNOWN: 0;
export const FS_DOS33: 1;
export const FS_PRODOS: 2;
export const FS_PASCAL: 3;

export const MSG_DEBUG: 0;
export const MSG_INFO: 1;
export const MSG_WARNING: 2;
export const MSG_ERROR: 3;

export type { WozMessage, CopyStats } from './wozerror.mjs';
import type { WozMessage, CopyStats } from './wozerror.mjs';

export { WozError } from './wozerror.mjs';

export interface WozEntry {
  name: string;
  // ProDOS file type; DOS 3.3 types mapped onto ProDOS ones; Pascal's
  // file kind on Pascal
  fileType: number;
  auxType: number;
  // Length of the contents, as read() returns them
  length: number;
  // Blocks (sectors on DOS 3.3)
  unitsUsed: number;
  access: number;
  isDirectory: boolean;
  locked: boolean;
  hasResourceFork: boolean;
  created: Date | null;
  modified: Date | null;
}

export interface VolumeInfo {
  fsType: FsName | null;
  volumeName: string;
  dosVolume: number;
  unitSize: number;
  totalUnits: number;
  freeUnits: number;
  isHardDisk: boolean;
  writeProtected: boolean;
  wozVersion: number;
  diskType: number;
  creator: string;
}

export interface BlankOptions {
  name: string;
  fsType: FsName;
  volumeName?: string | null;
  dosVolume?: number;
  blocks?: number;
  bootable?: boolean;
}

export interface CopyOptions {
  paths?: string[];
  destDir?: string | null;
  force?: boolean;
}

export class WozImage {
  readonly fsType: FsName | null;
  probe(): boolean;
  isDirty(): boolean;
  volumeInfo(): VolumeInfo;
  list(dir?: string): WozEntry[];
  stat(path: string): WozEntry;
  read(path: string, opts?: { resourceFork?: boolean; raw?: boolean }): Uint8Array;
  listBasic(path: string): string;
  write(path: string, bytes: Uint8Array,
        opts?: { fileType?: number; auxType?: number; tokenize?: boolean }): void;
  delete(path: string, opts?: { recursive?: boolean }): void;
  removeDirectory(path: string): void;
  makeDirectory(path: string): void;
  renameVolume(name: string): void;
  installBootBlocks(donorBytes?: Uint8Array | null, donorName?: string): void;
  krunch(afterFile?: string | null): void;
  resize(blocks: number, opts?: { relocate?: boolean }): void;
  save(name?: string): Uint8Array;
  close(): void;
}

export class Wozzle {
  messages: WozMessage[];
  detect(bytes: Uint8Array, name: string): FsName | null;
  open(bytes: Uint8Array, name: string, fsType?: FsName): WozImage;
  createBlank(opts: BlankOptions): Uint8Array;
  convert(bytes: Uint8Array, fromName: string, toName: string): Uint8Array;
  parseSize(token: string): number;
  copy(src: WozImage, dst: WozImage, opts?: CopyOptions): CopyStats;
}

export function loadWozzle(moduleOptions?: object): Promise<Wozzle>;
