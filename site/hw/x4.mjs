#!/usr/bin/env node
// Runs the web installer's own flasher (site/assets/flasher.js + the vendored esptool-js) against a
// real X4 over USB, so an install, a rollback, and a backup are proven on hardware before the site
// ships. Same code path as the browser; only Web Serial is swapped for node-serialport.
//
//   cd site/hw && npm ci
//   node x4.mjs probe
//   node x4.mjs install <firmware.bin | https://…/crossx-x4.bin> [--reset]
//   node x4.mjs rollback
//   node x4.mjs backup <out.bin>
//   node x4.mjs monitor [seconds]
import { readFile, writeFile } from 'node:fs/promises';
import { SerialPort } from 'serialport';

import { X4Flasher, parseOtadata, parsePartitionTable, extractLayout } from '../assets/flasher.js';
import { NodeSerialPort } from './webserial-shim.mjs';

const args = process.argv.slice(2);
const flag = (name) => {
  const i = args.indexOf(name);
  if (i < 0) return null;
  const [, value] = args.splice(i, 2);
  return value ?? true;
};
const portArg = flag('--port');
const reset = args.includes('--reset') && args.splice(args.indexOf('--reset'), 1);
const verbose = args.includes('--verbose') && args.splice(args.indexOf('--verbose'), 1);
const [command, target] = args;

const t0 = Date.now();
const say = (msg) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1).padStart(5)}s] ${msg}`);

// A hung serial session should fail the run, not wedge it.
setTimeout(() => {
  say('FAILED: no result after 30 minutes');
  process.exit(3);
}, 30 * 60 * 1000).unref();
async function findPort() {
  if (portArg) return portArg;
  const ports = await SerialPort.list();
  const x4 = ports.find((p) => p.vendorId?.toLowerCase() === '303a' && p.productId?.toLowerCase() === '1001');
  if (!x4) throw new Error('No Espressif USB-Serial-JTAG device (303a:1001) found. Is the X4 plugged in with a data cable?');
  // macOS lists /dev/tty.*; the call-out device is the one to open.
  return x4.path.replace('/dev/tty.', '/dev/cu.');
}

function reporter() {
  let lastLabel = '';
  let lastPct = -1;
  return {
    onStep: (_i, name, status) => say(`${status === 'running' ? '…' : status === 'done' ? '✓' : '✗'} ${name}`),
    onProgress: (label, current, total) => {
      const pct = Math.floor((current / total) * 100);
      if (label !== lastLabel || pct >= lastPct + 10 || pct === 100) {
        if (pct !== lastPct || label !== lastLabel) say(`  ${label} ${pct}%`);
        lastLabel = label;
        lastPct = pct;
      }
    },
  };
}

async function loadFirmware(source) {
  if (/^https?:\/\//.test(source)) {
    const response = await fetch(source);
    if (!response.ok) throw new Error(`Download failed: HTTP ${response.status}`);
    return new Uint8Array(await response.arrayBuffer());
  }
  return new Uint8Array(await readFile(source));
}

// With --reset, pulses EN through RTS first so the log starts at the boot banner.
async function monitor(path, seconds, withReset) {
  const port = new SerialPort({ path, baudRate: 115200, hupcl: false });
  await new Promise((resolve) => port.on('open', resolve));
  port.on('data', (chunk) => process.stdout.write(chunk.toString('latin1')));
  if (withReset) {
    const set = (signals) => new Promise((resolve) => port.set(signals, () => resolve()));
    await set({ dtr: false, rts: true });
    await new Promise((resolve) => setTimeout(resolve, 100));
    await set({ dtr: false, rts: false });
  }
  await new Promise((resolve) => setTimeout(resolve, seconds * 1000));
  await new Promise((resolve) => port.close(() => resolve()));
  process.stdout.write('\n');
}

async function main() {
  const path = await findPort();
  say(`X4 on ${path}`);
  const port = new NodeSerialPort(path);
  const flasher = new X4Flasher(port, { log: verbose ? (line) => say(`  esptool: ${line.trim()}`) : null });
  const r = reporter();

  switch (command) {
    case 'probe': {
      await flasher.connect();
      say(`chip ${flasher.loader.chip.CHIP_NAME}`);
      const partitions = parsePartitionTable(await flasher.loader.readFlash(0x8000, 0x1000));
      for (const p of partitions) say(`  ${p.type.padEnd(14)} 0x${p.offset.toString(16).padStart(6, '0')} 0x${p.size.toString(16)}`);
      flasher.layout = extractLayout(partitions);
      const { raw } = await flasher.readOtadata();
      const ota = parseOtadata(raw);
      say(`boots app${ota.activeApp} (seq ${ota.activeSeq}); an install would write app${ota.inactiveApp}`);
      await flasher.disconnect({ reset: Boolean(reset) });
      return;
    }
    case 'install': {
      if (!target) throw new Error('install needs a firmware path or URL');
      const firmware = await loadFirmware(target);
      say(`firmware ${firmware.length} bytes`);
      const result = await flasher.install(firmware, r);
      say(`installed into ${result.slot}`);
      if (reset) await rebootIntoApp(path);
      return;
    }
    case 'rollback': {
      const result = await flasher.rollback(r);
      say(`booting ${result.slot}`);
      return;
    }
    case 'backup': {
      if (!target) throw new Error('backup needs an output path');
      const image = await flasher.backup(r);
      await writeFile(target, image);
      say(`saved ${image.length} bytes to ${target}`);
      return;
    }
    case 'monitor':
      await monitor(path, Number(target ?? 10), Boolean(reset));
      return;
    default:
      throw new Error('usage: x4.mjs probe|install <bin|url> [--reset]|rollback|backup <out>|monitor [s] [--port p] [--verbose]');
  }
}

// After an install the web page leaves the chip in the flasher stub for the user to reset; here a
// second connection pulses EN so the new firmware boots without touching the device.
async function rebootIntoApp(path) {
  const flasher = new X4Flasher(new NodeSerialPort(path));
  await flasher.connect();
  await flasher.disconnect({ reset: true });
  say('reset; the new firmware is booting');
}

main().then(
  () => process.exit(0),
  (err) => {
    say(`FAILED: ${err?.message || err}`);
    process.exit(1);
  },
);
