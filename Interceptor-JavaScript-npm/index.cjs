'use strict';
const path = require('node:path');
const native = require(process.env.INTERCEPTOR_SDK_NODE_PATH || path.join(__dirname, 'prebuilds/win32-x64/interceptor_sdk_node.node'));
class Interceptor {
  #id; #ticket = 0n; #pending = false;
  constructor() { this.#id = native.open(); }
  #active() { if (!this.#id) throw new Error('Interceptor is closed'); return this.#id; }
  initialize() { return native.initialize(this.#active()); }
  info() { return native.info(this.#active()); }
  devices() { return native.devices(this.#active()); }
  async receive(timeout = null) {
    if (this.#pending) throw new Error('An input wait is already pending');
    if (timeout !== null && (!Number.isInteger(timeout) || timeout < 0 || timeout > 0xfffffffe)) throw new RangeError('Invalid timeout in milliseconds');
    const id = this.#active(); this.#pending = true;
    try {
      const result = await native.receive(id, timeout === null ? 0xffffffff : timeout);
      this.#ticket = result.ticket;
      const batch = result.events;
      Object.defineProperties(batch, { owner: { value: this }, ticket: { value: result.ticket } });
      return Object.freeze(batch);
    } catch (error) { if (!this.#id) return Object.freeze([]); throw error; } finally { this.#pending = false; }
  }
  #batch(inputs) {
    this.#active();
    if (inputs?.owner !== this || inputs.ticket !== this.#ticket) throw new Error('Stale or foreign input batch');
  }
  send(inputs) { this.#batch(inputs); return native.send(this.#id, inputs.ticket, inputs); }
  discard(inputs) { this.#batch(inputs); native.discard(this.#id, inputs.ticket); }
  inject(input) { return native.inject(this.#active(), { state: {}, ...input }); }
  descriptor(token, report = false) { return native.descriptor(this.#active(), token, report); }
  cancel() { native.cancel(this.#active()); }
  close() { if (this.#id) { const id=this.#id; this.#id=0n; native.close(id); } }
  [Symbol.dispose]() { this.close(); }
}
module.exports = { Interceptor };
