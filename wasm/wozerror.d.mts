// Types for wozerror.mjs

export interface WozMessage {
  level: number;
  text: string;
}

export interface CopyStats {
  files: number;
  dirsMade: number;
  bytes: number;
  collisions: number;
}

export class WozError extends Error {
  constructor(message: string, messages: WozMessage[]);
  messages: WozMessage[];
  // Set when a copy fails, so the caller can see what collided
  stats?: CopyStats & { ok: boolean };
}
