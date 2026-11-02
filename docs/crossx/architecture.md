# CrossX architecture

How the sync layer sits on top of CrossPoint Reader, who owns what, and the rules every piece follows. Read this before touching `lib/CrossXSync` or wiring it into `src/`.

## Goals and non-goals

**Goals (v1)**

- Keep CrossPoint's reading experience and reliability on the original X4 (ESP32-C3) untouched.
- Let the CrossX app discover the X4 over BLE, authenticate, and ask it to receive books.
- Move EPUBs over a temporary station-mode Wi-Fi connection with bounded, verified, durable commits.
- Make every outcome observable: the app can always ask "did file X land?" and get a durable answer.
- Keep a shared-source path to the X4 Pro (ESP32-S3).

**Non-goals (v1)**

- No cloud, accounts, telemetry, or always-on Wi-Fi.
- No EPUB bytes over BLE.
- No remote shell, file browser, or arbitrary commands.
- No new reader engine, no on-device web extraction or conversion.
- No promise of guaranteed unattended delivery from iOS (background BLE/URLSession is best effort).

## Component map

```
            CrossX iOS app
        BLE (control)  │  HTTP (bytes)
───────────────────────┼──────────────────────────────── firmware
  BleGattServer*  ───► SyncCoordinator ◄─── HttpReceiver*
  (NimBLE peripheral)   │  the only owner of      (bounded upload, SHA-256,
                        │  session state           streamed to SD)
                        ▼
                 Ports (platform glue)*
         radio ▪ Wi-Fi STA ▪ HTTP ▪ nonce ▪ HMAC ▪ counter store
                        │
     ┌──────────────────┼──────────────────┐
     ▼                  ▼                  ▼
 LibraryCommit     SyncPowerPolicy     ProtocolV1
 (paths, decision  (what each power    (frames, payloads,
  table)            mode allows)        bounds, UUIDs)
───────────────────────────────────────────────────────── CrossPoint (unchanged)
  Reader ▪ Epub ▪ GfxRenderer ▪ HalStorage/SdFat ▪ HalPowerManager ▪ WifiCredentialStore ▪ DeviceSecret
```

`*` = planned firmware glue (Phase 1–2). Everything without a star exists today in `lib/CrossXSync` and is covered by host tests.

### `lib/CrossXSync` (pure, host-tested)

| File | Owns |
| --- | --- |
| `ProtocolV1.h/.cpp` | Wire format: 6-byte header, payload layouts, `Result` codes, UUIDs, `buildAuthMessage`, constant-time compare. No heap, no Arduino. |
| `SyncCoordinator.h/.cpp` | The session state machine. Consumes radio/Wi-Fi/receiver events and time; emits effects through `Ports`. Enforces auth order, replay counter, lockout, rate limit, idempotent `START_SYNC`, all timeouts. |
| `SyncPowerPolicy.h` | `constexpr` table: per power mode, is BLE allowed, is Wi-Fi allowed, advertising interval, standby cap. |
| `LibraryCommit.h/.cpp` | Staging / receipt / commit paths, title sanitising, file-id and digest validation, the commit decision table, boot-time leftover handling. |

Design constraint: nothing in this library may include Arduino, NimBLE, SdFat, or FreeRTOS headers. That is what makes the 52 host tests possible and what lets the X4 Pro reuse it unchanged.

### Platform glue (planned, `src/crossx/`)

- **`BleGattServer`**: NimBLE peripheral exposing the service in `ProtocolV1.h`. Translates characteristic writes to `SyncCoordinator::onControlWrite`, notifications from `Ports::notifyResult`. Owns advertising start/stop. Shares the NimBLE host with the existing page-turner central; the `crossx_x4` env enables the peripheral and broadcaster roles that `default` compiles out.
- **`WifiSession`**: joins the saved network with CrossPoint's `WifiCredentialStore` credentials, reports `onWifiUp(ip, port)` / `onWifiFailed`, tears down on `wifiStop()`.
- **`HttpReceiver`**: the endpoints in [protocol-v1.md](protocol-v1.md). Streams the body straight to the `.part` file in 4 KB chunks (same budget as `CrossPointWebServer::UploadState`) while hashing with mbedTLS SHA-256. Never holds a whole EPUB in RAM.
- **`ReceiptStore`**: `/crossx/receipts/<id>.meta`, tiny fixed-format files; read on `GET …/receipt` and on duplicate uploads.
- **`PairingStore`**: pairing key derived from CrossPoint's NVS `DeviceSecret` (`lib/DeviceSecret`) plus the last accepted AUTH counter.
- **`Ports` implementation**: the above, behind the interface `SyncCoordinator` already compiles against.

## The state machine

Owned by `SyncCoordinator`. States and the events that move them:

```
Idle ──requestAdvertising──► Advertising ──centralConnected──► Connected
  ▲                              ▲    ▲                             │ AUTH ok
  │ requestIdle                  │    │ disconnect / timeout        ▼
  │                              │    └───────────────────── Authenticated
  │                              │ cleanupDone                       │ START_SYNC
  │                              │                                   ▼
  │                           Cleanup ◄── cancel/finish/timeout ── WifiConnecting ──wifiUp──► TransferReady
  │                              ▲                                     │ wifiFailed/timeout        │ first upload
  │                              └────── Error (2 s hold) ◄────────────┘                           ▼
  └──────────────────────────── (requestIdle pending) ◄────────── Finishing ◄──────────── Transferring
```

Rules the tests pin down:

- Writes are ignored unless a central is connected; malformed frames get `BadFrame`, nothing else.
- `HELLO` → `AUTH` → anything else. A failed `AUTH` burns the nonce; five failures lock AUTH for 60 s and drop the link.
- The AUTH counter is persisted and must strictly increase: replay across reconnects or reboots fails.
- `START_SYNC` is refused (without touching Wi-Fi) when a book is open, SD is missing, battery is under 10 %, or no Wi-Fi credentials are saved.
- `START_SYNC` for the same session id is idempotent; a different id while a session runs is `Busy`.
- The BLE link may drop during a transfer; the session continues over Wi-Fi and the idle timer (90 s) closes it.
- Every exit from a session passes through `Cleanup`, which stops HTTP, then Wi-Fi, then drops the central, in that order.

Bounds (`SyncCoordinator.h`): 15 s unauthenticated, 60 s authenticated idle, 20 s Wi-Fi join, 90 s transfer idle, 100 ms minimum command spacing, 64 files / 256 MiB per session.

## Storage transaction

```
POST /crossx/v1/sessions/{s}/files/{id}
  → /crossx/incoming/{id}.part     stream + SHA-256, bounded by Content-Length ≤ 64 MiB
  → verify length and digest       mismatch: delete .part, 422
  → flush + close
  → rename to /Books/CrossX/{title}-{id8}.epub   same volume, so atomic at the FAT level
  → write /crossx/receipts/{id}.meta              {digest, size, path, committedAt}
  → 201 with the receipt
```

- Duplicate id + same digest: answer the stored receipt, write nothing (`CommitDecision::AlreadyCommitted`).
- Duplicate id + different digest: `409`; the app must mint a new id (`RejectDigestChanged`).
- On boot, `/crossx/incoming/*.part` with a valid id and no receipt is deleted; anything else there is left alone.
- The library never indexes `/crossx/incoming`. Existing books and reading progress are never touched.

## Power states

| Mode | BLE | Wi-Fi | Notes |
| --- | --- | --- | --- |
| Reading | only if the user enabled it, slow advertising | off | nothing may cost a page turn |
| Auto-sync standby | slow advertising (1 s) | only inside a session | device awake at low clock; capped at 30 min then powers off; low battery disables it |
| Power off / deep sleep | off | off | nothing can arrive; the UI must say so |

The X4 has no software power latch and CrossPoint's sleep is ESP32 deep sleep, which turns the radio off. "Standby" therefore has to be a new awake-at-low-clock path, and whether its current draw is acceptable is **decision gate A** in the [roadmap](roadmap.md). Until measured, the firmware ships with standby off by default.

## Memory strategy (ESP32-C3)

- No new permanent allocations while reading. The GATT server and the receiver are started on demand and freed on `Cleanup`.
- BLE host stack costs about 49 KB on the C3 (`lib/BlePageTurner/src/RadioPolicy.h`); the coordinator refuses to advertise unless that fits with a 32 KB block to spare, reusing the page-turner's measured thresholds.
- Receiver buffers: one 4 KB stream chunk, one SHA-256 context, one receipt struct. No `std::string` growth in the hot path.
- Release gates track free heap, minimum-ever free heap, largest free block, and task stack high-water marks for every state.

## Security model (summary)

See [protocol-v1.md](protocol-v1.md#security) for the details. In short: pairing requires a physical action on the X4; a 32-byte pairing key is derived from the per-device NVS secret; every AUTH is an HMAC over both nonces and a strictly increasing counter; every HTTP request carries a session token issued over the authenticated BLE link and bound to method, path, length, and digest. BLE addresses and names are never trusted. HTTP on the local network is not confidential; TLS is a measured follow-up, not an assumption.
