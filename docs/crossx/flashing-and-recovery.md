# Flashing and recovery

**Do not flash an experimental build to an X4 until you have a verified way to recover it.**

## Before the first flash

1. Identify the device. Note the model, firmware version shown in the stock settings, and purchase date. Some X4 batches ship **USB-locked**: the bootloader only accepts images signed or recognised by the vendor path, and a generic `esptool` flash can leave the device unbootable with no serial recovery.
2. Read CrossPoint's guides, which apply unchanged to this fork:
   - [Upstream README: install methods and the USB-locked warning](../upstream-crosspoint-readme.md)
   - [Fix a bricked Xteink](../fix-bricked-xteink.md)
   - [Troubleshooting](../troubleshooting.md)
3. Back up the stock firmware if the device lets you (`esptool.py read_flash 0 0x1000000 stock-x4.bin`) and keep the SD card contents.
4. Confirm that plain CrossPoint `default` installs and boots on *this* unit first. If CrossPoint cannot be installed by a supported path, CrossX cannot either.
5. Only then flash `crossx_x4`.

## Flashing

```bash
pio run -e crossx_x4 -t upload          # USB serial, 921600 baud
# or, from a built image:
esptool.py --chip esp32c3 write_flash 0x10000 .pio/build/crossx_x4/firmware.bin
```

`crossx_x4` uses the same partition layout and offset (`0x10000`) as CrossPoint `default`, so CrossPoint's OTA and recovery instructions keep working.

## Recovery

- CrossX builds are OTA-compatible with CrossPoint: flashing CrossPoint `default` over `crossx_x4` restores the plain reader.
- The SD card is never required to boot. Books, progress, and CrossX receipts live on the SD card and survive reflashing.
- `/crossx/incoming/*.part` leftovers are cleaned on boot; nothing else under `/crossx` is deleted automatically.
- If the device does not enumerate over USB after a flash, follow [fix-bricked-xteink.md](../fix-bricked-xteink.md) before anything else.

## What CrossX changes that affects flashing

- Nothing in partitions, bootloader, or offsets.
- NVS gains two keys (pairing key, AUTH counter) under a `crossx` namespace. A full flash erase removes them; the app must pair again.
