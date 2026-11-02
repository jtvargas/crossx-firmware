# Flashing and recovery

**Do not flash an experimental build to an X4 until you have a verified way to recover it.**

## Before the first flash

1. Identify the device. Note the model, firmware version shown in the stock settings, and purchase date. Some X4 batches ship **USB-locked**: the bootloader only accepts images signed or recognised by the vendor path, and a generic `esptool` flash can leave the device unbootable with no serial recovery.
2. Read CrossPoint's guides, which apply unchanged to this fork:
   - [Upstream README: install methods and the USB-locked warning](../upstream-crosspoint-readme.md)
   - [Fix a bricked Xteink](../fix-bricked-xteink.md)
   - [Troubleshooting](../troubleshooting.md)
3. Back up the whole flash, stock firmware included: [web installer](https://jtvargas.github.io/crossx-firmware/) → *Recovery → Back up flash* (about 25 minutes, every 1 MB chunk checked against the chip's MD5), or `esptool.py read_flash 0 0x1000000 stock-x4.bin`. Keep the SD card contents too.
4. Confirm that plain CrossPoint installs and boots on *this* unit first (the installer's *CrossPoint* build, or [crosspointreader.com](https://crosspointreader.com/#flash-tools)). If CrossPoint cannot be installed by a supported path, CrossX cannot either.
5. Only then install CrossX.

## Flashing

### Web installer (recommended)

[jtvargas.github.io/crossx-firmware](https://jtvargas.github.io/crossx-firmware/), in Chrome or Edge on a computer. The flow is CrossPoint's, step for step:

1. Read the partition table at `0x8000` and refuse anything that would write below `0x9000`, wrap, or run past 16 MB.
2. Read `otadata` and work out the running slot the way ESP-IDF does (`(seq - 1) % 2` of the newest valid entry).
3. Write the image into the **other** OTA slot. esptool checks the written range against the chip's MD5.
4. Write a new `otadata` entry into the other `otadata` sector and read it back. The old entry stays, so a power cut mid-write still boots the previous firmware.
5. Leave the chip without a serial reset; the user presses Reset, then holds power for 3–5 seconds.

Before any of that, the page checks the download against the size and SHA-256 in `manifest.json` and walks the ESP image (magic, chip id ESP32-C3, segment bounds, checksum, SHA-256 trailer). Nothing is written until those pass.

The published builds come from `.github/workflows/pages.yml`: after `CI (build)` succeeds on a push to `main`, it takes that run's `crossx_x4` and `default` artifacts, runs `scripts/build_flasher_site.py`, and deploys `site/` to GitHub Pages. A local `pio run` build installs through *Advanced → Custom .bin*.

The installer is adapted from [CrossPoint Tools](https://github.com/crosspoint-reader/crosspoint-tools) (MIT) and vendors [esptool-js](https://github.com/espressif/esptool-js) 0.5.7 (`site/assets/vendor/esptool-js/bundle.js`, Apache-2.0), the release crosspointreader.com ships; 0.7.1 stalled at the first flash read in the browser. To change esptool-js, replace that bundle with the `bundle.js` from the npm package, then prove an install, a rollback, and a backup on a real X4 with the hardware harness, which runs the installer's own `flasher.js` over USB from the command line:

```bash
cd site/hw && npm ci
node x4.mjs probe                      # chip, partition table, which slot boots
node x4.mjs install <firmware.bin|url> --reset
node x4.mjs rollback
node x4.mjs backup x4-backup.bin       # about 25 minutes, like the web page
node x4.mjs monitor 20 --reset         # boot log (crossx_x4 has no app logging; use a debug build)
```

### Command line

```bash
pio run -e crossx_x4 -t upload          # USB serial, 921600 baud
```

This writes the Arduino bootloader, the partition table, `boot_app0.bin` at `0xe000` (which resets `otadata` to boot `app0`), and the app at `0x10000`. Writing only `firmware.bin` at `0x10000` with `esptool.py` is not enough: if `otadata` points at `app1`, the reader keeps booting the old image. Include the boot selector (`boot_app0.bin` ships in `~/.platformio/packages/framework-arduinoespressif32/tools/partitions/`):

```bash
esptool.py --chip esp32c3 write_flash 0xe000 boot_app0.bin 0x10000 .pio/build/crossx_x4/firmware.bin
```

`crossx_x4` uses the same partition layout as CrossPoint `default` (two `0x640000` OTA slots at `0x10000` and `0x650000`), so CrossPoint's OTA and recovery instructions keep working.

## Recovery

- **Roll back:** the web installer's *Recovery → Roll back* points `otadata` at the other slot, which holds the firmware that ran before the last install. It refuses when that slot is empty.
- **Plain reader:** install the installer's *CrossPoint* build (same source, `default` env) over CrossX, or any official CrossPoint release from crosspointreader.com.
- **Full restore:** *Recovery → Restore a backup* writes a 16 MB backup made by the installer (or `esptool.py read_flash`) back from `0x0`. This replaces everything, including NVS.
- The SD card is never required to boot. Books, progress, and CrossX receipts live on the SD card and survive reflashing.
- On boot, `/crossx/incoming/*.part` leftovers are deleted, and each `/crossx/receipts/*.pending` is promoted to a receipt if its book landed or deleted if not. Nothing else under `/crossx` is deleted automatically.
- If the device does not enumerate over USB after a flash, follow [fix-bricked-xteink.md](../fix-bricked-xteink.md) before anything else.

## What CrossX changes that affects flashing

- Nothing in partitions, bootloader, or offsets.
- NVS gains two keys (pairing key, AUTH counter) under a `crossx` namespace. A full flash erase or a restore from an older backup removes them; the app must pair again.
