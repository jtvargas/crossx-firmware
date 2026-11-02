#pragma once

#include <cstddef>
#include <cstdint>

// CrossX BLE control protocol, version 1. Pure encoding and validation: no radio, no heap, no
// Arduino. The firmware's GATT server and the host tests both go through these functions, so
// every bound the protocol document promises (docs/protocol-v1.md) is enforced here, once.
//
// A frame is a fixed 6-byte header followed by a payload whose length the header states:
//   [opcode u8][flags u8][sequence u16 LE][payloadLength u16 LE][payload ...]
// The whole frame must fit one ATT write at the negotiated MTU (247 - 3 = 244 bytes).
namespace crossx::protocol {

inline constexpr uint8_t kProtocolMajor = 1;
inline constexpr uint8_t kProtocolMinor = 0;

inline constexpr size_t kHeaderBytes = 6;
inline constexpr size_t kMaxFrameBytes = 244;
inline constexpr size_t kMaxPayloadBytes = kMaxFrameBytes - kHeaderBytes;
inline constexpr size_t kNonceBytes = 16;
inline constexpr size_t kMacBytes = 32;
inline constexpr size_t kSessionIdBytes = 16;
inline constexpr size_t kFileIdBytes = 16;

// 128-bit GATT UUIDs, text form as NimBLE takes them. The base is fixed for protocol v1; a
// future incompatible protocol changes the service UUID rather than the payloads.
inline constexpr const char* kServiceUuid = "c0a5e000-6b5c-4f1e-9a2d-8c1f3e5b7d01";
inline constexpr const char* kProtocolInfoUuid = "c0a5e001-6b5c-4f1e-9a2d-8c1f3e5b7d01";
inline constexpr const char* kDeviceStatusUuid = "c0a5e002-6b5c-4f1e-9a2d-8c1f3e5b7d01";
inline constexpr const char* kControlUuid = "c0a5e003-6b5c-4f1e-9a2d-8c1f3e5b7d01";
inline constexpr const char* kControlResultUuid = "c0a5e004-6b5c-4f1e-9a2d-8c1f3e5b7d01";

// Requests the app writes to the control characteristic. Values are part of the wire format.
enum class Opcode : uint8_t {
  Hello = 0x01,
  Auth = 0x02,
  GetStatus = 0x03,
  StartSync = 0x04,
  CancelSync = 0x05,
  // Results are notified on controlResult with the request's opcode OR'd with this bit.
  ResultBit = 0x80,
};

// Result codes carried in the first payload byte of every result frame.
enum class Result : uint8_t {
  Ok = 0x00,
  BadFrame = 0x01,
  UnsupportedVersion = 0x02,
  NotPaired = 0x03,
  NotAuthenticated = 0x04,
  AuthFailed = 0x05,
  Replayed = 0x06,
  RateLimited = 0x07,
  Busy = 0x08,
  ReaderActive = 0x09,
  WifiUnavailable = 0x0A,
  StorageUnavailable = 0x0B,
  Internal = 0x0C,
  LockedOut = 0x0D,
  OutOfOrder = 0x0E,
};

// Device state as reported in status payloads. Mirrors crossx::sync::State without exposing it.
enum class DeviceState : uint8_t {
  Idle = 0,
  Advertising = 1,
  Connected = 2,
  Authenticated = 3,
  WifiConnecting = 4,
  TransferReady = 5,
  Transferring = 6,
  Finishing = 7,
  Cleanup = 8,
  Error = 9,
};

// Capability bits in Hello (app) and HelloResult (device). Unknown bits are ignored.
enum Feature : uint32_t {
  kFeatureWifiStation = 1u << 0,
  kFeatureReceipts = 1u << 1,
  kFeatureCancel = 1u << 2,
};

struct Header {
  Opcode opcode;
  uint8_t flags;
  uint16_t sequence;
  uint16_t payloadLength;
};

struct Frame {
  Header header;
  const uint8_t* payload;  // Points into the caller's buffer; valid while that buffer is.
};

enum class ParseStatus : uint8_t {
  Ok,
  TooShort,
  TooLong,
  LengthMismatch,
  UnknownOpcode,
  ReservedFlags,
};

// Validates the header against the buffer; never reads past `length`.
ParseStatus parseFrame(const uint8_t* bytes, size_t length, Frame& out);

// Writes header + payload into `out`; returns bytes written or 0 when it does not fit.
size_t encodeFrame(Opcode opcode, uint16_t sequence, const uint8_t* payload, size_t payloadLength,
                   uint8_t* out, size_t capacity);

// --- Payloads. Fixed layouts, little-endian, decoded with explicit bounds. ---

struct HelloRequest {
  uint8_t major;
  uint8_t minor;
  uint32_t features;
  uint8_t appNonce[kNonceBytes];
};
inline constexpr size_t kHelloRequestBytes = 2 + 4 + kNonceBytes;

struct HelloResult {
  Result result;
  uint8_t major;
  uint8_t minor;
  uint32_t features;
  uint8_t deviceNonce[kNonceBytes];
  DeviceState state;
};
inline constexpr size_t kHelloResultBytes = 1 + 2 + 4 + kNonceBytes + 1;

struct AuthRequest {
  uint32_t counter;  // Strictly increasing per pairing; the device stores the last accepted value.
  uint8_t mac[kMacBytes];
};
inline constexpr size_t kAuthRequestBytes = 4 + kMacBytes;

struct StartSyncRequest {
  uint8_t sessionId[kSessionIdBytes];
  uint16_t fileCount;
  uint32_t totalBytes;
};
inline constexpr size_t kStartSyncRequestBytes = kSessionIdBytes + 2 + 4;

struct StartSyncResult {
  Result result;
  uint8_t sessionId[kSessionIdBytes];
  uint32_t ipv4;  // Host order; 0 while Wi-Fi is still connecting.
  uint16_t httpPort;
  uint16_t ttlSeconds;  // How long the HTTP receiver stays up without traffic.
};
inline constexpr size_t kStartSyncResultBytes = 1 + kSessionIdBytes + 4 + 2 + 2;

struct StatusPayload {
  DeviceState state;
  uint8_t batteryPercent;
  uint8_t flags;  // bit0 SD mounted, bit1 Wi-Fi up, bit2 reader open, bit3 paired
  uint32_t freeHeap;
  uint32_t largestFreeBlock;
  uint16_t pendingReceipts;
};
inline constexpr size_t kStatusPayloadBytes = 1 + 1 + 1 + 4 + 4 + 2;
inline constexpr uint8_t kStatusSdMounted = 1u << 0;
inline constexpr uint8_t kStatusWifiUp = 1u << 1;
inline constexpr uint8_t kStatusReaderOpen = 1u << 2;
inline constexpr uint8_t kStatusPaired = 1u << 3;

bool decodeHelloRequest(const uint8_t* payload, size_t length, HelloRequest& out);
size_t encodeHelloResult(const HelloResult& in, uint8_t* out, size_t capacity);
bool decodeAuthRequest(const uint8_t* payload, size_t length, AuthRequest& out);
bool decodeStartSyncRequest(const uint8_t* payload, size_t length, StartSyncRequest& out);
size_t encodeStartSyncResult(const StartSyncResult& in, uint8_t* out, size_t capacity);
size_t encodeStatus(const StatusPayload& in, uint8_t* out, size_t capacity);
// Every other result is one byte: the Result code.
size_t encodeResult(Result result, uint8_t* out, size_t capacity);

// The bytes the app and the device both MAC for AUTH: a fixed label, both nonces, the counter.
// The MAC itself (HMAC-SHA256 under the pairing key) is supplied by the platform layer.
inline constexpr size_t kAuthMessageBytes = 14 + kNonceBytes + kNonceBytes + 4;
size_t buildAuthMessage(const uint8_t (&deviceNonce)[kNonceBytes], const uint8_t (&appNonce)[kNonceBytes],
                        uint32_t counter, uint8_t (&out)[kAuthMessageBytes]);

// Constant-time comparison, so AUTH failures do not leak how many bytes matched.
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length);

// A device accepts a client whose major matches; minor differences are negotiated down.
constexpr bool versionCompatible(uint8_t major) { return major == kProtocolMajor; }

}  // namespace crossx::protocol
