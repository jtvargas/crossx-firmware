#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ProtocolV1.h"
#include "SyncCoordinator.h"

using namespace crossx::protocol;
using namespace crossx::sync;

namespace {

// Every effect the coordinator can ask for, recorded. The MAC is a toy: the bytes of the message
// XOR'd into a 32-byte block, enough to tell a right AUTH from a wrong one.
struct FakePorts : Ports {
  std::vector<std::string> log;
  std::vector<std::vector<uint8_t>> results;
  bool paired = true;
  bool entropy = true;
  uint32_t counter = 0;
  uint8_t nonceSeed = 1;

  void startAdvertising() override { log.push_back("adv:on"); }
  void stopAdvertising() override { log.push_back("adv:off"); }
  void disconnectCentral() override { log.push_back("ble:drop"); }
  void notifyResult(const uint8_t* frame, size_t length) override { results.emplace_back(frame, frame + length); }
  void wifiStart() override { log.push_back("wifi:start"); }
  void wifiStop() override { log.push_back("wifi:stop"); }
  void httpStart(const uint8_t (&)[kSessionIdBytes]) override { log.push_back("http:start"); }
  void httpStop() override { log.push_back("http:stop"); }
  bool randomNonce(uint8_t (&out)[kNonceBytes]) override {
    if (!entropy) return false;
    memset(out, nonceSeed++, kNonceBytes);
    return true;
  }
  bool computeAuthMac(const uint8_t* message, size_t length, uint8_t (&out)[kMacBytes]) override {
    if (!paired) return false;
    memset(out, 0, kMacBytes);
    for (size_t i = 0; i < length; ++i) out[i % kMacBytes] ^= message[i];
    return true;
  }
  uint32_t loadAuthCounter() override { return counter; }
  void storeAuthCounter(uint32_t c) override { counter = c; }
  void onStateChanged(State from, State to, Reason reason) override {
    log.push_back(std::string(stateName(from)) + ">" + stateName(to) + ":" + reasonName(reason));
  }

  bool logged(const char* entry) const {
    for (const auto& e : log) {
      if (e == entry) return true;
    }
    return false;
  }
  size_t count(const char* entry) const {
    size_t n = 0;
    for (const auto& e : log) n += e == entry;
    return n;
  }
  // The last result frame: opcode (with the result bit), sequence, and the Result code.
  Opcode lastOpcode() const { return static_cast<Opcode>(results.back()[0]); }
  uint16_t lastSequence() const { return static_cast<uint16_t>(results.back()[2] | (results.back()[3] << 8)); }
  Result lastResult() const { return static_cast<Result>(results.back()[kHeaderBytes]); }
  const uint8_t* lastPayload() const { return results.back().data() + kHeaderBytes; }
};

struct Session {
  FakePorts ports;
  SyncCoordinator sut{ports};
  uint32_t now = 1000;
  uint16_t seq = 1;
  uint8_t appNonce[kNonceBytes];
  uint8_t deviceNonce[kNonceBytes];
  uint8_t sessionId[kSessionIdBytes];

  Session() {
    memset(appNonce, 0xA5, sizeof(appNonce));
    memset(sessionId, 0x5E, sizeof(sessionId));
    Environment env;
    env.paired = true;
    env.sdMounted = true;
    env.wifiCredentialsSaved = true;
    env.batteryPercent = 80;
    sut.setEnvironment(env);
  }

  void advance(uint32_t ms) {
    now += ms;
    sut.tick(now);
  }

  void write(Opcode op, const std::vector<uint8_t>& payload) {
    uint8_t frame[kMaxFrameBytes];
    const size_t n = encodeFrame(op, seq++, payload.data(), payload.size(), frame, sizeof(frame));
    now += kMinCommandIntervalMs;  // Stay under the rate limit unless a test wants to hit it.
    sut.onControlWrite(frame, n, now);
  }

  void connect() {
    sut.requestAdvertising(now);
    sut.onCentralConnected(now);
  }

  void hello(uint8_t major = kProtocolMajor) {
    std::vector<uint8_t> p(kHelloRequestBytes, 0);
    p[0] = major;
    p[1] = 0;
    memcpy(p.data() + 6, appNonce, kNonceBytes);
    write(Opcode::Hello, p);
    if (!ports.results.empty() && ports.lastResult() == Result::Ok) {
      memcpy(deviceNonce, ports.lastPayload() + 7, kNonceBytes);
    }
  }

  std::vector<uint8_t> authPayload(uint32_t counter, bool correct = true) {
    uint8_t message[kAuthMessageBytes];
    buildAuthMessage(deviceNonce, appNonce, counter, message);
    uint8_t mac[kMacBytes];
    ports.computeAuthMac(message, sizeof(message), mac);
    if (!correct) mac[0] ^= 0xFF;
    std::vector<uint8_t> p(kAuthRequestBytes, 0);
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(counter >> (8 * i));
    memcpy(p.data() + 4, mac, kMacBytes);
    return p;
  }

  void auth(uint32_t counter = 1, bool correct = true) { write(Opcode::Auth, authPayload(counter, correct)); }

  void startSync(uint16_t files = 1, uint32_t bytes = 100000) {
    std::vector<uint8_t> p(kStartSyncRequestBytes, 0);
    memcpy(p.data(), sessionId, kSessionIdBytes);
    p[16] = static_cast<uint8_t>(files);
    p[17] = static_cast<uint8_t>(files >> 8);
    for (int i = 0; i < 4; ++i) p[18 + i] = static_cast<uint8_t>(bytes >> (8 * i));
    write(Opcode::StartSync, p);
  }

  void authenticate() {
    connect();
    hello();
    auth();
  }
};

TEST(SyncCoordinator, StartsIdleWithRadioOff) {
  Session s;
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_TRUE(s.ports.log.empty());
}

TEST(SyncCoordinator, AdvertisingTurnsTheRadioOnAndOff) {
  Session s;
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_EQ(s.ports.log.front(), "adv:on");
  s.sut.requestIdle(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_TRUE(s.ports.logged("adv:off"));
}

TEST(SyncCoordinator, UnpairedWithoutSdStaysDark) {
  Session s;
  Environment env;
  s.sut.setEnvironment(env);
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
}

TEST(SyncCoordinator, ConnectionWhileIdleIsDropped) {
  Session s;
  s.sut.onCentralConnected(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_TRUE(s.ports.logged("ble:drop"));
}

TEST(SyncCoordinator, WritesBeforeConnectionAreIgnored) {
  Session s;
  s.sut.requestAdvertising(s.now);
  s.hello();
  EXPECT_TRUE(s.ports.results.empty());
}

TEST(SyncCoordinator, HelloAnswersWithVersionAndNonce) {
  Session s;
  s.connect();
  s.hello();
  ASSERT_EQ(s.ports.results.size(), 1u);
  EXPECT_EQ(s.ports.lastOpcode(), static_cast<Opcode>(0x81));
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(s.ports.lastPayload()[1], kProtocolMajor);
  EXPECT_EQ(s.ports.lastPayload()[7 + kNonceBytes], static_cast<uint8_t>(DeviceState::Connected));
  EXPECT_EQ(s.sut.state(), State::Connected);
}

TEST(SyncCoordinator, MajorVersionMismatchFailsAndDisconnects) {
  Session s;
  s.connect();
  s.hello(2);
  EXPECT_EQ(s.ports.lastResult(), Result::UnsupportedVersion);
  EXPECT_TRUE(s.ports.logged("ble:drop"));
}

TEST(SyncCoordinator, HelloWithoutPairingSaysNotPaired) {
  Session s;
  Environment env;
  env.sdMounted = true;
  s.sut.setEnvironment(env);
  s.connect();
  s.hello();
  EXPECT_EQ(s.ports.lastResult(), Result::NotPaired);
  s.auth();
  EXPECT_EQ(s.ports.lastResult(), Result::OutOfOrder);
}

TEST(SyncCoordinator, NoEntropyNoHello) {
  Session s;
  s.ports.entropy = false;
  s.connect();
  s.hello();
  EXPECT_EQ(s.ports.lastResult(), Result::Internal);
}

TEST(SyncCoordinator, MalformedFrameGetsBadFrame) {
  Session s;
  s.connect();
  uint8_t junk[kHeaderBytes] = {0x01, 0x00, 0x2A, 0x00, 0x09, 0x00};  // claims 9 bytes
  s.sut.onControlWrite(junk, sizeof(junk), s.now);
  EXPECT_EQ(s.ports.lastResult(), Result::BadFrame);
  EXPECT_EQ(s.ports.lastSequence(), 0x2A);
}

TEST(SyncCoordinator, AuthBeforeHelloIsOutOfOrder) {
  Session s;
  s.connect();
  memset(s.deviceNonce, 0, sizeof(s.deviceNonce));
  s.auth();
  EXPECT_EQ(s.ports.lastResult(), Result::OutOfOrder);
  EXPECT_EQ(s.sut.state(), State::Connected);
}

TEST(SyncCoordinator, CorrectAuthAuthenticatesAndStoresCounter) {
  Session s;
  s.authenticate();
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(s.sut.state(), State::Authenticated);
  EXPECT_TRUE(s.sut.authenticated());
  EXPECT_EQ(s.ports.counter, 1u);
}

TEST(SyncCoordinator, WrongMacFailsAndBurnsTheNonce) {
  Session s;
  s.connect();
  s.hello();
  s.auth(1, false);
  EXPECT_EQ(s.ports.lastResult(), Result::AuthFailed);
  EXPECT_EQ(s.sut.state(), State::Connected);
  s.auth(1, true);  // right MAC, but HELLO must be repeated first
  EXPECT_EQ(s.ports.lastResult(), Result::OutOfOrder);
  s.hello();
  s.auth(1, true);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
}

TEST(SyncCoordinator, ReplayedCounterIsRefused) {
  Session s;
  s.authenticate();
  s.sut.onCentralDisconnected(s.now);
  s.sut.onCentralConnected(s.now);
  s.hello();
  s.auth(1);
  EXPECT_EQ(s.ports.lastResult(), Result::Replayed);
  s.auth(2);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
}

TEST(SyncCoordinator, FiveFailuresLockOutForAMinute) {
  Session s;
  s.connect();
  for (int i = 0; i < kMaxAuthFailures - 1; ++i) {
    s.hello();
    s.auth(1, false);
    EXPECT_EQ(s.ports.lastResult(), Result::AuthFailed);
  }
  s.hello();
  s.auth(1, false);
  EXPECT_EQ(s.ports.lastResult(), Result::LockedOut);
  EXPECT_TRUE(s.ports.logged("ble:drop"));
  s.sut.onCentralDisconnected(s.now);
  s.sut.onCentralConnected(s.now);
  s.hello();
  s.auth(1, true);
  EXPECT_EQ(s.ports.lastResult(), Result::LockedOut);
  s.advance(kLockoutMs);  // The unauthenticated link timed out meanwhile; the app reconnects.
  EXPECT_EQ(s.sut.state(), State::Advertising);
  s.sut.onCentralConnected(s.now);
  s.hello();
  s.auth(1, true);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
}

TEST(SyncCoordinator, CommandsNeedAuth) {
  Session s;
  s.connect();
  s.hello();
  s.write(Opcode::GetStatus, {});
  EXPECT_EQ(s.ports.lastResult(), Result::NotAuthenticated);
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::NotAuthenticated);
  s.write(Opcode::CancelSync, {});
  EXPECT_EQ(s.ports.lastResult(), Result::NotAuthenticated);
  EXPECT_FALSE(s.ports.logged("wifi:start"));
}

TEST(SyncCoordinator, RateLimitAnswersRateLimited) {
  Session s;
  s.authenticate();
  uint8_t frame[kMaxFrameBytes];
  const size_t n = encodeFrame(Opcode::GetStatus, 77, nullptr, 0, frame, sizeof(frame));
  s.sut.onControlWrite(frame, n, s.now + 1);
  EXPECT_EQ(s.ports.lastResult(), Result::RateLimited);
  EXPECT_EQ(s.ports.lastSequence(), 77);
}

TEST(SyncCoordinator, GetStatusReportsEnvironment) {
  Session s;
  s.authenticate();
  s.write(Opcode::GetStatus, {});
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  const uint8_t* p = s.ports.lastPayload() + 1;
  EXPECT_EQ(p[0], static_cast<uint8_t>(DeviceState::Authenticated));
  EXPECT_EQ(p[1], 80);
  EXPECT_EQ(p[2] & kStatusSdMounted, kStatusSdMounted);
  EXPECT_EQ(p[2] & kStatusWifiUp, 0);
  EXPECT_EQ(p[2] & kStatusPaired, kStatusPaired);
}

TEST(SyncCoordinator, HappyPathSession) {
  Session s;
  s.authenticate();
  const size_t before = s.ports.results.size();
  s.startSync(2, 200000);
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
  EXPECT_TRUE(s.ports.logged("wifi:start"));
  EXPECT_EQ(s.ports.results.size(), before);  // The result waits for the address.

  s.sut.onWifiUp(0xC0A80105, 8080, s.now);
  EXPECT_EQ(s.sut.state(), State::TransferReady);
  EXPECT_TRUE(s.ports.logged("http:start"));
  ASSERT_EQ(s.ports.results.size(), before + 1);
  EXPECT_EQ(s.ports.lastOpcode(), static_cast<Opcode>(0x84));
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(0, memcmp(s.ports.lastPayload() + 1, s.sessionId, kSessionIdBytes));
  EXPECT_EQ(s.ports.lastPayload()[1 + kSessionIdBytes], 0x05);
  EXPECT_EQ(s.ports.lastPayload()[5 + kSessionIdBytes], 0x90);

  s.sut.onUploadStarted(s.now);
  EXPECT_EQ(s.sut.state(), State::Transferring);
  s.sut.onUploadCommitted(s.now);
  s.sut.onUploadCommitted(s.now);
  s.sut.onSessionFinishRequested(s.now);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_TRUE(s.ports.logged("http:stop"));
  EXPECT_TRUE(s.ports.logged("wifi:stop"));
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_FALSE(s.sut.authenticated());
}

TEST(SyncCoordinator, StartSyncIsIdempotentForTheSameSession) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onWifiUp(0x0A000001, 80, s.now);
  const size_t n = s.ports.results.size();
  s.startSync();
  EXPECT_EQ(s.ports.results.size(), n + 1);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(s.ports.count("wifi:start"), 1u);
  s.sessionId[0] ^= 1;
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::Busy);
}

TEST(SyncCoordinator, StartSyncRefusalsDoNotTouchWifi) {
  Session s;
  s.authenticate();
  Environment env;
  env.paired = true;
  env.sdMounted = true;
  env.wifiCredentialsSaved = true;
  env.batteryPercent = 80;

  env.readerOpen = true;
  s.sut.setEnvironment(env);
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::ReaderActive);
  env.readerOpen = false;

  env.sdMounted = false;
  s.sut.setEnvironment(env);
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::StorageUnavailable);
  env.sdMounted = true;

  env.wifiCredentialsSaved = false;
  s.sut.setEnvironment(env);
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::WifiUnavailable);
  env.wifiCredentialsSaved = true;

  env.batteryPercent = kMinBatteryPercentForSync - 1;
  s.sut.setEnvironment(env);
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::Busy);
  env.batteryPercent = 80;

  s.sut.setEnvironment(env);
  s.startSync(0);
  EXPECT_EQ(s.ports.lastResult(), Result::BadFrame);
  s.startSync(kMaxFilesPerSession + 1);
  EXPECT_EQ(s.ports.lastResult(), Result::BadFrame);
  s.startSync(1, kMaxSessionBytes + 1);
  EXPECT_EQ(s.ports.lastResult(), Result::BadFrame);

  EXPECT_FALSE(s.ports.logged("wifi:start"));
  EXPECT_EQ(s.sut.state(), State::Authenticated);
}

TEST(SyncCoordinator, WifiFailureReportsAndRecoversToAdvertising) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onWifiFailed(s.now);
  EXPECT_EQ(s.ports.lastResult(), Result::WifiUnavailable);
  EXPECT_EQ(s.sut.state(), State::Error);
  EXPECT_TRUE(s.ports.logged("wifi:stop"));
  s.advance(kErrorHoldMs);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
}

TEST(SyncCoordinator, WifiConnectTimeout) {
  Session s;
  s.authenticate();
  s.startSync();
  s.advance(kWifiConnectMs - 1);
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
  s.advance(1);
  EXPECT_EQ(s.sut.state(), State::Error);
  EXPECT_EQ(s.ports.lastResult(), Result::WifiUnavailable);
}

TEST(SyncCoordinator, CancelTearsDownASession) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onWifiUp(1, 80, s.now);
  s.write(Opcode::CancelSync, {});
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_TRUE(s.ports.logged("http:stop"));
}

TEST(SyncCoordinator, BleDropDuringTransferKeepsTheSession) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onWifiUp(1, 80, s.now);
  s.sut.onUploadStarted(s.now);
  s.sut.onCentralDisconnected(s.now);
  EXPECT_EQ(s.sut.state(), State::Transferring);
  EXPECT_FALSE(s.ports.logged("http:stop"));
}

TEST(SyncCoordinator, BleDropWhileJoiningWifiAbortsTheSession) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onCentralDisconnected(s.now);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_TRUE(s.ports.logged("wifi:stop"));
}

TEST(SyncCoordinator, TransferIdleTimeoutCleansUp) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onWifiUp(1, 80, s.now);
  s.advance(kTransferIdleMs - 1);
  s.sut.onUploadStarted(s.now);  // Activity resets the clock.
  s.advance(kTransferIdleMs - 1);
  EXPECT_EQ(s.sut.state(), State::Transferring);
  s.advance(1);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
}

TEST(SyncCoordinator, UnauthenticatedCentralIsDroppedAfterTimeout) {
  Session s;
  s.connect();
  s.advance(kConnectedUnauthMs);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_TRUE(s.ports.logged("ble:drop"));
}

TEST(SyncCoordinator, AuthenticatedIdleIsDroppedAfterTimeout) {
  Session s;
  s.authenticate();
  s.advance(kAuthIdleMs);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_FALSE(s.sut.authenticated());
}

TEST(SyncCoordinator, IdleRequestDuringSessionCleansUpThenGoesIdle) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onWifiUp(1, 80, s.now);
  s.sut.requestIdle(s.now);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_TRUE(s.ports.logged("adv:off"));
}

TEST(SyncCoordinator, SecondCentralDuringSessionIsDropped) {
  Session s;
  s.authenticate();
  s.startSync();
  const size_t drops = s.ports.count("ble:drop");
  s.sut.onCentralConnected(s.now);
  EXPECT_EQ(s.ports.count("ble:drop"), drops + 1);
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
}

TEST(SyncCoordinator, StatusShowsWifiOnlyWhileUp) {
  Session s;
  s.authenticate();
  EXPECT_EQ(s.sut.status().flags & kStatusWifiUp, 0);
  s.startSync();
  s.sut.onWifiUp(1, 80, s.now);
  EXPECT_EQ(s.sut.status().flags & kStatusWifiUp, kStatusWifiUp);
  s.sut.onSessionFinishRequested(s.now);
  EXPECT_EQ(s.sut.status().flags & kStatusWifiUp, 0);
}

}  // namespace
