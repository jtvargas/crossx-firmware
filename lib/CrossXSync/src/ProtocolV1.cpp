#include "ProtocolV1.h"

#include <cstring>

namespace crossx::protocol {
namespace {

void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>(value >> 8);
}

void putU32(uint8_t* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint16_t getU16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

bool knownOpcode(uint8_t raw) {
  const uint8_t request = raw & ~static_cast<uint8_t>(Opcode::ResultBit);
  return request >= static_cast<uint8_t>(Opcode::Hello) && request <= static_cast<uint8_t>(Opcode::CancelSync);
}

}  // namespace

ParseStatus parseFrame(const uint8_t* bytes, size_t length, Frame& out) {
  if (bytes == nullptr || length < kHeaderBytes) return ParseStatus::TooShort;
  if (length > kMaxFrameBytes) return ParseStatus::TooLong;
  if (!knownOpcode(bytes[0])) return ParseStatus::UnknownOpcode;
  if (bytes[1] != 0) return ParseStatus::ReservedFlags;
  const uint16_t payloadLength = getU16(bytes + 4);
  if (static_cast<size_t>(payloadLength) + kHeaderBytes != length) return ParseStatus::LengthMismatch;
  out.header.opcode = static_cast<Opcode>(bytes[0]);
  out.header.flags = bytes[1];
  out.header.sequence = getU16(bytes + 2);
  out.header.payloadLength = payloadLength;
  out.payload = payloadLength ? bytes + kHeaderBytes : nullptr;
  return ParseStatus::Ok;
}

size_t encodeFrame(Opcode opcode, uint16_t sequence, const uint8_t* payload, size_t payloadLength, uint8_t* out,
                   size_t capacity) {
  if (payloadLength > kMaxPayloadBytes) return 0;
  const size_t total = kHeaderBytes + payloadLength;
  if (out == nullptr || capacity < total) return 0;
  out[0] = static_cast<uint8_t>(opcode);
  out[1] = 0;
  putU16(out + 2, sequence);
  putU16(out + 4, static_cast<uint16_t>(payloadLength));
  if (payloadLength) memcpy(out + kHeaderBytes, payload, payloadLength);
  return total;
}

bool decodeHelloRequest(const uint8_t* payload, size_t length, HelloRequest& out) {
  if (payload == nullptr || length != kHelloRequestBytes) return false;
  out.major = payload[0];
  out.minor = payload[1];
  out.features = getU32(payload + 2);
  memcpy(out.appNonce, payload + 6, kNonceBytes);
  return true;
}

size_t encodeHelloResult(const HelloResult& in, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kHelloResultBytes) return 0;
  out[0] = static_cast<uint8_t>(in.result);
  out[1] = in.major;
  out[2] = in.minor;
  putU32(out + 3, in.features);
  memcpy(out + 7, in.deviceNonce, kNonceBytes);
  out[7 + kNonceBytes] = static_cast<uint8_t>(in.state);
  return kHelloResultBytes;
}

bool decodeAuthRequest(const uint8_t* payload, size_t length, AuthRequest& out) {
  if (payload == nullptr || length != kAuthRequestBytes) return false;
  out.counter = getU32(payload);
  memcpy(out.mac, payload + 4, kMacBytes);
  return true;
}

bool decodeStartSyncRequest(const uint8_t* payload, size_t length, StartSyncRequest& out) {
  if (payload == nullptr || length != kStartSyncRequestBytes) return false;
  memcpy(out.sessionId, payload, kSessionIdBytes);
  out.fileCount = getU16(payload + kSessionIdBytes);
  out.totalBytes = getU32(payload + kSessionIdBytes + 2);
  return true;
}

size_t encodeStartSyncResult(const StartSyncResult& in, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kStartSyncResultBytes) return 0;
  out[0] = static_cast<uint8_t>(in.result);
  memcpy(out + 1, in.sessionId, kSessionIdBytes);
  putU32(out + 1 + kSessionIdBytes, in.ipv4);
  putU16(out + 5 + kSessionIdBytes, in.httpPort);
  putU16(out + 7 + kSessionIdBytes, in.ttlSeconds);
  return kStartSyncResultBytes;
}

size_t encodeStatus(const StatusPayload& in, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kStatusPayloadBytes) return 0;
  out[0] = static_cast<uint8_t>(in.state);
  out[1] = in.batteryPercent;
  out[2] = in.flags;
  putU32(out + 3, in.freeHeap);
  putU32(out + 7, in.largestFreeBlock);
  putU16(out + 11, in.pendingReceipts);
  return kStatusPayloadBytes;
}

size_t encodeResult(Result result, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < 1) return 0;
  out[0] = static_cast<uint8_t>(result);
  return 1;
}

size_t buildAuthMessage(const uint8_t (&deviceNonce)[kNonceBytes], const uint8_t (&appNonce)[kNonceBytes],
                        uint32_t counter, uint8_t (&out)[kAuthMessageBytes]) {
  static constexpr char kLabel[14] = {'C', 'R', 'O', 'S', 'S', 'X', '-', 'A', 'U', 'T', 'H', '-', 'v', '1'};
  memcpy(out, kLabel, sizeof(kLabel));
  memcpy(out + sizeof(kLabel), deviceNonce, kNonceBytes);
  memcpy(out + sizeof(kLabel) + kNonceBytes, appNonce, kNonceBytes);
  putU32(out + sizeof(kLabel) + 2 * kNonceBytes, counter);
  return kAuthMessageBytes;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length) {
  if (a == nullptr || b == nullptr) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < length; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace crossx::protocol
