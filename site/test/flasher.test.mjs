// node --test site/test/*.test.mjs
// The OTA-slot and image checks decide where bytes land on a user's reader; a wrong answer
// overwrites the running firmware or boots an empty slot.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { test } from 'node:test';

import {
  buildOtadataSector,
  extractLayout,
  md5,
  parseOtadata,
  parsePartitionTable,
  toHex,
  validateFirmwareImage,
} from '../assets/flasher.js';

// --- otadata fixtures (ESP-IDF esp_ota_select_entry_t at the start of each 4 KB sector) ---

function blankOtadata() {
  return new Uint8Array(0x2000).fill(0xff);
}

function writeEntry(otadata, sector, seq, state = 2) {
  const fresh = buildOtadataSector(otadata.subarray(sector * 0x1000, (sector + 1) * 0x1000), seq);
  new DataView(fresh.buffer).setUint32(0x18, state, true);
  otadata.set(fresh, sector * 0x1000);
  return otadata;
}

test('blank otadata boots app0, so the install targets app1 with seq 2', () => {
  const ota = parseOtadata(blankOtadata());
  assert.equal(ota.activeApp, 0);
  assert.equal(ota.inactiveApp, 1);
  assert.equal(ota.newSeq, 2);
  assert.equal(ota.targetSector, 0);
});

test('the newest valid entry selects the running app; the new entry goes to the other sector', () => {
  const ota = parseOtadata(writeEntry(writeEntry(blankOtadata(), 0, 1), 1, 2));
  assert.equal(ota.activeSeq, 2);
  assert.equal(ota.activeApp, 1);
  assert.equal(ota.inactiveApp, 0);
  assert.equal(ota.newSeq, 3);
  assert.equal(ota.targetSector, 0);
});

test('sector index does not imply the app: seq 3 in sector 1 boots app0', () => {
  const ota = parseOtadata(writeEntry(writeEntry(blankOtadata(), 0, 2), 1, 3));
  assert.equal(ota.activeApp, 0);
  assert.equal(ota.newSeq, 4);
  assert.equal(ota.targetSector, 0);
});

test('entries with a bad CRC or an INVALID/ABORTED state are not eligible', () => {
  const corrupt = writeEntry(writeEntry(blankOtadata(), 0, 1), 1, 2);
  corrupt[0x1000 + 0x1c] ^= 0xff;
  assert.equal(parseOtadata(corrupt).activeSeq, 1);

  for (const state of [3, 4]) {
    const rejected = writeEntry(writeEntry(blankOtadata(), 0, 1), 1, 2, state);
    assert.equal(parseOtadata(rejected).activeSeq, 1, `state ${state}`);
  }
});

test('writing the computed entry switches boot to the idle slot and keeps the old pointer', () => {
  for (const otadata of [blankOtadata(), writeEntry(blankOtadata(), 0, 1), writeEntry(writeEntry(blankOtadata(), 0, 4), 1, 5)]) {
    const before = parseOtadata(otadata);
    const start = before.targetSector * 0x1000;
    const after = new Uint8Array(otadata);
    after.set(buildOtadataSector(otadata.subarray(start, start + 0x1000), before.newSeq), start);
    const parsed = parseOtadata(after);
    assert.equal(parsed.activeApp, before.inactiveApp);
    assert.equal(parsed.activeSeq, before.newSeq);
    // The sector that was active is untouched: a power cut mid-write still boots the old app.
    const otherStart = (1 - before.targetSector) * 0x1000;
    assert.deepEqual(after.subarray(otherStart, otherStart + 0x1000), otadata.subarray(otherStart, otherStart + 0x1000));
  }
});

// --- partition table ---

function partitionTable(rows) {
  const out = new Uint8Array(0x1000).fill(0xff);
  rows.forEach(([type, subtype, offset, size], i) => {
    const row = new DataView(out.buffer, i * 32, 32);
    row.setUint16(0, 0x50aa, true);
    row.setUint8(2, type);
    row.setUint8(3, subtype);
    row.setUint32(4, offset, true);
    row.setUint32(8, size, true);
    for (let j = 12; j < 32; j++) row.setUint8(j, 0);
  });
  return out;
}

const X4_ROWS = [
  [0x01, 0x02, 0x9000, 0x5000],
  [0x01, 0x00, 0xe000, 0x2000],
  [0x00, 0x10, 0x10000, 0x640000],
  [0x00, 0x11, 0x650000, 0x640000],
  [0x01, 0x82, 0xc90000, 0x360000],
  [0x01, 0x03, 0xff0000, 0x10000],
];

test('the X4 partition table yields otadata at 0xe000 and two 0x640000 app slots', () => {
  const layout = extractLayout(parsePartitionTable(partitionTable(X4_ROWS)));
  assert.equal(layout.otadataOffset, 0xe000);
  assert.deepEqual(layout.appSlots, [
    { offset: 0x10000, size: 0x640000 },
    { offset: 0x650000, size: 0x640000 },
  ]);
});

test('layouts that would write into the bootloader, wrap, or run past 16 MB are refused', () => {
  const below = X4_ROWS.map((r) => (r[1] === 0x10 ? [0x00, 0x10, 0x8000, 0x1000] : r));
  const wraps = X4_ROWS.map((r) => (r[1] === 0x11 ? [0x00, 0x11, 0x650000, 0xffffffff] : r));
  const past = X4_ROWS.map((r) => (r[1] === 0x11 ? [0x00, 0x11, 0xff0000, 0x20000] : r));
  for (const rows of [below, wraps, past]) {
    assert.throws(() => extractLayout(parsePartitionTable(partitionTable(rows))), /safe flash window/);
  }
  assert.throws(() => extractLayout(parsePartitionTable(partitionTable(X4_ROWS.filter((r) => r[1] !== 0x00)))), /otadata/);
});

// --- app image validation ---

function appImage({ chipId = 0x0005, hash = true, segment = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]) } = {}) {
  const header = new Uint8Array(24);
  header[0] = 0xe9;
  header[1] = 1;
  header[12] = chipId & 0xff;
  header[13] = chipId >> 8;
  header[23] = hash ? 1 : 0;
  const segHeader = new Uint8Array(8);
  new DataView(segHeader.buffer).setUint32(4, segment.length, true);
  const bodyLen = 24 + 8 + segment.length;
  const padEnd = (bodyLen + 16) & ~15;
  const image = new Uint8Array(padEnd + (hash ? 32 : 0));
  image.set(header, 0);
  image.set(segHeader, 24);
  image.set(segment, 32);
  let xor = 0xef;
  for (const b of segment) xor ^= b;
  image[padEnd - 1] = xor;
  if (hash) image.set(createHash('sha256').update(image.subarray(0, padEnd)).digest(), padEnd);
  return image;
}

test('a well-formed ESP32-C3 app image passes, with and without the SHA-256 trailer', async () => {
  await validateFirmwareImage(appImage(), { expectedChip: 'ESP32-C3' });
  await validateFirmwareImage(appImage({ hash: false }), { expectedChip: 'ESP32-C3' });
});

test('images that must never reach flash are rejected before connecting', async () => {
  const html = new TextEncoder().encode('<!doctype html><html>404</html>'.padEnd(64));
  await assert.rejects(validateFirmwareImage(html), /not an ESP32 firmware image/);
  await assert.rejects(validateFirmwareImage(appImage().subarray(0, 40)), /past the end|declares/);
  await assert.rejects(validateFirmwareImage(appImage({ chipId: 0x0009 }), { expectedChip: 'ESP32-C3' }), /ESP32-S3/);

  const flipped = appImage();
  flipped[33] ^= 0x01;
  await assert.rejects(validateFirmwareImage(flipped), /checksum|SHA-256/);

  const hostile = appImage();
  new DataView(hostile.buffer).setUint32(28, 0xffffffff, true);
  await assert.rejects(validateFirmwareImage(hostile), /past the end/);
});

test('md5 matches RFC 1321 vectors (esptool compares it with the chip after each write)', () => {
  const enc = (s) => new TextEncoder().encode(s);
  assert.equal(toHex(md5(enc(''))), 'd41d8cd98f00b204e9800998ecf8427e');
  assert.equal(toHex(md5(enc('abc'))), '900150983cd24fb0d6963f7d28e17f72');
  const big = new Uint8Array(100_000).map((_, i) => i & 0xff);
  assert.equal(toHex(md5(big)), createHash('md5').update(big).digest('hex'));
});
