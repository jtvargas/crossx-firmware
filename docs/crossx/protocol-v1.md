# CrossX protocol v1

The contract between the CrossX app and the X4. The firmware side is `lib/CrossXSync/src/ProtocolV1.h`; every constant below is defined there and pinned by `lib/CrossXSync/test/ProtocolV1Test.cpp`.

Version: **1.0**. The major number changes only for incompatible frames; the service UUID changes with it.

## Transport roles

| Plane | Carries | Transport |
| --- | --- | --- |
| Control | discovery, pairing, authentication, session start/cancel, status | BLE GATT, one connection |
| Data | EPUB bytes, receipts | HTTP/1.1 over temporary station-mode Wi-Fi |

## GATT service

| | UUID | Properties | Payload |
| --- | --- | --- | --- |
| Service | `c0a5e000-6b5c-4f1e-9a2d-8c1f3e5b7d01` | | |
| `protocolInfo` | `c0a5e001-…7d01` | read | `major u8, minor u8, features u32 LE` |
| `deviceStatus` | `c0a5e002-…7d01` | read, notify | `StatusPayload` (below) |
| `control` | `c0a5e003-…7d01` | write with response | one request frame |
| `controlResult` | `c0a5e004-…7d01` | notify, read | one result frame |

Advertising carries the service UUID and a device name (`CrossX-XXXX`, last two bytes of the BLE address). Nothing sensitive is ever advertised.

Preferred MTU is 247, so one frame (max 244 bytes) is one ATT write.

## Frame

```
offset  size  field
0       1     opcode (requests 0x01–0x05; results = opcode | 0x80)
1       1     flags, must be 0
2       2     sequence, little-endian, echoed in the result
4       2     payloadLength, little-endian, must equal frame length - 6
6       n     payload
```

Parse failures (`TooShort`, `TooLong`, `LengthMismatch`, `UnknownOpcode`, `ReservedFlags`) are answered with a one-byte `BadFrame` result carrying whatever sequence the frame had.

## Commands

| Opcode | Name | Needs | Request payload | Result payload |
| --- | --- | --- | --- | --- |
| `0x01` | `HELLO` | connection | `major u8, minor u8, features u32, appNonce[16]` | `result u8, major u8, minor u8, features u32, deviceNonce[16], state u8` |
| `0x02` | `AUTH` | HELLO | `counter u32, mac[32]` | `result u8` |
| `0x03` | `GET_STATUS` | AUTH | empty | `result u8, StatusPayload` |
| `0x04` | `START_SYNC` | AUTH | `sessionId[16], fileCount u16, totalBytes u32` | `result u8, sessionId[16], ipv4 u32, httpPort u16, ttlSeconds u16` |
| `0x05` | `CANCEL_SYNC` | AUTH | empty | `result u8` |

`StatusPayload`: `state u8, battery u8, flags u8 (bit0 SD, bit1 Wi-Fi up, bit2 reader open, bit3 paired), freeHeap u32, largestFreeBlock u32, pendingReceipts u16`.

### Sequence

```
app                                 X4
 │── HELLO(appNonce) ───────────────►│  checks major, pairing, draws deviceNonce
 │◄─ HELLO result(deviceNonce) ──────│
 │── AUTH(counter, mac) ────────────►│  mac = HMAC-SHA256(key, "CROSSX-AUTH-v1"‖deviceNonce‖appNonce‖counter)
 │◄─ AUTH result(Ok) ────────────────│  counter stored
 │── START_SYNC(sessionId, n, bytes)►│  joins Wi-Fi; no result yet
 │◄─ START_SYNC result(ip, port) ────│  when Wi-Fi is up and the receiver listens
 │══ HTTP uploads ═══════════════════│
 │── POST …/finish ─────────────────►│
 │◄─ (BLE) status: Cleanup → Advertising
```

`START_SYNC` is idempotent for the same `sessionId`: if the app missed the result it sends the same request and gets the same answer. A different `sessionId` while one is active is `Busy`.

## Result codes

| Code | Name | Meaning |
| --- | --- | --- |
| `0x00` | `Ok` | |
| `0x01` | `BadFrame` | malformed frame or payload, or out-of-range values |
| `0x02` | `UnsupportedVersion` | major mismatch; the X4 disconnects |
| `0x03` | `NotPaired` | the X4 has no pairing key for any app |
| `0x04` | `NotAuthenticated` | command before a successful AUTH |
| `0x05` | `AuthFailed` | wrong MAC; HELLO must be repeated |
| `0x06` | `Replayed` | counter not greater than the last accepted |
| `0x07` | `RateLimited` | commands closer than 100 ms |
| `0x08` | `Busy` | another session is active, or battery too low |
| `0x09` | `ReaderActive` | a book is open; sync will not interrupt reading |
| `0x0A` | `WifiUnavailable` | no saved credentials, or join failed / timed out |
| `0x0B` | `StorageUnavailable` | SD not mounted |
| `0x0C` | `Internal` | e.g. no entropy for the nonce |
| `0x0D` | `LockedOut` | five AUTH failures; try again after 60 s |
| `0x0E` | `OutOfOrder` | AUTH before HELLO |

## Pairing

Pairing is a one-time, physical step: the user opens *Settings → CrossX → Pair* on the X4, which shows a 6-digit code and advertises for 2 minutes. The app connects, performs LE Secure Connections pairing with passkey entry, and the X4 derives the 32-byte pairing key as `HKDF-SHA256(deviceSecret, "crossx-pairing-v1" ‖ appIdentity)`. The key never leaves the X4; the app stores its own copy from the pairing exchange. One app at a time in v1; pairing again replaces the key and resets the counter.

> Pairing details are a Phase 1 deliverable and may change after hardware testing. The AUTH message format above is final.

## HTTP API (data plane)

Base: `http://<ipv4>:<httpPort>/crossx/v1`, only while the session is active. Every request carries `Authorization: CrossX <token>` where `token = base64(HMAC-SHA256(sessionKey, method ‖ path ‖ contentLength ‖ sha256Hex ‖ expiresAt))` and `X-CrossX-Expires: <unix seconds>`. `sessionKey` is derived from the pairing key and `sessionId`.

| Method & path | Purpose | Notes |
| --- | --- | --- |
| `GET /health` | receiver is up | returns `{"state":"TransferReady","session":"<id>"}` |
| `POST /sessions/{sessionId}/files/{fileId}` | upload one EPUB | raw body; headers `Content-Length`, `X-CrossX-Sha256`, `X-CrossX-Title`. `201` + receipt, `200` + receipt if already committed, `409` if the id exists with another digest, `413` over 64 MiB, `422` digest or length mismatch |
| `GET /files/{fileId}/receipt` | durable proof | `200` + receipt or `404` |
| `POST /sessions/{sessionId}/finish` | app is done | triggers Cleanup; `204` |

`fileId` is 32 lowercase hex characters minted by the app per queue item and reused across retries. `sha256Hex` is 64 lowercase hex.

Receipt (JSON, also stored as `/crossx/receipts/<fileId>.meta`):

```json
{"fileId":"…","sha256":"…","size":1234567,"path":"/Books/CrossX/Deep-Work-01234567.epub","committedAt":1730000000}
```

The app marks an item **On Device** only after it holds a receipt. A TCP success or a `2xx` without a receipt body is not enough.

## Security

- BLE address and device name are identification, not authentication.
- Pairing needs a physical action on the X4 and LE Secure Connections.
- Application-layer AUTH binds both nonces and a monotonic counter; failures are counted and locked out.
- HTTP requests are authenticated per request and bound to method, path, length, digest, and expiry; the receiver only exists during a session.
- Everything is bounded: frames (244 B), payload fields (fixed), file size (64 MiB), files per session (64), timeouts.
- HTTP over the local network is not confidential. TLS on the C3 is a measured follow-up (RAM and time per handshake) before it can be promised.
