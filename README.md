<p align="center">
  <strong>CrossX Firmware</strong><br>
  Reading-first firmware for the XTEINK X4, with opt-in Bluetooth-coordinated sync from the CrossX iOS app.
</p>

<p align="center">
  <a href="https://github.com/jtvargas/crossx-firmware/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/jtvargas/crossx-firmware/actions/workflows/ci.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-blue.svg"></a>
  <img alt="Target" src="https://img.shields.io/badge/target-XTEINK%20X4%20(ESP32--C3)-black">
  <img alt="Status" src="https://img.shields.io/badge/status-pre--alpha-orange">
</p>

---

CrossX Firmware is a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) for one device and one job: read EPUBs well on the original XTEINK X4, and let the [CrossX app](https://github.com/jtvargas/crosspoint-app) send new books to it without the user digging through Wi-Fi menus.

Everything that makes CrossPoint a good reader (EPUB rendering, fonts, SD library, sleep, battery) is kept as-is. What CrossX adds is a small, bounded, well-tested sync layer:

| Layer | What it does | Where |
| --- | --- | --- |
| **BLE control plane** | The X4 advertises a GATT service. The app pairs once, then authenticates and asks for a sync. No book bytes ever travel over BLE. | `lib/CrossXSync` (protocol, state machine) |
| **Temporary Wi-Fi + HTTP receiver** | Only during an authenticated session the X4 joins the saved 2.4 GHz network and accepts bounded, hashed EPUB uploads. | planned: `src/crossx/` |
| **Durable SD commit** | Uploads stream to `/crossx/incoming/*.part`, are verified (length + SHA-256), renamed into the library, and acknowledged with a receipt the app can re-read. | `lib/CrossXSync/src/LibraryCommit.*` |
| **Power policy** | Reading always wins. Standby advertising is best effort and bounded; deep sleep means no radio, and the docs say so. | `lib/CrossXSync/src/SyncPowerPolicy.h` |

## Status

**Pre-alpha. Nothing here has been flashed to an X4 yet.**

| Phase | Scope | State |
| --- | --- | --- |
| 0 | Fork, pinned SDK, baseline build, docs, host-testable core | **in progress** (this repo) |
| 1 | BLE peripheral spike on real hardware: advertise, pair, `HELLO`/`AUTH`/`GET_STATUS`, standby current measured | not started |
| 2 | Wi-Fi bring-up + one-file HTTP transfer with staged SD commit and receipts | not started |
| 3 | CrossX iOS app integration (`CrossXReaderDeviceService`) | not started |
| 4 | Minimal reading UI | not started |
| 5 | Power and reliability hardening, 500-cycle soak | not started |
| 6 | X4 Pro (ESP32-S3) port | not started |

The full plan, with exit criteria per phase, is in [docs/crossx/roadmap.md](docs/crossx/roadmap.md).

## Hardware

| | Original X4 (first target) | X4 Pro (later) |
| --- | --- | --- |
| MCU | ESP32-C3, single core, no PSRAM, ~380 KB usable RAM | ESP32-S3, PSRAM |
| Display | 800×480 mono E Ink, one 48 KB framebuffer | same panel |
| Radio | Wi-Fi 2.4 GHz + BLE 5 (shared) | same |
| Power latch | none in software: sleep is deep sleep, which turns BLE off | TBD |
| Build env | `crossx_x4` | planned `crossx_x4pro` |

The honest hardware notes, including what is *not* yet known about standby battery life, are in [docs/crossx/hardware-x4.md](docs/crossx/hardware-x4.md).

## Installing

The quickest way to try CrossX is the **web installer**: https://jtvargas.github.io/crossx-firmware/

It flashes the latest `main` build to an original X4 from Chrome or Edge over USB-C, with no toolchain on your computer. The same page can install plain CrossPoint on the same device as a way back. Every push to `main` rebuilds both images, so the installer always matches this repository.

> **Before flashing anything:** read [docs/crossx/flashing-and-recovery.md](docs/crossx/flashing-and-recovery.md). Some X4 units are USB-locked and can only be recovered through specific paths. Do not flash an experimental image without a verified way back.

## Building

Prerequisites: Python 3.10+, [pioarduino core 6.1.19](https://github.com/pioarduino/platformio-core), `clang-format` 21, CMake 3.16+. The details are in CrossPoint's [getting-started guide](docs/contributing/getting-started.md), which applies unchanged.

```bash
git clone --recurse-submodules https://github.com/jtvargas/crossx-firmware.git
cd crossx-firmware

# Firmware for the original X4 with the CrossX BLE service
pio run -e crossx_x4

# Plain CrossPoint build for the same board (sanity check that the fork is still a reader)
pio run -e default

# Host tests for the CrossX core only (fast)
cmake -S lib/CrossXSync/test -B build/crossx && cmake --build build/crossx && ctest --test-dir build/crossx

# Every host test, CrossPoint's included
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test -j
```

Flashing a local build: `pio run -e crossx_x4 -t upload` once you have read the recovery notes, or use the [web installer](https://jtvargas.github.io/crossx-firmware/) for the published `main` build.

## Repository map

```
lib/CrossXSync/        CrossX core: pure C++, no radio or filesystem, fully host-tested
  src/ProtocolV1.*       BLE control frames, UUIDs, payload encoding and bounds
  src/SyncCoordinator.*  the one state machine that owns a sync session
  src/SyncPowerPolicy.h  what each power mode allows the radios to do
  src/LibraryCommit.*    staging/receipt/commit paths and the idempotency decision table
  test/                  GoogleTest suite (52 tests)
docs/crossx/           architecture, protocol v1, hardware notes, flashing & recovery, roadmap
web/                   browser installer (ESP Web Tools) published to GitHub Pages
src/, lib/, freeink-sdk/   CrossPoint Reader, unchanged apart from the crossx_x4 build env
platformio.ini         build environments; CrossX adds [env:crossx_x4]
```

## Documentation

- [Architecture](docs/crossx/architecture.md): components, ownership, state machine, storage transaction, memory strategy
- [Protocol v1](docs/crossx/protocol-v1.md): GATT service, frame format, commands, results, HTTP API, receipts
- [Hardware: X4](docs/crossx/hardware-x4.md): what is known, what is measured, what is still a question
- [Flashing and recovery](docs/crossx/flashing-and-recovery.md)
- [Roadmap](docs/crossx/roadmap.md)
- [Upstream CrossPoint README](docs/upstream-crosspoint-readme.md), kept verbatim for reader features, supported devices, and credits

## Design rules (short version)

1. **Reading first.** No sync feature may slow a page turn, grow the reader's heap budget, or touch an open book.
2. **BLE is a control plane.** Discovery, pairing, authentication, "start a sync". Never file bytes.
3. **Wi-Fi is temporary.** Station mode, saved credentials, up only inside an authenticated session, torn down after.
4. **Commits are durable or they did not happen.** `.part` → verify → rename → receipt. Interrupted uploads leave nothing the library can see.
5. **Everything is bounded.** Frame sizes, file sizes, retries, timeouts, counters. The bounds live in headers and the tests pin them.
6. **No promises the hardware has not proven.** Standby battery life, background delivery on iOS, and recovery paths are documented as measured or as unknown, never assumed.

## Relationship to CrossPoint Reader

This repository carries CrossPoint's full history and keeps its MIT licence, credits, and `freeink-sdk` submodule pin, so upstream fixes can be cherry-picked. CrossX changes are kept in clearly separated places (`lib/CrossXSync`, `docs/crossx`, the `crossx_x4` env) to make that easy. Upstream's own contributor rules (`docs/contributing`, `.agents/`) still apply to the code they cover. Nothing here is affiliated with or endorsed by the CrossPoint maintainers or XTEINK.

## Contributing

Issues and PRs are welcome once Phase 1 has hardware results. Until then the most useful contribution is an X4 on a bench with a current meter: see the open measurements in [docs/crossx/hardware-x4.md](docs/crossx/hardware-x4.md).

Run `./bin/clang-format-fix` before committing; CI checks formatting, `cppcheck`, the firmware builds, and the host tests.

## License

MIT, same as CrossPoint Reader. See [LICENSE](LICENSE).
