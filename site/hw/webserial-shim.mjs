// The slice of the Web Serial SerialPort API esptool-js uses, over node-serialport, so the
// installer's own flasher.js can drive a real X4 from the command line.
//
// Stream lifecycle follows the Web Serial spec: cancelling `readable` (or closing `writable`)
// drops it, and the next access creates a fresh, unlocked stream while the port stays open.
// esptool-js 0.5.x depends on that to get past its unlock wait in Transport.disconnect().
import { SerialPort as NodePort } from 'serialport';

export class NodeSerialPort {
  #node = null;
  #readable = null;
  #writable = null;
  #dtr = false;
  #rts = false;

  constructor(path) {
    this.path = path;
  }

  getInfo() {
    return { usbVendorId: 0x303a, usbProductId: 0x1001 };
  }

  get readable() {
    const node = this.#node;
    if (!node) return null;
    if (!this.#readable) {
      let onData;
      const stream = new ReadableStream({
        start: (controller) => {
          onData = (chunk) => controller.enqueue(new Uint8Array(chunk));
          node.on('data', onData);
        },
        cancel: () => {
          node.off('data', onData);
          if (this.#readable === stream) this.#readable = null;
        },
      });
      this.#readable = stream;
    }
    return this.#readable;
  }

  get writable() {
    const node = this.#node;
    if (!node) return null;
    if (!this.#writable) {
      const stream = new WritableStream({
        write: (chunk) =>
          new Promise((resolve, reject) =>
            node.write(Buffer.from(chunk), (err) => (err ? reject(err) : node.drain(() => resolve()))),
          ),
        close: () => {
          if (this.#writable === stream) this.#writable = null;
        },
        abort: () => {
          if (this.#writable === stream) this.#writable = null;
        },
      });
      this.#writable = stream;
    }
    return this.#writable;
  }

  async open({ baudRate }) {
    if (this.#node) throw new Error('InvalidStateError: the port is already open.');
    const node = new NodePort({ path: this.path, baudRate, autoOpen: false, hupcl: false });
    await new Promise((resolve, reject) => node.open((err) => (err ? reject(err) : resolve())));
    this.#node = node;
  }

  // node-serialport's set() turns every unspecified line on, so always send both.
  async setSignals({ dataTerminalReady, requestToSend }) {
    if (dataTerminalReady !== undefined) this.#dtr = dataTerminalReady;
    if (requestToSend !== undefined) this.#rts = requestToSend;
    await new Promise((resolve, reject) =>
      this.#node.set({ dtr: this.#dtr, rts: this.#rts }, (err) => (err ? reject(err) : resolve())),
    );
  }

  async close() {
    const node = this.#node;
    if (!node) return;
    this.#node = null;
    this.#readable = null;
    this.#writable = null;
    await new Promise((resolve) => node.close(() => resolve()));
  }
}
