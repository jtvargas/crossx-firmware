# Roadmap

Phased so that every step leaves a bootable reader. A phase is done when its exit criteria are met on real hardware, not when the code compiles.

## Phase 0: foundation (this repository, in progress)

- [x] Fork CrossPoint with full history, pinned `freeink-sdk`
- [x] Reproduce the upstream `default` build with the pinned toolchain
- [x] `crossx_x4` build env with NimBLE peripheral + broadcaster roles
- [x] `lib/CrossXSync`: protocol v1, `SyncCoordinator`, power policy, commit rules, host tests
- [x] Docs: README, architecture, protocol, hardware, recovery, roadmap
- [x] CI builds `crossx_x4` and runs the host suite
- [x] Web installer on GitHub Pages (CrossPoint's OTA-slot install, rollback, full-flash backup/restore)
- [ ] Baseline measurements table filled from a real X4 (`docs/crossx/hardware-x4.md`)
- [ ] Recovery path verified on the actual unit

**Exit:** `crossx_x4` boots and reads EPUBs exactly like `default`; measurements recorded.

## Phase 1: BLE spike

- `BleGattServer` on NimBLE: advertise, connect, `HELLO`/`AUTH`/`GET_STATUS`, `START_SYNC` acknowledged without Wi-Fi
- Pairing UI (passkey) and key derivation from `DeviceSecret`
- Heap and current measurements with the peripheral running
- **Decision gate A:** standby current acceptable? If not, standby is off by default.

**Exit:** iOS test app (or nRF Connect scripts) completes HELLO/AUTH 100× in a row; reconnect and lockout behave as the host tests say; numbers in the hardware table.

## Phase 2: one-file transfer

- `WifiSession`, `HttpReceiver`, `ReceiptStore`
- Streamed upload → `.part` → verify → rename → receipt
- Interrupted-transfer tests: pull the plug mid-upload, confirm nothing visible in the library and the `.part` is gone after reboot
- 100 consecutive uploads without heap growth

**Exit:** a 5 MB EPUB lands and opens; receipts are re-readable; no reader regression.

## Phase 3: CrossX app integration

- `CrossXReaderDeviceService` in the iOS app beside Stock and CrossPoint transports
- Queue items carry `fileId` and SHA-256; "On Device" only after a receipt whose MAC verifies
- Foreground and background scenarios documented as best effort

## Phase 4: minimal reading UI

- Settings → CrossX (pair, standby toggle, last sync, pending receipts)
- Sync status on the home screen; nothing in the reader view

## Phase 5: hardening

- Fault injection: SD removed, Wi-Fi drop, BLE drop, low battery, power button mid-upload
- 500-cycle soak; battery life in mixed use; release checklist

## Phase 6: X4 Pro

- `crossx_x4pro` env; same `lib/CrossXSync`; S3 power, PSRAM, display, and recovery differences documented
