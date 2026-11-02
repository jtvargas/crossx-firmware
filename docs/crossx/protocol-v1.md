# CrossX protocol v1

The contract between the CrossX app and the X4. The firmware side is `lib/CrossXSync/src/ProtocolV1.h` (wire format) and `lib/CrossXSync/src/SyncCoordinator.h` (bounds and timeouts); every constant below is defined there and pinned by the tests in `lib/CrossXSync/test/`.

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

The X4 advertises only while it waits for an app: it stops when a central connects and starts again when that link closes outside a session, or when a session's Cleanup ends. It serves one central at a time; any other central that connects is disconnected, and its writes are never answered.

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

Every write counts against the rate limit before it is parsed: a write less than 100 ms after the last one the X4 accepted is not processed. If it is a well-formed frame it is answered `RateLimited`; if it is malformed it is dropped without an answer, so a burst of junk earns at most one notification per 100 ms.

Otherwise, parse failures (`TooShort`, `TooLong`, `LengthMismatch`, `UnknownOpcode`, `ReservedFlags`) are answered with a one-byte `BadFrame` result carrying whatever sequence the frame had.

## Commands

| Opcode | Name | Needs | Request payload | Result payload |
| --- | --- | --- | --- | --- |
| `0x01` | `HELLO` | connection, no session running | `major u8, minor u8, features u32, appNonce[16]` | `result u8, major u8, minor u8, features u32, deviceNonce[16], state u8` |
| `0x02` | `AUTH` | HELLO | `counter u32, mac[32]` | Ok: `result u8, deviceMac[32]`; otherwise `result u8` |
| `0x03` | `GET_STATUS` | AUTH | empty | `result u8, StatusPayload` |
| `0x04` | `START_SYNC` | AUTH | `sessionId[16], fileCount u16, totalBytes u32` | `result u8, sessionId[16], ipv4[4], httpPort u16, ttlSeconds u16` |
| `0x05` | `CANCEL_SYNC` | AUTH | empty | `result u8` |

All integers are little-endian. `ipv4` is the address in dotted order: `192.168.1.5` is the bytes `C0 A8 01 05`; it is all zero unless the result is `Ok`. `ttlSeconds` is how long the receiver stays up without upload traffic (90).

`StatusPayload`: `state u8, battery u8, flags u8 (bit0 SD, bit1 Wi-Fi up, bit2 reader open, bit3 paired), freeHeap u32, largestFreeBlock u32, pendingReceipts u16`.

### HELLO

Accepted while the link is `Connected` or `Authenticated`. On an `Authenticated` link a successful HELLO starts trust over: authentication is cleared, the link is `Connected` again, and the 15 s limit for authenticating restarts from that HELLO. While a session runs (`WifiConnecting` through `Transferring`) HELLO is answered `Busy` in the usual layout (zero nonce, current state) and changes nothing.

### AUTH

```
mac       = HMAC-SHA256(pairingKey, "CROSSX-AUTH-v1" ‖ deviceNonce ‖ appNonce ‖ counter)
deviceMac = HMAC-SHA256(pairingKey, "CROSSX-DEV-v1"  ‖ appNonce ‖ deviceNonce ‖ counter)
```

`counter` is the request's counter as 4 little-endian bytes; the labels are ASCII without a terminator. The counter must be greater than the last one the X4 accepted. The X4 checks `mac`, persists the counter, and only then answers `Ok` with `deviceMac`. The app verifies `deviceMac` (constant-time) before it sends anything else and disconnects if it does not match: the X4 has to prove it holds the pairing key too. If the counter cannot be persisted the answer is `Internal` and the app starts again with HELLO. A wrong `mac` burns the nonces (`AuthFailed`, HELLO again); the fifth failure in a row answers `LockedOut`, drops the link, and refuses AUTH for 60 s.

### START_SYNC

The X4 answers once Wi-Fi is up and the receiver listens (`Ok` with the address) or when joining fails or takes more than 20 s (`WifiUnavailable`). Retries:

- **Same `sessionId` while Wi-Fi is joining**: no immediate answer. The single final result is sent once, carrying the sequence of the newest request.
- **Same `sessionId` once Wi-Fi is up**: the stored `Ok` result again, under the new request's sequence.
- **Another `sessionId` while a session runs**: `Busy`.

`fileCount` (1–64) and `totalBytes` (≤ 256 MiB) are the session's budget: the receiver refuses uploads beyond either.

### Sequence

```
app                                 X4
 │── HELLO(appNonce) ───────────────►│  checks major, pairing, draws deviceNonce
 │◄─ HELLO result(deviceNonce) ──────│
 │── AUTH(counter, mac) ────────────►│  checks mac, persists counter
 │◄─ AUTH result(Ok, deviceMac) ─────│  app verifies deviceMac
 │── START_SYNC(sessionId, n, bytes)►│  joins Wi-Fi; no result yet
 │◄─ START_SYNC result(ip, port) ────│  when Wi-Fi is up and the receiver listens
 │══ HTTP uploads, one at a time ════│  each answered with a MAC'd receipt
 │── POST …/finish ─────────────────►│  204; the X4 enters Cleanup
 │◄─ (BLE) link dropped ─────────────│  receiver, Wi-Fi, then the link go down
```

After `POST /finish` the X4 tears the session down and drops the BLE link; the app sees the disconnect. The receipts it already holds are the proof of what landed. Once the X4 advertises again the app can reconnect and start the next session.

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
| `0x07` | `RateLimited` | well-formed write less than 100 ms after the last accepted one |
| `0x08` | `Busy` | another session is active, battery too low, or HELLO during a session |
| `0x09` | `ReaderActive` | a book is open; sync will not interrupt reading |
| `0x0A` | `WifiUnavailable` | no saved credentials, or join failed / timed out |
| `0x0B` | `StorageUnavailable` | SD not mounted |
| `0x0C` | `Internal` | no entropy for the nonce, or the AUTH counter could not be persisted |
| `0x0D` | `LockedOut` | five AUTH failures; try again after 60 s |
| `0x0E` | `OutOfOrder` | AUTH before HELLO |

## Pairing

Pairing is a one-time, physical step: the user opens *Settings → CrossX → Pair* on the X4, which shows a 6-digit code and advertises for 2 minutes. The app connects, performs LE Secure Connections pairing with passkey entry, and the X4 derives the 32-byte pairing key as `HKDF-SHA256(deviceSecret, "crossx-pairing-v1" ‖ appIdentity)`. The key never leaves the X4; the app stores its own copy from the pairing exchange. One app at a time in v1; pairing again replaces the key and resets the counter.

> Pairing details are a Phase 1 deliverable and may change after hardware testing. The AUTH and device-proof message formats above are final.

## HTTP API (data plane)

Base: `http://<ipv4>:<httpPort>/crossx/v1`, only while the session is active. The session key is derived from the pairing key and the nonces of the HELLO that authenticated the link which sent `START_SYNC`:

```
sessionKey = HKDF-SHA256(pairingKey, salt = sessionId, info = "crossx-session-v1" ‖ deviceNonce ‖ appNonce)   32 bytes
token      = base64(HMAC-SHA256(sessionKey, method "\n" path "\n" contentLength "\n" sha256Hex))
```

Every request carries `Authorization: CrossX <token>`. `method` is the upper-case HTTP method, `path` the request path as sent (starting with `/crossx/v1`, no query), `contentLength` the decimal body length, and `sha256Hex` the body's lowercase SHA-256 (for uploads the `X-CrossX-Sha256` value; for requests without a body `0` and the digest of the empty string). base64 is RFC 4648 with padding. A missing or wrong token is `401`.

There is no expiry field: the X4 has no trusted clock. Freshness comes from the per-session nonces, and the receiver only exists during the session. Replaying a captured token within the session only repeats that request: the same upload answers its receipt, a receipt query answers again, and a finish ends the session the app was already ending. After the session ends its key is gone.

| Method & path | Purpose | Notes |
| --- | --- | --- |
| `GET /health` | receiver is up | returns `{"state":"TransferReady","session":"<id>"}` |
| `POST /sessions/{sessionId}/files/{fileId}` | upload one EPUB | raw body; headers `Content-Length`, `X-CrossX-Sha256`, `X-CrossX-Title`. See below |
| `GET /files/{fileId}/receipt` | durable proof | `200` + receipt or `404` |
| `POST /sessions/{sessionId}/finish` | app is done | `204`; the X4 enters Cleanup and drops the BLE link |

`fileId` is 32 lowercase hex characters minted by the app per queue item and reused across retries. `sha256Hex` is 64 lowercase hex.

Upload answers. The receiver checks the request itself (token, session, id, digest, length, existing receipt) before it asks the session for admission (one upload at a time, within the declared budget), and writes nothing until both pass:

| Status | When |
| --- | --- |
| `401` | missing or wrong token |
| `404` | `sessionId` is not the running session, or the session no longer accepts uploads |
| `400` | invalid `fileId` or digest, or under 1 KiB |
| `200` + receipt | already committed with the same digest; nothing is written |
| `409` | the id exists with another digest; mint a new id |
| `429` | another upload is in flight (one at a time) |
| `413` | over 64 MiB, or past the `fileCount` / `totalBytes` declared in `START_SYNC` |
| `422` | the body's length or digest does not match the headers |
| `201` + receipt | committed |

A failed or refused upload does not count against the session budget; a committed one does. The session ends after 90 s without upload traffic.

Receipt (JSON):

```json
{"fileId":"…","sha256":"…","size":1234567,"path":"/Books/CrossX/Deep-Work-01234567.epub","mac":"…"}
```

`mac` is lowercase hex `HMAC-SHA256(sessionKey, fileId "\n" sha256 "\n" size "\n" path)`, `size` in decimal. The X4 computes it when it sends a receipt, under the key of the session serving the request; the copy on SD (`/crossx/receipts/<fileId>.meta`) holds the other four fields. The app marks an item **On Device** only after it holds a receipt whose `mac` it has verified. A TCP success, a `2xx` without a receipt body, or a receipt that does not verify is not enough.

## Security

- BLE address and device name are identification, not authentication. Only the one accepted link is served; other centrals are disconnected and cannot end or alter the session.
- Pairing needs a physical action on the X4 and LE Secure Connections.
- AUTH is mutual. The app's MAC binds both nonces and a strictly increasing counter that the X4 persists before it answers; the X4's `deviceMac` uses its own label and nonce order so neither MAC can be reflected as the other. Failures are counted and locked out.
- HTTP requests are authenticated per request under a per-session key and bound to method, path, length, and digest. There is no expiry; the per-session nonces and the receiver's lifetime (the session only) bound replay.
- Receipts are MAC'd under the session key, so a receipt the app accepts came from the X4 it authenticated.
- Everything is bounded: frames (244 B), one write per 100 ms, payload fields (fixed), file size (64 MiB), files and bytes per session (as declared, at most 64 and 256 MiB), one upload at a time, timeouts.
- HTTP over the local network is not confidential. TLS on the C3 is a measured follow-up (RAM and time per handshake) before it can be promised.
