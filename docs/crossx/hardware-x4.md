# Hardware notes: original XTEINK X4

What is known from CrossPoint and the FreeInk SDK, what is measured, and what is still a question. Entries marked **open** are blockers for the phase that needs them.

## Known (from source)

| Item | Value | Where |
| --- | --- | --- |
| MCU | ESP32-C3, single core RISC-V, no PSRAM | `freeink-sdk/libs/hardware/BoardConfig`, `AGENTS.md` |
| Usable RAM | ~380 KB, one 48 KB framebuffer for 800×480 | `AGENTS.md`, `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1` |
| Flash | 16 MB | `platformio.ini` `[base]` |
| Radios | Wi-Fi 2.4 GHz and BLE 5 share one RF path | ESP32-C3 datasheet |
| BLE in CrossPoint | NimBLE **central/HID host only**; peripheral and broadcaster roles compiled out | `platformio.ini` `[ble_page_turner]` |
| BLE host RAM | ~49 KB on the C3 while running; needs a 32 KB block free afterwards for the reader | `lib/BlePageTurner/src/RadioPolicy.h` |
| Power latch | none for X4/X3 (`PowerConfig` pins unassigned); power button wake only | `BoardConfig.h` |
| Sleep | ESP32 deep sleep via `HalPowerManager::startDeepSleep`; BLE and Wi-Fi are stopped first | `src/main.cpp` `enterDeepSleep()` |
| Low-power clock | 10 MHz when idle | `lib/hal/HalPowerManager.h` `LOW_POWER_FREQ` |
| Device secret | 32 random bytes in NVS, survives SD swaps | `lib/DeviceSecret` |
| Saved Wi-Fi | station credentials stored by CrossPoint | `WifiCredentialStore` |
| Upload staging | existing `.part` + rename pattern, 4 KB buffer | `src/network/CrossPointWebServer.h` |

## Consequences

- "Always listening" over BLE is impossible in deep sleep. A standby mode has to keep the chip awake (light sleep with BLE modem sleep at best). Its cost is unmeasured.
- Running the BLE peripheral while reading is a RAM question, not a feature flag: ~49 KB on a ~380 KB chip that also needs 32 KB contiguous for the EPUB inflater.
- Wi-Fi and BLE coexist on one radio; a transfer while advertising will slow one or the other. Sessions stop advertising once Wi-Fi is up.

## Measurements (fill in on real hardware)

| Measurement | Baseline CrossPoint `default` | `crossx_x4` | Method |
| --- | --- | --- | --- |
| Firmware size (flash) | | | `pio run` summary |
| Free heap at Home | | | serial log at boot |
| Largest free block at Home | | | `heap_caps_get_largest_free_block` |
| Free heap while reading | | | |
| Free heap with BLE advertising | n/a | | |
| Free heap during HTTP upload | | | |
| Current: deep sleep | | | USB power meter / shunt |
| Current: reading idle | | | |
| Current: standby advertising 1 s | n/a | | |
| Current: Wi-Fi join + 5 MB upload | | | |
| Page-turn latency | | | stopwatch / logic analyser on EPD busy |
| Wi-Fi join time (saved network) | | | |
| 5 MB upload time | | | |

## Open questions

- **open, gate A:** standby current with BLE advertising at 1 s interval and the CPU at 10 MHz. Target: comparable to idle reading. If it is not, standby ships off by default and sync is user-initiated only.
- **open:** exact board revision(s) of Jonathan's X4(s) and whether they are USB-locked (see [flashing-and-recovery.md](flashing-and-recovery.md)).
- **open:** can the NimBLE host run peripheral + the page-turner central at once within RAM, or does the `crossx_x4` env drop the page-turner?
- **open:** does the 247 MTU negotiate with iOS reliably on this stack (iOS usually offers 185 or 251)?
- **open:** SD write throughput while hashing on the C3 (sets the realistic upload speed).
