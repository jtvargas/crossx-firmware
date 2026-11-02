/**
 * CrossX web flasher: WebSerial + esptool-js OTA install for the XTEINK X4.
 *
 * Adapted from CrossPoint Tools (src/lib/flasher.js, MIT, (c) 2025 SoFriendly,
 * https://github.com/crosspoint-reader/crosspoint-tools), which is itself based
 * on xteink-flasher (MIT). The install flow is the same as crosspointreader.com:
 * read the device's partition table, write the image into the OTA slot that is
 * NOT running, then point otadata at it. The running firmware stays intact in
 * the other slot, so a bad image can be rolled back with one otadata write.
 *
 * Pure helpers are exported for the unit tests (site/test/flasher.test.mjs).
 */

let ESPLoader;
let Transport;

async function loadEsptool() {
  if (ESPLoader) return;
  const mod = await import('./vendor/esptool-js/bundle.js');
  ESPLoader = mod.ESPLoader;
  Transport = mod.Transport;
}

// Espressif USB-Serial-JTAG, the port the X4's ESP32-C3 enumerates as.
export const X4_PORT_FILTERS = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }];
export const FLASH_BYTES = 0x1000000;

// --- Bytes, CRC32, MD5 ---

const CRC32_TABLE = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
  let crc = i;
  for (let j = 0; j < 8; j++) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
  CRC32_TABLE[i] = crc >>> 0;
}

function crc32(data, previous = 0) {
  let crc = previous === 0 ? 0 : (previous ^ 0xffffffff) >>> 0;
  for (let i = 0; i < data.length; i++) crc = CRC32_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function u32le(value) {
  return new Uint8Array([value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff]);
}

function readU32le(bytes, offset = 0) {
  return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)) >>> 0;
}

function equalBytes(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

// ESP-IDF stores crc32_le(UINT32_MAX, ota_seq, 4) in each otadata entry.
function otaSeqCrc(sequence) {
  return u32le(crc32(u32le(sequence), 0xffffffff));
}

// WebCrypto has no MD5; esptool's flash verify and the full-flash backup need it.
const MD5_K = new Uint32Array(64);
for (let i = 0; i < 64; i++) MD5_K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32);
const MD5_S = [
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

export function md5(input) {
  const bitLen = input.length * 8;
  const paddedLen = (Math.floor((input.length + 8) / 64) + 1) * 64;
  const msg = new Uint8Array(paddedLen);
  msg.set(input);
  msg[input.length] = 0x80;
  msg.set(u32le(bitLen >>> 0), paddedLen - 8);
  msg.set(u32le(Math.floor(bitLen / 2 ** 32)), paddedLen - 4);

  let a0 = 0x67452301;
  let b0 = 0xefcdab89;
  let c0 = 0x98badcfe;
  let d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let chunk = 0; chunk < paddedLen; chunk += 64) {
    for (let i = 0; i < 16; i++) M[i] = readU32le(msg, chunk + i * 4);
    let A = a0;
    let B = b0;
    let C = c0;
    let D = d0;
    for (let i = 0; i < 64; i++) {
      let F;
      let g;
      if (i < 16) {
        F = (B & C) | (~B & D);
        g = i;
      } else if (i < 32) {
        F = (D & B) | (~D & C);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        F = B ^ C ^ D;
        g = (3 * i + 5) % 16;
      } else {
        F = C ^ (B | ~D);
        g = (7 * i) % 16;
      }
      F = (F + A + MD5_K[i] + M[g]) >>> 0;
      A = D;
      D = C;
      C = B;
      B = (B + ((F << MD5_S[i]) | (F >>> (32 - MD5_S[i])))) >>> 0;
    }
    a0 = (a0 + A) >>> 0;
    b0 = (b0 + B) >>> 0;
    c0 = (c0 + C) >>> 0;
    d0 = (d0 + D) >>> 0;
  }
  const out = new Uint8Array(16);
  out.set(u32le(a0), 0);
  out.set(u32le(b0), 4);
  out.set(u32le(c0), 8);
  out.set(u32le(d0), 12);
  return out;
}

export function toHex(bytes) {
  let out = '';
  for (const b of bytes) out += b.toString(16).padStart(2, '0');
  return out;
}

export async function sha256Hex(data) {
  return toHex(new Uint8Array(await crypto.subtle.digest('SHA-256', data)));
}

// --- Firmware image validation ---

const ESP_IMAGE_MAGIC = 0xe9;
const IMG_HEADER_SIZE = 24;
const IMG_SEG_HEADER_SIZE = 8;
const IMG_SHA_TRAILER = 32;
const IMG_CHECKSUM_SEED = 0xef;
const IMG_HASH_APPENDED_OFFSET = 23;
// esp_chip_id_t in the extended header (bytes 12-13). 0xFFFF on legacy images means "any".
const ESP_IMAGE_CHIP_IDS = { 0x0000: 'ESP32', 0x0002: 'ESP32-S2', 0x0005: 'ESP32-C3', 0x0009: 'ESP32-S3' };

export function imageChipName(data) {
  if (data.length < 14) return null;
  return ESP_IMAGE_CHIP_IDS[data[12] | (data[13] << 8)] ?? null;
}

// Walks the ESP app image (header, segments, XOR checksum, optional SHA-256 trailer) so an HTML
// error page, a truncated download, or a non-app .bin never reaches flash. Bounds use the
// headroom form (`total - pos < n`) so a hostile segment length cannot wrap past the check.
export async function validateFirmwareImage(data, { expectedChip = null } = {}) {
  const total = data.length;
  if (total < IMG_HEADER_SIZE) throw new Error('Firmware is too small: the image header is truncated.');
  if (data[0] !== ESP_IMAGE_MAGIC) {
    throw new Error('This is not an ESP32 firmware image (magic byte 0xE9 missing).');
  }
  const chip = imageChipName(data);
  if (expectedChip && chip && chip !== expectedChip) {
    throw new Error(`This firmware is built for the ${chip}, but the X4 uses an ${expectedChip}.`);
  }
  const segments = data[1];
  const hashAppended = (data[IMG_HASH_APPENDED_OFFSET] & 0x01) !== 0;
  let xor = IMG_CHECKSUM_SEED;
  let pos = IMG_HEADER_SIZE;
  for (let i = 0; i < segments; i++) {
    if (total - pos < IMG_SEG_HEADER_SIZE) throw new Error('Invalid firmware: a segment header runs past the end.');
    const length = readU32le(data, pos + 4);
    pos += IMG_SEG_HEADER_SIZE;
    if (length > total - pos) throw new Error('Invalid firmware: segment data runs past the end.');
    const end = pos + length;
    for (let j = pos; j < end; j++) xor ^= data[j];
    pos = end;
  }
  const padEnd = (pos + 16) & ~15;
  const expectedTotal = padEnd + (hashAppended ? IMG_SHA_TRAILER : 0);
  if (expectedTotal !== total) {
    throw new Error(`Invalid firmware: the image declares ${expectedTotal} bytes but the file has ${total}.`);
  }
  if ((xor & 0xff) !== data[padEnd - 1]) throw new Error('Invalid firmware: segment checksum mismatch.');
  if (hashAppended) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', data.subarray(0, total - IMG_SHA_TRAILER)));
    if (!equalBytes(digest, data.subarray(total - IMG_SHA_TRAILER))) {
      throw new Error('Invalid firmware: SHA-256 trailer mismatch. The file is corrupt or truncated.');
    }
  }
}

// --- Partition table and otadata ---

const PARTITION_TYPES = {
  0x00: { 0x00: 'app-factory', 0x10: 'app-ota_0', 0x11: 'app-ota_1' },
  0x01: { 0x00: 'data-ota', 0x01: 'data-phy', 0x02: 'data-nvs', 0x03: 'data-coredump', 0x82: 'data-spiffs' },
};

export function parsePartitionTable(data) {
  const partitions = [];
  for (let offset = 0; offset + 32 <= data.length; offset += 32) {
    const row = data.subarray(offset, offset + 32);
    if (row.every((b) => b === 0xff)) break;
    if (row[0] === 0xeb && row[1] === 0xeb) continue; // MD5 checksum row
    partitions.push({
      type: PARTITION_TYPES[row[2]]?.[row[3]] ?? 'unknown',
      offset: readU32le(row, 4),
      size: readU32le(row, 8),
    });
  }
  return partitions;
}

// 0x0..0x9000 holds the bootloader and the partition table itself; nothing may be written there,
// and nothing may wrap uint32 or run past the 16 MB flash.
function assertSafeRange(p) {
  const end = (p.offset + p.size) >>> 0;
  if (p.offset < 0x9000 || end < p.offset || end > FLASH_BYTES) {
    throw new Error(`Partition ${p.type} (0x${p.offset.toString(16)}..0x${end.toString(16)}) is outside the safe flash window.`);
  }
}

const OTA_SECTOR_BYTES = 0x1000;
const OTADATA_BYTES = 2 * OTA_SECTOR_BYTES;
const OTA_STATE_NEW = 0;
const OTA_INVALID_STATES = new Set([3, 4]); // INVALID, ABORTED

export function extractLayout(partitions) {
  const otadata = partitions.find((p) => p.type === 'data-ota');
  const app0 = partitions.find((p) => p.type === 'app-ota_0');
  const app1 = partitions.find((p) => p.type === 'app-ota_1');
  if (!otadata) throw new Error('The partition table has no otadata partition.');
  if (!app0 || !app1) throw new Error('The partition table is missing an OTA app slot.');
  if (otadata.size < OTADATA_BYTES) throw new Error(`otadata is too small (${otadata.size} bytes).`);
  [otadata, app0, app1].forEach(assertSafeRange);
  return {
    otadataOffset: otadata.offset,
    appSlots: [
      { offset: app0.offset, size: app0.size },
      { offset: app1.offset, size: app1.size },
    ],
  };
}

function parseOtaEntry(data, offset) {
  const sequence = readU32le(data, offset);
  return {
    sequence,
    state: readU32le(data, offset + 0x18),
    crcValid: equalBytes(data.subarray(offset + 0x1c, offset + 0x20), otaSeqCrc(sequence)),
  };
}

// IDF model: the active app is (seq - 1) % 2 of the newest eligible entry. The new entry goes
// into the OTHER otadata sector so a power cut mid-write leaves the old pointer as a fallback.
export function parseOtadata(data) {
  const slot0 = parseOtaEntry(data, 0);
  const slot1 = parseOtaEntry(data, OTA_SECTOR_BYTES);
  const eligible = [slot0, slot1]
    .map((s, sector) => ({ ...s, sector }))
    .filter((s) => s.sequence !== 0xffffffff && s.crcValid && !OTA_INVALID_STATES.has(s.state))
    .sort((a, b) => b.sequence - a.sequence);

  const active = eligible[0] ?? null;
  const activeSeq = active ? active.sequence : 0;
  const activeApp = active ? (activeSeq - 1) % 2 : 0;
  const inactiveApp = 1 - activeApp;
  // Smallest seq above activeSeq that selects inactiveApp (blank otadata needs 2, not 1).
  let newSeq = activeSeq + 1;
  while ((newSeq - 1) % 2 !== inactiveApp) newSeq++;
  const targetSector = active ? 1 - active.sector : 0;
  return { slot0, slot1, activeApp, inactiveApp, activeSeq, newSeq, targetSector };
}

// State NEW keeps the bootloader's rollback path on rollback-enabled boards; the X4's bootloader
// ignores ota_state, so this is a no-op there.
export function buildOtadataSector(existingSector, newSeq) {
  const sector = new Uint8Array(existingSector);
  sector.set(u32le(newSeq), 0);
  sector.set(u32le(OTA_STATE_NEW), 0x18);
  sector.set(otaSeqCrc(newSeq), 0x1c);
  return sector;
}

function assertOtaSwitched(ota, expectedApp, expectedSeq) {
  if (ota.activeSeq !== expectedSeq || ota.activeApp !== expectedApp) {
    throw new Error(
      `The boot selector did not verify after writing (expected app${expectedApp} via seq ${expectedSeq}, ` +
        `read app${ota.activeApp} via seq ${ota.activeSeq}). Nothing will boot differently; try again.`,
    );
  }
}

// --- Device session ---

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class X4Flasher {
  // log receives esptool-js's own progress lines (chip, stub, MD5 checks) for the page's log view.
  constructor(port, { expectedChip = 'ESP32-C3', log = null } = {}) {
    this.port = port;
    this.expectedChip = expectedChip;
    this.log = log;
    this.loader = null;
    this.layout = null;
  }

  static serialSupported() {
    return typeof navigator !== 'undefined' && typeof navigator.serial?.requestPort === 'function';
  }

  // Must run inside the click handler before any other await, or the browser rejects the
  // picker for lack of a user gesture.
  static requestPort() {
    if (!X4Flasher.serialSupported()) throw new Error('WebSerial is not available. Use Chrome or Edge on a computer.');
    return navigator.serial.requestPort({ filters: X4_PORT_FILTERS });
  }

  // A running reader can still be printing its serial log when esptool resets it into the
  // bootloader; those bytes can break the first sync ("Invalid head of packet"). The reset that
  // the second attempt starts with leaves a quiet ROM, so one retry is enough.
  async connect() {
    await loadEsptool();
    for (let attempt = 1; ; attempt++) {
      try {
        await this.connectOnce();
        break;
      } catch (err) {
        await this.disconnect({ reset: false }).catch(() => {});
        if (attempt >= 2) throw err;
        this.log?.(`Connect failed (${err?.message || err}); retrying`);
        await delay(500);
      }
    }
    const chip = this.loader.chip?.CHIP_NAME;
    if (this.expectedChip && chip && chip !== this.expectedChip) {
      await this.disconnect({ reset: false }).catch(() => {});
      throw new Error(`The connected device is an ${chip}, not the X4's ${this.expectedChip}. Nothing was written.`);
    }
  }

  async connectOnce() {
    const transport = new Transport(this.port, false);
    // 0.5.x hex-dumps every received packet to the console regardless of `tracing`; on a 16 MB
    // backup that formatting dominates the read time.
    transport.trace = () => {};
    const log = (line) => this.log?.(String(line));
    const terminal = { clean() {}, writeLine: log, write: log };
    this.loader = new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, terminal, enableTracing: false });
    await this.loader.main();
  }

  // reset=false leaves the chip in the flasher stub; the user power-cycles it, exactly as
  // crosspointreader.com does for X4 installs. reset=true pulses EN through RTS.
  async disconnect({ reset }) {
    const loader = this.loader;
    if (!loader) return;
    this.loader = null;
    const transport = loader.transport;
    try {
      await transport.setDTR(false).catch(() => {});
      if (reset) {
        await transport.setRTS(true);
        await delay(100);
        await loader.after('hard_reset');
      } else {
        await loader.after('no_reset_stub');
      }
    } finally {
      // Clear both lines before closing so the OS release does not glitch EN/IO0 into download
      // mode, and always close the port so a retry can reopen it.
      await transport.setDTR(false).catch(() => {});
      await transport.setRTS(false).catch(() => {});
      await delay(100);
      await transport.disconnect().catch(() => {});
    }
  }

  async readLayout() {
    this.layout = extractLayout(parsePartitionTable(await this.loader.readFlash(0x8000, 0x1000)));
    return this.layout;
  }

  async readOtadata() {
    const raw = await this.loader.readFlash(this.layout.otadataOffset, OTADATA_BYTES);
    return { raw, ota: parseOtadata(raw) };
  }

  // esptool-js 0.5.x (the release CrossPoint ships) takes images as binary strings.
  async write(data, address, label, onProgress) {
    const loader = this.loader;
    await loader.writeFlash({
      fileArray: [{ data: loader.ui8ToBstr(data), address }],
      flashSize: 'keep',
      flashMode: 'keep',
      flashFreq: 'keep',
      eraseAll: false,
      compress: true,
      // esptool compares this against the chip's own MD5 of the written range.
      calculateMD5Hash: (image) => toHex(md5(loader.bstrToUi8(image))),
      reportProgress: (_, written, total) => onProgress?.(label, written, total),
    });
  }

  async pointBootAt(otaRaw, ota, onProgress) {
    const start = ota.targetSector * OTA_SECTOR_BYTES;
    const sector = buildOtadataSector(otaRaw.subarray(start, start + OTA_SECTOR_BYTES), ota.newSeq);
    await this.write(sector, this.layout.otadataOffset + start, 'Update boot partition', onProgress);
    const verify = await this.readOtadata();
    assertOtaSwitched(verify.ota, ota.inactiveApp, ota.newSeq);
  }

  // Runs one device operation; on failure the port is released (no reset, nothing else
  // written) so the user can retry without replugging.
  async session(names, onStep, operation) {
    const step = runSteps(names, onStep);
    try {
      return await operation(step);
    } catch (err) {
      await this.disconnect({ reset: false }).catch(() => {});
      throw err;
    }
  }

  static installSteps() {
    return ['Connect to the X4', 'Read partition table', 'Read boot selector', 'Write firmware', 'Switch boot partition', 'Disconnect'];
  }

  // Image into the idle OTA slot, then otadata. A failure before the otadata write leaves the
  // device booting exactly what it booted before.
  async install(firmware, { onStep, onProgress } = {}) {
    await validateFirmwareImage(firmware, { expectedChip: this.expectedChip });
    return this.session(X4Flasher.installSteps(), onStep, async (step) => {
      await step(0, () => this.connect());
      await step(1, () => this.readLayout());
      const { raw, ota } = await step(2, () => this.readOtadata());
      await step(3, async () => {
        const slot = this.layout.appSlots[ota.inactiveApp];
        if (firmware.length > slot.size) {
          throw new Error(`Firmware is ${firmware.length} bytes but app${ota.inactiveApp} holds ${slot.size}.`);
        }
        await this.write(firmware, slot.offset, 'Write firmware', onProgress);
      });
      await step(4, () => this.pointBootAt(raw, ota, onProgress));
      await step(5, () => this.disconnect({ reset: false }));
      return { slot: `app${ota.inactiveApp}` };
    });
  }

  static rollbackSteps() {
    return ['Connect to the X4', 'Read partition table', 'Read boot selector', 'Switch boot partition', 'Restart'];
  }

  // Boots the other OTA slot: the firmware that ran before the last install.
  async rollback({ onStep, onProgress } = {}) {
    return this.session(X4Flasher.rollbackSteps(), onStep, async (step) => {
      await step(0, () => this.connect());
      await step(1, () => this.readLayout());
      const { raw, ota } = await step(2, async () => {
        const current = await this.readOtadata();
        if (current.ota.activeSeq === 0) throw new Error('This X4 has no previous firmware recorded.');
        const slot = this.layout.appSlots[current.ota.inactiveApp];
        const head = await this.loader.readFlash(slot.offset, 0x1000);
        if (head[0] !== ESP_IMAGE_MAGIC) throw new Error('The other app slot is empty; there is nothing to roll back to.');
        return current;
      });
      await step(3, () => this.pointBootAt(raw, ota, onProgress));
      await step(4, () => this.disconnect({ reset: true }));
      return { slot: `app${ota.inactiveApp}` };
    });
  }

  static backupSteps() {
    return ['Connect to the X4', 'Read 16 MB of flash', 'Disconnect'];
  }

  // 1 MB reads, each checked against the chip's MD5: one 16 MB read degrades quadratically in
  // esptool-js and dies on a read timeout.
  async backup({ onStep, onProgress } = {}) {
    return this.session(X4Flasher.backupSteps(), onStep, async (step) => {
      await step(0, () => this.connect());
      const image = await step(1, async () => {
        const out = new Uint8Array(FLASH_BYTES);
        const CHUNK = 0x100000;
        for (let offset = 0; offset < FLASH_BYTES; offset += CHUNK) {
          for (let attempt = 1; ; attempt++) {
            try {
              const chunk = await this.loader.readFlash(offset, CHUNK, (_, read) =>
                onProgress?.('Read flash', offset + read, FLASH_BYTES),
              );
              if (toHex(md5(chunk)) !== (await this.loader.flashMd5sum(offset, CHUNK))) {
                throw new Error(`MD5 mismatch reading flash at 0x${offset.toString(16)}.`);
              }
              out.set(chunk, offset);
              break;
            } catch (err) {
              if (attempt >= 3) throw err;
              // Let the stub finish streaming the aborted read, then drop it.
              await delay(500);
              await this.loader.transport.flushInput();
            }
          }
        }
        return out;
      });
      await step(2, () => this.disconnect({ reset: false }));
      return image;
    });
  }

  static restoreSteps() {
    return ['Connect to the X4', 'Write 16 MB of flash', 'Restart'];
  }

  async restore(image, { onStep, onProgress } = {}) {
    if (image.length !== FLASH_BYTES) throw new Error(`A full backup is exactly 16 MB; this file is ${image.length} bytes.`);
    if (image[0] !== ESP_IMAGE_MAGIC) throw new Error('This file does not start with a bootloader; it is not a full-flash backup.');
    if (imageChipName(image) && imageChipName(image) !== this.expectedChip) {
      throw new Error(`This backup's bootloader is for the ${imageChipName(image)}, not the X4's ${this.expectedChip}.`);
    }
    return this.session(X4Flasher.restoreSteps(), onStep, async (step) => {
      await step(0, () => this.connect());
      await step(1, () => this.write(image, 0, 'Write flash', onProgress));
      await step(2, () => this.disconnect({ reset: true }));
    });
  }
}

function runSteps(names, onStep) {
  return async (index, fn) => {
    onStep?.(index, names[index], 'running');
    try {
      const result = await fn();
      onStep?.(index, names[index], 'done');
      return result;
    } catch (err) {
      onStep?.(index, names[index], 'error');
      throw err;
    }
  };
}
