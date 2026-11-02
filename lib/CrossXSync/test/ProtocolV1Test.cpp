#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "ProtocolV1.h"

using namespace crossx::protocol;

namespace {

std::vector<uint8_t> frameOf(Opcode op, uint16_t seq, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out(kMaxFrameBytes);
  const size_t n = encodeFrame(op, seq, payload.data(), payload.size(), out.data(), out.size());
  out.resize(n);
  return out;
}

TEST(ProtocolV1, HeaderRoundTrip) {
  const std::vector<uint8_t> payload{1, 2, 3};
  const auto bytes = frameOf(Opcode::GetStatus, 0xBEEF, payload);
  ASSERT_EQ(bytes.size(), kHeaderBytes + 3);
  Frame f{};
  ASSERT_EQ(parseFrame(bytes.data(), bytes.size(), f), ParseStatus::Ok);
  EXPECT_EQ(f.header.opcode, Opcode::GetStatus);
  EXPECT_EQ(f.header.sequence, 0xBEEF);
  EXPECT_EQ(f.header.payloadLength, 3);
  EXPECT_EQ(0, memcmp(f.payload, payload.data(), 3));
}

TEST(ProtocolV1, EmptyPayloadHasNullPointer) {
  const auto bytes = frameOf(Opcode::GetStatus, 1, {});
  Frame f{};
  ASSERT_EQ(parseFrame(bytes.data(), bytes.size(), f), ParseStatus::Ok);
  EXPECT_EQ(f.payload, nullptr);
}

TEST(ProtocolV1, RejectsShortLongAndMismatchedFrames) {
  Frame f{};
  uint8_t tiny[3] = {1, 0, 0};
  EXPECT_EQ(parseFrame(tiny, sizeof(tiny), f), ParseStatus::TooShort);
  EXPECT_EQ(parseFrame(nullptr, 100, f), ParseStatus::TooShort);

  std::vector<uint8_t> huge(kMaxFrameBytes + 1, 0);
  huge[0] = 1;
  EXPECT_EQ(parseFrame(huge.data(), huge.size(), f), ParseStatus::TooLong);

  auto bytes = frameOf(Opcode::Hello, 1, {9, 9});
  bytes[4] = 5;  // claims 5 bytes, carries 2
  EXPECT_EQ(parseFrame(bytes.data(), bytes.size(), f), ParseStatus::LengthMismatch);
  bytes[4] = 2;
  bytes[1] = 0x80;
  EXPECT_EQ(parseFrame(bytes.data(), bytes.size(), f), ParseStatus::ReservedFlags);
  bytes[1] = 0;
  bytes[0] = 0x55;
  EXPECT_EQ(parseFrame(bytes.data(), bytes.size(), f), ParseStatus::UnknownOpcode);
}

TEST(ProtocolV1, EncodeRefusesOversizedPayloadAndSmallBuffer) {
  std::vector<uint8_t> payload(kMaxPayloadBytes + 1, 0);
  uint8_t out[kMaxFrameBytes];
  EXPECT_EQ(encodeFrame(Opcode::Hello, 0, payload.data(), payload.size(), out, sizeof(out)), 0u);
  payload.resize(kMaxPayloadBytes);
  EXPECT_EQ(encodeFrame(Opcode::Hello, 0, payload.data(), payload.size(), out, sizeof(out)), kMaxFrameBytes);
  EXPECT_EQ(encodeFrame(Opcode::Hello, 0, payload.data(), payload.size(), out, sizeof(out) - 1), 0u);
}

TEST(ProtocolV1, HelloRequestDecodesOnlyExactLength) {
  std::vector<uint8_t> p(kHelloRequestBytes, 0);
  p[0] = 1;
  p[1] = 3;
  p[2] = 0x07;  // features little-endian
  for (size_t i = 0; i < kNonceBytes; ++i) p[6 + i] = static_cast<uint8_t>(i);
  HelloRequest req{};
  ASSERT_TRUE(decodeHelloRequest(p.data(), p.size(), req));
  EXPECT_EQ(req.major, 1);
  EXPECT_EQ(req.minor, 3);
  EXPECT_EQ(req.features, 7u);
  EXPECT_EQ(req.appNonce[15], 15);
  EXPECT_FALSE(decodeHelloRequest(p.data(), p.size() - 1, req));
  EXPECT_FALSE(decodeHelloRequest(p.data(), p.size() + 1, req));
}

TEST(ProtocolV1, StartSyncRoundTripIsLittleEndian) {
  std::vector<uint8_t> p(kStartSyncRequestBytes, 0xAA);
  p[16] = 0x02;
  p[17] = 0x01;  // fileCount 0x0102
  p[18] = 0x78;
  p[19] = 0x56;
  p[20] = 0x34;
  p[21] = 0x12;  // totalBytes 0x12345678
  StartSyncRequest req{};
  ASSERT_TRUE(decodeStartSyncRequest(p.data(), p.size(), req));
  EXPECT_EQ(req.fileCount, 0x0102);
  EXPECT_EQ(req.totalBytes, 0x12345678u);

  StartSyncResult res{};
  res.result = Result::Ok;
  memset(res.sessionId, 0xCD, kSessionIdBytes);
  res.ipv4 = 0xC0A80164;  // 192.168.1.100
  res.httpPort = 8080;
  res.ttlSeconds = 90;
  uint8_t out[kStartSyncResultBytes];
  ASSERT_EQ(encodeStartSyncResult(res, out, sizeof(out)), kStartSyncResultBytes);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1 + kSessionIdBytes], 0x64);
  EXPECT_EQ(out[4 + kSessionIdBytes], 0xC0);
  EXPECT_EQ(out[5 + kSessionIdBytes], 0x90);
  EXPECT_EQ(out[6 + kSessionIdBytes], 0x1F);
  EXPECT_EQ(encodeStartSyncResult(res, out, sizeof(out) - 1), 0u);
}

TEST(ProtocolV1, AuthMessageBindsLabelNoncesAndCounter) {
  uint8_t device[kNonceBytes], app[kNonceBytes];
  memset(device, 0x11, sizeof(device));
  memset(app, 0x22, sizeof(app));
  uint8_t m1[kAuthMessageBytes], m2[kAuthMessageBytes];
  EXPECT_EQ(buildAuthMessage(device, app, 5, m1), kAuthMessageBytes);
  EXPECT_EQ(0, memcmp(m1, "CROSSX-AUTH-v1", 14));
  EXPECT_EQ(m1[14], 0x11);
  EXPECT_EQ(m1[14 + kNonceBytes], 0x22);
  EXPECT_EQ(m1[kAuthMessageBytes - 4], 5);
  buildAuthMessage(device, app, 6, m2);
  EXPECT_NE(0, memcmp(m1, m2, sizeof(m1)));
  buildAuthMessage(app, device, 5, m2);
  EXPECT_NE(0, memcmp(m1, m2, sizeof(m1)));
}

TEST(ProtocolV1, ConstantTimeEqual) {
  uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4}, c[4] = {1, 2, 3, 5};
  EXPECT_TRUE(constantTimeEqual(a, b, 4));
  EXPECT_FALSE(constantTimeEqual(a, c, 4));
  EXPECT_FALSE(constantTimeEqual(nullptr, b, 4));
}

TEST(ProtocolV1, EveryPayloadFitsOneFrame) {
  EXPECT_LE(kHelloRequestBytes, kMaxPayloadBytes);
  EXPECT_LE(kHelloResultBytes, kMaxPayloadBytes);
  EXPECT_LE(kAuthRequestBytes, kMaxPayloadBytes);
  EXPECT_LE(kStartSyncRequestBytes, kMaxPayloadBytes);
  EXPECT_LE(kStartSyncResultBytes, kMaxPayloadBytes);
  EXPECT_LE(1 + kStatusPayloadBytes, kMaxPayloadBytes);
}

}  // namespace
