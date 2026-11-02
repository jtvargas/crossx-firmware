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
| `ProtocolV1.h/.cpp` | Wire format: 6-byte header, payload layouts, `Result` codes, UUIDs, `buildAuthMessage` / `buildDeviceProofMessage`, `encodeAuthResult`, constant-time compare. No heap, no Arduino. |
| `SyncCoordinator.h/.cpp` | The session state machine. Consumes radio/Wi-Fi/receiver events and time; emits effects through `Ports`. Serves one link by connection handle; owns advertising (on iff `Advertising`) and the heap gate in front of it; enforces mutual auth, the persisted replay counter, lockout, rate limit, `START_SYNC` retries, the per-session upload budget with one upload at a time, and all timeouts. |
| `SyncPowerPolicy.h` | `constexpr` table: per power mode, is BLE allowed, is Wi-Fi allowed, advertising interval, standby cap. |
| `LibraryCommit.h/.cpp` | Staging / pending-receipt / receipt / commit paths, title sanitising, file-id and digest validation, the commit decision table, the receipt MAC message, boot-time reconciliation rules. |

Design constraint: nothing in this library may include Arduino, NimBLE, SdFat, or FreeRTOS headers. That is what makes the host tests possible and what lets the X4 Pro reuse it unchanged.

Threading: `SyncCoordinator` has no locking. Every input (`requestAdvertising`, `requestIdle`, the `on…` events, `tick`) is delivered from one owning task; NimBLE, Wi-Fi, and HTTP callbacks queue events to that task instead of calling in. `Ports` calls never block, except `storeAuthCounter`, which may wait on NVS.

### Platform glue (planned, `src/crossx/`)

- **`BleGattServer`**: NimBLE peripheral exposing the service in `ProtocolV1.h`. Forwards connections where the X4 is the peripheral, with their connection handles, and control writes to the coordinator's task (`onCentralConnected/Disconnected`, `onControlWrite`); sends `Ports::notifyResult`. Starts and stops advertising only when the coordinator says so: NimBLE's advertise-on-disconnect stays off. The `crossx_x4` env enables the peripheral and broadcaster roles that `default` compiles out, and compiles out the BLE page-turner (the HID host links its stubs), so the CrossX peripheral is the NimBLE host's only user.
- **`WifiSession`**: joins the saved network with CrossPoint's `WifiCredentialStore` credentials, reports `onWifiUp(ipv4[4], port)` / `onWifiFailed`, tears down on `wifiStop()`.
- **`HttpReceiver`**: the endpoints in [protocol-v1.md](protocol-v1.md). Checks the token and the request, then asks `onUploadStarted(contentLength)` before writing a byte (`429` / `413` / `404` on refusal). Streams the body straight to the `.part` file in 4 KB chunks (same budget as `CrossPointWebServer::UploadState`) while hashing with mbedTLS SHA-256, calling `onUploadProgress` per chunk and `onUploadCommitted` / `onUploadFailed` at the end. Never holds a whole EPUB in RAM.
- **`ReceiptStore`**: `/crossx/receipts/<id>.pending` and `.meta`, tiny fixed-format files; read on `GET …/receipt` and on duplicate uploads; MACs each receipt it sends under the session key.
- **`PairingStore`**: pairing key derived from CrossPoint's NVS `DeviceSecret` (`lib/DeviceSecret`) plus the last accepted AUTH counter in NVS.
- **`Ports` implementation**: the above, behind the interface `SyncCoordinator` already compiles against.

## The state machine

Owned by `SyncCoordinator`. The main path:

```
Idle ──requestAdvertising──► Advertising ──central connects──► Connected ──AUTH ok──► Authenticated
                                  ▲                                                       │ START_SYNC
                                  │ cleanupDone                                           ▼
                               Cleanup ◄── finish / cancel / timeout ── TransferReady ◄── WifiConnecting
                                                                         │ upload admitted
                                                                         ▼
                                                                      Transferring
```

Every edge:

| From | Event | To |
| --- | --- | --- |
| Idle | `requestAdvertising`, SD mounted and the heap gate passes (else stays Idle: `StorageFailed` / `LowMemory`) | Advertising |
| Advertising | a central connects; its handle becomes the one link served | Connected |
| Advertising | `requestIdle` | Idle |
| Connected | AUTH ok | Authenticated |
| Authenticated | HELLO ok (trust starts over, 15 s clock restarts) | Connected |
| Connected, Authenticated | the link closes; 15 s without AUTH; 60 s without a command; HELLO with another major; fifth AUTH failure | Advertising |
| Connected, Authenticated | `requestIdle` | Idle |
| Authenticated | `START_SYNC` accepted | WifiConnecting |
| WifiConnecting | Wi-Fi up (`START_SYNC` answered) | TransferReady |
| WifiConnecting | Wi-Fi failed or 20 s (`WifiUnavailable`) | Error |
| WifiConnecting | the link closes, `CANCEL_SYNC`, `requestIdle` | Cleanup |
| TransferReady | first upload admitted | Transferring |
| TransferReady, Transferring | `POST /finish` | Finishing, then Cleanup in the same call |
| TransferReady, Transferring | `CANCEL_SYNC`, 90 s without upload traffic, `requestIdle` | Cleanup |
| Error | 2 s hold | Cleanup |
| Cleanup | `cleanupDone` with `requestIdle` pending, or the SD / heap gate fails | Idle |
| Cleanup | `cleanupDone` otherwise | Advertising |

The radio advertises exactly while the state is `Advertising`: the coordinator starts it on entering that state and stops it on leaving it, so it is off for the whole session. During `TransferReady` / `Transferring` the BLE link is optional; if it closes, the session carries on over Wi-Fi.

Rules the tests pin down:

- Only the accepted link is served. Another central is disconnected by its own handle; its disconnect and its writes change nothing. Writes with no link are ignored.
- Advertising is on iff the state is `Advertising`: it stops when a central connects; it restarts when the link closes, when an unauthenticated link times out, and after `Cleanup`; after `requestIdle` it stays off even when the dropped link's disconnect arrives.
- Advertising needs the SD card and the heap: with the BLE host down, 49,152 B for the host plus 32,768 B, and a 32,768 B largest block; with it running, only the block. Refused, the coordinator stays (or, after `Cleanup`, goes) `Idle` with `StorageFailed` or `LowMemory`. An unpaired device still advertises, to pair. The two numbers equal the page turner's.
- Every write counts against the 100 ms rate limit before it is parsed: a well-formed frame inside it gets `RateLimited`, a malformed one is dropped, so junk earns at most one answer per 100 ms. Outside it, malformed frames get `BadFrame`.
- `HELLO` → `AUTH` → anything else. HELLO is accepted only while `Connected` or `Authenticated`; on an authenticated link it clears trust and restarts the 15 s clock; during a session it is `Busy` and changes nothing. A major mismatch answers `UnsupportedVersion` and drops the link.
- A failed `AUTH` burns the nonce; five failures lock AUTH for 60 s and drop the link.
- AUTH is mutual: `Ok` carries `deviceMac` over `"CROSSX-DEV-v1" ‖ appNonce ‖ deviceNonce ‖ counter`; every other AUTH result is one byte.
- The AUTH counter is persisted before `Ok` and must strictly increase: replay across reconnects or reboots fails. If it cannot be persisted, AUTH answers `Internal` and authenticates nothing.
- `START_SYNC` is refused (without touching Wi-Fi) when a book is open, SD is missing, battery is under 10 %, no Wi-Fi credentials are saved, or the counts are out of range.
- `START_SYNC` retries: the same id while Wi-Fi joins gets one final result under the newest sequence; the same id afterwards replays the stored result; another id while a session runs is `Busy`. The address is in dotted byte order.
- Uploads: one at a time; at most the files and bytes `START_SYNC` declared; a failed upload gives its reservation back; none outside `TransferReady` / `Transferring`.
- 90 s without upload traffic (admission, chunk, commit, failure) ends the session; chunks keep a long upload alive, BLE commands do not keep a stalled one alive.
- The BLE link may drop during a transfer; the session continues over Wi-Fi and the idle timer closes it. A drop while Wi-Fi joins aborts the session.
- A failed or slow Wi-Fi join answers `WifiUnavailable`, holds `Error` for 2 s, then cleans up and advertises again.
- Every exit from a session passes through `Cleanup`, which stops HTTP, then Wi-Fi, then drops the link, in that order.
- Wire values: opcodes, result codes, the five UUIDs, the HELLO / AUTH / START_SYNC payload layouts, and the AUTH and device-proof messages.

Bounds (`SyncCoordinator.h`): 15 s unauthenticated, 60 s authenticated idle, 20 s Wi-Fi join, 90 s without upload traffic, 2 s error hold, 100 ms minimum command spacing, 5 AUTH failures then 60 s lockout, 64 files / 256 MiB per session and never more than `START_SYNC` declared, one upload in flight.

## Storage transaction

The receiver's I/O, in this order (`LibraryCommit.h` names every path):

```
POST /crossx/v1/sessions/{s}/files/{id}
  1. check token + request, then onUploadStarted(Content-Length)   refused: 4xx, nothing written
  2. stream to /crossx/incoming/{id}.part + SHA-256                 ≤ 64 MiB; onUploadProgress per 4 KB chunk
  3. verify length and digest                                       mismatch: delete .part, onUploadFailed, 422
  4. flush + close the .part
  5. write + flush /crossx/receipts/{id}.pending                    the receipt: {fileId, sha256, size, path}
  6. rename .part → /Books/CrossX/{title}-{id8}.epub                same volume, atomic at the FAT level: the commit
  7. rename .pending → /crossx/receipts/{id}.meta
  8. onUploadCommitted; 201 with the receipt and its mac
```

Power lost at any point leaves one of these, and the boot pass (before the receiver can start) settles it:

| Lost during | Left on SD | Boot does | The app then |
| --- | --- | --- | --- |
| steps 2–4 | `.part` | deletes the `.part` | finds no receipt, uploads again |
| step 5, or between 5 and 6 | `.part`, a whole or partial `.pending` | deletes the `.part`; no book at the path, so deletes the `.pending` | finds no receipt, uploads again |
| between 6 and 7 | the book, `.pending` | the book exists, so renames `.pending` to `.meta` | gets the receipt |
| after 7 | the book, `.meta` | nothing | gets the receipt |

- `/crossx/incoming`: every well-formed `<32 hex>.part` is deleted (a commit renames its `.part` away, so a leftover never finished); any other name is left alone (`leftoverAction`).
- `/crossx/receipts/<id>.pending`: promoted to `.meta` if the book at its path exists, deleted otherwise (`pendingReceiptAction`); a `.pending` that cannot be read counts as no book.
- Duplicate id + same digest: answer the stored receipt, write nothing (`CommitDecision::AlreadyCommitted`).
- Duplicate id + different digest: `409`; the app must mint a new id (`RejectDigestChanged`).
- Book names keep ASCII letters, digits, and `_`; every other byte is a separator, a run of separators is one `-`, none at either end, empty is `Untitled`.
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
- The BLE host takes 49,152 B on the C3 and the reader still needs one 32,768 B block beside it (`kBleHostBytes`, `kReaderLargestBlockBytes`, the page turner's measured numbers in `lib/BlePageTurner/src/RadioPolicy.h`, pinned equal by a test). Before advertising, from `Idle` or again after `Cleanup`, the coordinator requires both plus the block when the host is down, and the block alone when it is already running; otherwise it stays `Idle` with `LowMemory`.
- Receiver buffers: one 4 KB stream chunk, one SHA-256 context, one receipt struct, for the one upload the coordinator admits at a time. No `std::string` growth in the hot path.
- Release gates track free heap, minimum-ever free heap, largest free block, and task stack high-water marks for every state.

## Security model (summary)

See [protocol-v1.md](protocol-v1.md#security) for the details. In short: pairing requires a physical action on the X4; a 32-byte pairing key is derived from the per-device NVS secret; AUTH is mutual (the app's HMAC over both nonces and a strictly increasing counter, persisted before the X4 answers; the X4's own HMAC under a different label in the answer); only the one authenticated link is served. HTTP requests carry a per-request token under a session key derived from the pairing key, the session id, and both nonces, bound to method, path, length, and digest; there is no expiry because the X4 has no trusted clock, and the receiver exists only during the session. Receipts are MAC'd under the session key and the app trusts only receipts that verify. BLE addresses and names are never trusted. HTTP on the local network is not confidential; TLS is a measured follow-up, not an assumption.
