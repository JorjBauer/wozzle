// The error every wozzle call throws, in a module of its own so code on
// the far side of a worker (wozzle-remote.mjs) can use it without loading
// the library.

export class WozError extends Error {
  constructor(message, messages) {
    super(message || 'wozzle: the operation failed');
    this.name = 'WozError';
    this.messages = messages;
  }
}
