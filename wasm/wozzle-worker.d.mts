// Types for wozzle-worker.mjs

export interface MessageEndpoint {
  addEventListener(type: 'message', listener: (e: { data: any }) => void): void;
  postMessage(message: any, transfer?: Transferable[]): void;
  start?(): void;
}

export function serveWozzle(port?: MessageEndpoint, moduleOptions?: object): void;
