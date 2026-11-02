#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ProtocolV1.h"
#include "SyncCoordinator.h"

using namespace crossx::protocol;
using namespace crossx::sync;

namespace {

constexpr uint16_t kApp = 1;    // The app's link.
constexpr uint16_t kOther = 2;  // Anyone else's: a second phone, a rejected central.

// Every effect the coordinator can ask for, recorded in order. The MAC is a toy: the bytes of the
// message XOR'd into a 32-byte block, enough to tell a right AUTH from a wrong one.
struct FakePorts : Ports {
  std::vector<std::string> log;
  std::vector<std::vector<uint8_t>> results;
  bool advertising = false;
  bool paired = true;
  bool entropy = true;
  bool counterStoreWorks = true;
  uint32_t counter = 0;  // What NVS holds.
  uint8_t nonceSeed = 1;

  void startAdvertising() override {
    advertising = true;
    log.push_back("adv:on");
  }
  void stopAdvertising() override {
    advertising = false;
    log.push_back("adv:off");
  }
  void disconnectCentral(uint16_t connHandle) override { log.push_back("ble:drop:" + std::to_string(connHandle)); }
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
  bool storeAuthCounter(uint32_t c) override {
    if (!counterStoreWorks) return false;
    counter = c;
    return true;
  }
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
  std::vector<std::string> logSince(size_t mark) const { return {log.begin() + mark, log.end()}; }
  // The last result frame: opcode (with the result bit), sequence, and the Result code.
  Opcode lastOpcode() const { return static_cast<Opcode>(results.back()[0]); }
  uint16_t lastSequence() const { return static_cast<uint16_t>(results.back()[2] | (results.back()[3] << 8)); }
  Result lastResult() const { return static_cast<Result>(results.back()[kHeaderBytes]); }
  const uint8_t* lastPayload() const { return results.back().data() + kHeaderBytes; }
  size_t lastPayloadLength() const { return results.back().size() - kHeaderBytes; }
};

struct Session {
  FakePorts ports;
  SyncCoordinator sut{ports};
  Environment env;
  uint32_t now = 1000;
  uint16_t seq = 1;
  uint8_t appNonce[kNonceBytes];
  uint8_t deviceNonce[kNonceBytes];
  uint8_t sessionId[kSessionIdBytes];

  Session() {
    memset(appNonce, 0xA5, sizeof(appNonce));
    memset(sessionId, 0x5E, sizeof(sessionId));
    env.paired = true;
    env.sdMounted = true;
    env.wifiCredentialsSaved = true;
    env.batteryPercent = 80;
    env.freeHeap = 200000;
    env.largestFreeBlock = 60000;
    apply();
  }

  void apply() { sut.setEnvironment(env); }

  void advance(uint32_t ms) {
    now += ms;
    sut.tick(now);
  }

  void write(Opcode op, const std::vector<uint8_t>& payload) {
    uint8_t frame[kMaxFrameBytes];
    const size_t n = encodeFrame(op, seq++, payload.data(), payload.size(), frame, sizeof(frame));
    now += kMinCommandIntervalMs;  // Stay under the rate limit unless a test wants to hit it.
    sut.onControlWrite(kApp, frame, n, now);
  }

  void connect() {
    sut.requestAdvertising(now);
    sut.onCentralConnected(kApp, now);
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

  void wifiUp() {
    const uint8_t ip[kIpv4Bytes] = {192, 168, 1, 5};
    sut.onWifiUp(ip, 8080, now);
  }

  void authenticate() {
    connect();
    hello();
    auth();
  }

  void transferReady(uint16_t files = 1, uint32_t bytes = 100000) {
    authenticate();
    startSync(files, bytes);
    wifiUp();
  }

  // The radio advertises iff the coordinator is in Advertising.
  bool advertisingMatchesState() const { return ports.advertising == (sut.state() == State::Advertising); }
};

// --- Advertising -------------------------------------------------------------------------------

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
  EXPECT_FALSE(s.ports.advertising);
}

TEST(SyncCoordinator, NoSdCardStaysDark) {
  Session s;
  s.env.sdMounted = false;
  s.apply();
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_EQ(s.sut.lastReason(), Reason::StorageFailed);
  EXPECT_FALSE(s.ports.logged("adv:on"));
}

TEST(SyncCoordinator, UnpairedDeviceAdvertisesForPairing) {
  Session s;
  s.env.paired = false;
  s.apply();
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
}

TEST(SyncCoordinator, AdvertisingNeedsRoomForTheBleHostAndTheReadersBlock) {
  Session s;
  s.env.bleHostRunning = false;
  s.env.freeHeap = kBleHostBytes + kReaderLargestBlockBytes - 1;
  s.env.largestFreeBlock = kReaderLargestBlockBytes;
  s.apply();
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_EQ(s.sut.lastReason(), Reason::LowMemory);

  s.env.freeHeap = kBleHostBytes + kReaderLargestBlockBytes;
  s.env.largestFreeBlock = kReaderLargestBlockBytes - 1;
  s.apply();
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_EQ(s.sut.lastReason(), Reason::LowMemory);
  EXPECT_FALSE(s.ports.logged("adv:on"));

  s.env.largestFreeBlock = kReaderLargestBlockBytes;
  s.apply();
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
}

TEST(SyncCoordinator, RunningBleHostOnlyNeedsTheReadersBlock) {
  Session s;
  s.env.bleHostRunning = true;
  s.env.freeHeap = kReaderLargestBlockBytes;
  s.env.largestFreeBlock = kReaderLargestBlockBytes - 1;
  s.apply();
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_EQ(s.sut.lastReason(), Reason::LowMemory);

  s.env.largestFreeBlock = kReaderLargestBlockBytes;
  s.apply();
  s.sut.requestAdvertising(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
}

TEST(SyncCoordinator, AdvertisingStopsOnConnectAndRestartsWhenTheLinkCloses) {
  Session s;
  s.connect();
  EXPECT_EQ(s.sut.state(), State::Connected);
  EXPECT_TRUE(s.advertisingMatchesState());
  s.sut.onCentralDisconnected(kApp, s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_TRUE(s.advertisingMatchesState());
  EXPECT_EQ(s.ports.count("adv:on"), 2u);
}

TEST(SyncCoordinator, IdleFromAConnectedLinkStaysDarkWhenTheLinkCloses) {
  Session s;
  s.connect();
  s.sut.requestIdle(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_TRUE(s.ports.logged("ble:drop:1"));
  s.sut.onCentralDisconnected(kApp, s.now);  // The drop completes later, as it does on the radio.
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_FALSE(s.ports.advertising);
  EXPECT_EQ(s.ports.count("adv:on"), 1u);
}

TEST(SyncCoordinator, AdvertisingStaysOffForTheSessionAndResumesAfterCleanup) {
  Session s;
  s.authenticate();
  s.startSync();
  EXPECT_TRUE(s.advertisingMatchesState());
  s.wifiUp();
  EXPECT_TRUE(s.advertisingMatchesState());
  s.sut.onSessionFinishRequested(s.now);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_TRUE(s.advertisingMatchesState());
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_TRUE(s.advertisingMatchesState());
  EXPECT_EQ(s.ports.count("adv:on"), 2u);
}

TEST(SyncCoordinator, CleanupGoesIdleWhenAdvertisingNoLongerFits) {
  Session s;
  s.transferReady();
  s.sut.onSessionFinishRequested(s.now);
  s.env.largestFreeBlock = kReaderLargestBlockBytes - 1;
  s.apply();
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_EQ(s.sut.lastReason(), Reason::LowMemory);
  EXPECT_FALSE(s.ports.advertising);
}

// --- Links -------------------------------------------------------------------------------------

TEST(SyncCoordinator, ConnectionWhileIdleIsDropped) {
  Session s;
  s.sut.onCentralConnected(kApp, s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_TRUE(s.ports.logged("ble:drop:1"));
}

TEST(SyncCoordinator, WritesBeforeConnectionAreIgnored) {
  Session s;
  s.sut.requestAdvertising(s.now);
  s.hello();
  EXPECT_TRUE(s.ports.results.empty());
}

TEST(SyncCoordinator, OtherLinksCannotTouchTheSession) {
  Session s;
  s.authenticate();
  s.sut.onCentralConnected(kOther, s.now);
  EXPECT_TRUE(s.ports.logged("ble:drop:2"));
  s.sut.onCentralDisconnected(kOther, s.now);
  EXPECT_EQ(s.sut.state(), State::Authenticated);
  EXPECT_TRUE(s.sut.authenticated());

  s.startSync();
  s.sut.onCentralConnected(kOther, s.now);
  s.sut.onCentralDisconnected(kOther, s.now);
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
  EXPECT_FALSE(s.ports.logged("wifi:stop"));

  // A write from another link is neither answered nor obeyed.
  const size_t results = s.ports.results.size();
  uint8_t frame[kMaxFrameBytes];
  const size_t n = encodeFrame(Opcode::CancelSync, 9, nullptr, 0, frame, sizeof(frame));
  s.sut.onControlWrite(kOther, frame, n, s.now + 1000);
  EXPECT_EQ(s.ports.results.size(), results);
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
  EXPECT_EQ(s.ports.count("ble:drop:1"), 0u);
}

// --- HELLO and AUTH ----------------------------------------------------------------------------

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
  EXPECT_TRUE(s.ports.logged("ble:drop:1"));
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_EQ(s.sut.lastReason(), Reason::VersionMismatch);
  EXPECT_TRUE(s.advertisingMatchesState());
}

TEST(SyncCoordinator, HelloWithoutPairingSaysNotPaired) {
  Session s;
  s.env.paired = false;
  s.apply();
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

TEST(SyncCoordinator, HelloDuringASessionIsBusyAndChangesNothing) {
  Session s;
  s.authenticate();
  s.startSync();
  s.hello();
  EXPECT_EQ(s.ports.lastOpcode(), static_cast<Opcode>(0x81));
  EXPECT_EQ(s.ports.lastResult(), Result::Busy);
  EXPECT_EQ(s.ports.lastPayload()[7 + kNonceBytes], static_cast<uint8_t>(DeviceState::WifiConnecting));
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
  EXPECT_TRUE(s.sut.authenticated());
  s.wifiUp();
  s.hello();
  EXPECT_EQ(s.ports.lastResult(), Result::Busy);
  EXPECT_EQ(s.sut.state(), State::TransferReady);
  EXPECT_TRUE(s.sut.authenticated());
}

TEST(SyncCoordinator, ReHelloDropsTrustAndRestartsTheUnauthenticatedClock) {
  Session s;
  s.authenticate();
  s.advance(kConnectedUnauthMs + 5000);  // Longer than the unauthenticated limit, shorter than the idle one.
  ASSERT_EQ(s.sut.state(), State::Authenticated);
  s.hello();
  const uint32_t helloAt = s.now;
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(s.sut.state(), State::Connected);
  EXPECT_EQ(s.sut.lastReason(), Reason::HelloOk);
  EXPECT_FALSE(s.sut.authenticated());
  s.write(Opcode::GetStatus, {});
  EXPECT_EQ(s.ports.lastResult(), Result::NotAuthenticated);
  s.advance(helloAt + kConnectedUnauthMs - 1 - s.now);
  EXPECT_EQ(s.sut.state(), State::Connected);
  s.advance(1);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_TRUE(s.ports.logged("ble:drop:1"));
}

TEST(SyncCoordinator, MalformedFrameGetsBadFrame) {
  Session s;
  s.connect();
  uint8_t junk[kHeaderBytes] = {0x01, 0x00, 0x2A, 0x00, 0x09, 0x00};  // claims 9 bytes
  s.sut.onControlWrite(kApp, junk, sizeof(junk), s.now);
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

TEST(SyncCoordinator, AuthOkCarriesTheDeviceProof) {
  Session s;
  s.connect();
  s.hello();
  s.auth(7);
  ASSERT_EQ(s.ports.lastResult(), Result::Ok);
  ASSERT_EQ(s.ports.lastPayloadLength(), kAuthResultBytes);
  uint8_t message[kDeviceProofMessageBytes];
  buildDeviceProofMessage(s.appNonce, s.deviceNonce, 7, message);
  uint8_t proof[kMacBytes];
  s.ports.computeAuthMac(message, sizeof(message), proof);
  EXPECT_EQ(0, memcmp(s.ports.lastPayload() + 1, proof, kMacBytes));
  // Not the app's own MAC sent back.
  uint8_t appMessage[kAuthMessageBytes];
  buildAuthMessage(s.deviceNonce, s.appNonce, 7, appMessage);
  uint8_t appMac[kMacBytes];
  s.ports.computeAuthMac(appMessage, sizeof(appMessage), appMac);
  EXPECT_NE(0, memcmp(s.ports.lastPayload() + 1, appMac, kMacBytes));
}

TEST(SyncCoordinator, WrongMacFailsAndBurnsTheNonce) {
  Session s;
  s.connect();
  s.hello();
  s.auth(1, false);
  EXPECT_EQ(s.ports.lastResult(), Result::AuthFailed);
  EXPECT_EQ(s.ports.lastPayloadLength(), 1u);
  EXPECT_EQ(s.sut.state(), State::Connected);
  s.auth(1, true);  // right MAC, but HELLO must be repeated first
  EXPECT_EQ(s.ports.lastResult(), Result::OutOfOrder);
  s.hello();
  s.auth(1, true);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
}

TEST(SyncCoordinator, CounterThatCannotBePersistedFailsAuth) {
  Session s;
  s.ports.counterStoreWorks = false;
  s.connect();
  s.hello();
  s.auth(1);
  EXPECT_EQ(s.ports.lastResult(), Result::Internal);
  EXPECT_EQ(s.ports.lastPayloadLength(), 1u);
  EXPECT_EQ(s.sut.state(), State::Connected);
  EXPECT_FALSE(s.sut.authenticated());
  s.write(Opcode::GetStatus, {});
  EXPECT_EQ(s.ports.lastResult(), Result::NotAuthenticated);

  s.ports.counterStoreWorks = true;
  s.auth(1);
  EXPECT_EQ(s.ports.lastResult(), Result::OutOfOrder);  // Start over from HELLO.
  s.hello();
  s.auth(1);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(s.ports.counter, 1u);
}

TEST(SyncCoordinator, ReplayedCounterIsRefused) {
  Session s;
  s.authenticate();
  s.sut.onCentralDisconnected(kApp, s.now);
  s.sut.onCentralConnected(kApp, s.now);
  s.hello();
  s.auth(1);
  EXPECT_EQ(s.ports.lastResult(), Result::Replayed);
  s.auth(2);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
}

TEST(SyncCoordinator, ReplayedCounterIsRefusedAfterAReboot) {
  Session before;
  before.authenticate();
  ASSERT_EQ(before.ports.counter, 1u);

  Session after;  // A new coordinator; only what NVS held survives.
  after.ports.counter = before.ports.counter;
  after.connect();
  after.hello();
  after.auth(1);
  EXPECT_EQ(after.ports.lastResult(), Result::Replayed);
  after.auth(2);
  EXPECT_EQ(after.ports.lastResult(), Result::Ok);
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
  EXPECT_TRUE(s.ports.logged("ble:drop:1"));
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_EQ(s.sut.lastReason(), Reason::LockedOut);
  s.sut.onCentralDisconnected(kApp, s.now);  // The dropped link's event, late.
  s.sut.onCentralConnected(kApp, s.now);
  s.hello();
  s.auth(1, true);
  EXPECT_EQ(s.ports.lastResult(), Result::LockedOut);
  s.advance(kLockoutMs);  // The unauthenticated link timed out meanwhile; the app reconnects.
  EXPECT_EQ(s.sut.state(), State::Advertising);
  s.sut.onCentralConnected(kApp, s.now);
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
  s.sut.onControlWrite(kApp, frame, n, s.now + 1);
  EXPECT_EQ(s.ports.lastResult(), Result::RateLimited);
  EXPECT_EQ(s.ports.lastSequence(), 77);
}

TEST(SyncCoordinator, MalformedBurstGetsAtMostOneAnswerPerInterval) {
  Session s;
  s.connect();
  const uint8_t junk[3] = {0x01, 0x00, 0x00};
  const uint32_t t0 = s.now + 1000;
  for (uint32_t t = 0; t < kMinCommandIntervalMs; t += 10) s.sut.onControlWrite(kApp, junk, sizeof(junk), t0 + t);
  ASSERT_EQ(s.ports.results.size(), 1u);
  EXPECT_EQ(s.ports.lastResult(), Result::BadFrame);

  // Junk spends the interval like any request: a well-formed frame inside it is RateLimited.
  uint8_t frame[kMaxFrameBytes];
  const size_t n = encodeFrame(Opcode::GetStatus, 5, nullptr, 0, frame, sizeof(frame));
  s.sut.onControlWrite(kApp, frame, n, t0 + kMinCommandIntervalMs - 1);
  EXPECT_EQ(s.ports.results.size(), 2u);
  EXPECT_EQ(s.ports.lastResult(), Result::RateLimited);

  s.sut.onControlWrite(kApp, junk, sizeof(junk), t0 + kMinCommandIntervalMs);
  EXPECT_EQ(s.ports.results.size(), 3u);
  EXPECT_EQ(s.ports.lastResult(), Result::BadFrame);
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

TEST(SyncCoordinator, UnauthenticatedCentralIsDroppedAfterTimeout) {
  Session s;
  s.connect();
  s.advance(kConnectedUnauthMs - 1);
  EXPECT_EQ(s.sut.state(), State::Connected);
  s.advance(1);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_TRUE(s.ports.logged("ble:drop:1"));
  EXPECT_TRUE(s.advertisingMatchesState());
  EXPECT_EQ(s.ports.count("adv:on"), 2u);
}

TEST(SyncCoordinator, AuthenticatedIdleIsDroppedAfterTimeout) {
  Session s;
  s.authenticate();
  s.advance(kAuthIdleMs);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_FALSE(s.sut.authenticated());
  EXPECT_TRUE(s.advertisingMatchesState());
}

// --- START_SYNC --------------------------------------------------------------------------------

TEST(SyncCoordinator, HappyPathSession) {
  Session s;
  s.authenticate();
  const size_t before = s.ports.results.size();
  s.startSync(2, 200000);
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
  EXPECT_TRUE(s.ports.logged("wifi:start"));
  EXPECT_EQ(s.ports.results.size(), before);  // The result waits for the address.

  s.wifiUp();
  EXPECT_EQ(s.sut.state(), State::TransferReady);
  EXPECT_TRUE(s.ports.logged("http:start"));
  ASSERT_EQ(s.ports.results.size(), before + 1);
  EXPECT_EQ(s.ports.lastOpcode(), static_cast<Opcode>(0x84));
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  const uint8_t* p = s.ports.lastPayload();
  EXPECT_EQ(0, memcmp(p + 1, s.sessionId, kSessionIdBytes));
  const uint8_t dotted[kIpv4Bytes] = {192, 168, 1, 5};  // 192.168.1.5, in reading order
  EXPECT_EQ(0, memcmp(p + 1 + kSessionIdBytes, dotted, kIpv4Bytes));
  EXPECT_EQ(p[1 + kSessionIdBytes + 4], 0x90);  // 8080 LE
  EXPECT_EQ(p[1 + kSessionIdBytes + 5], 0x1F);
  EXPECT_EQ(p[1 + kSessionIdBytes + 6], 90);  // ttlSeconds LE
  EXPECT_EQ(p[1 + kSessionIdBytes + 7], 0);

  EXPECT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  EXPECT_EQ(s.sut.state(), State::Transferring);
  s.sut.onUploadProgress(s.now);
  s.sut.onUploadCommitted(s.now);
  EXPECT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  s.sut.onUploadCommitted(s.now);
  s.sut.onSessionFinishRequested(s.now);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_TRUE(s.ports.logged("Transferring>Finishing:SessionFinished"));
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
  EXPECT_FALSE(s.sut.authenticated());
}

TEST(SyncCoordinator, StartSyncRetryWhileJoiningWifiIsAnsweredOnceUnderTheNewestSequence) {
  Session s;
  s.authenticate();
  const size_t before = s.ports.results.size();
  s.startSync();
  s.startSync();
  const uint16_t newest = s.seq;
  s.startSync();
  EXPECT_EQ(s.ports.results.size(), before);
  EXPECT_EQ(s.ports.count("wifi:start"), 1u);
  s.wifiUp();
  ASSERT_EQ(s.ports.results.size(), before + 1);
  EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  EXPECT_EQ(s.ports.lastSequence(), newest);
}

TEST(SyncCoordinator, WifiFailureAfterARetryIsAnsweredOnceUnderTheNewestSequence) {
  Session s;
  s.authenticate();
  const size_t before = s.ports.results.size();
  s.startSync();
  const uint16_t newest = s.seq;
  s.startSync();
  s.sut.onWifiFailed(s.now);
  ASSERT_EQ(s.ports.results.size(), before + 1);
  EXPECT_EQ(s.ports.lastResult(), Result::WifiUnavailable);
  EXPECT_EQ(s.ports.lastSequence(), newest);
}

TEST(SyncCoordinator, StartSyncRetryAfterWifiUpReplaysTheResult) {
  Session s;
  s.transferReady();
  const std::vector<uint8_t> first(s.ports.lastPayload(), s.ports.lastPayload() + s.ports.lastPayloadLength());

  uint16_t retry = s.seq;
  s.startSync();
  EXPECT_EQ(s.ports.lastSequence(), retry);
  EXPECT_EQ(std::vector<uint8_t>(s.ports.lastPayload(), s.ports.lastPayload() + s.ports.lastPayloadLength()), first);

  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  retry = s.seq;
  s.startSync();
  EXPECT_EQ(s.ports.lastSequence(), retry);
  EXPECT_EQ(std::vector<uint8_t>(s.ports.lastPayload(), s.ports.lastPayload() + s.ports.lastPayloadLength()), first);
  EXPECT_EQ(s.ports.count("wifi:start"), 1u);
}

TEST(SyncCoordinator, AnotherSessionWhileOneRunsIsBusy) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sessionId[0] ^= 1;
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::Busy);
  EXPECT_EQ(s.sut.state(), State::WifiConnecting);
  s.wifiUp();
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::Busy);
  EXPECT_EQ(s.sut.state(), State::TransferReady);
  EXPECT_EQ(s.ports.count("wifi:start"), 1u);
}

TEST(SyncCoordinator, StartSyncRefusalsDoNotTouchWifi) {
  Session s;
  s.authenticate();

  s.env.readerOpen = true;
  s.apply();
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::ReaderActive);
  s.env.readerOpen = false;

  s.env.sdMounted = false;
  s.apply();
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::StorageUnavailable);
  s.env.sdMounted = true;

  s.env.wifiCredentialsSaved = false;
  s.apply();
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::WifiUnavailable);
  s.env.wifiCredentialsSaved = true;

  s.env.batteryPercent = kMinBatteryPercentForSync - 1;
  s.apply();
  s.startSync();
  EXPECT_EQ(s.ports.lastResult(), Result::Busy);
  s.env.batteryPercent = 80;

  s.apply();
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

// --- Uploads -----------------------------------------------------------------------------------

TEST(SyncCoordinator, UploadsNeedATransferSession) {
  Session s;
  s.authenticate();
  EXPECT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::NoSession);
  s.startSync();
  EXPECT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::NoSession);
  s.wifiUp();
  s.sut.onSessionFinishRequested(s.now);
  EXPECT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::NoSession);
}

TEST(SyncCoordinator, OneUploadAtATime) {
  Session s;
  s.transferReady(3, 300000);
  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  EXPECT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::UploadInFlight);
  s.sut.onUploadCommitted(s.now);
  EXPECT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
}

TEST(SyncCoordinator, UploadBudgetIsWhatStartSyncDeclared) {
  Session s;
  s.transferReady(2, 300000);
  ASSERT_EQ(s.sut.onUploadStarted(200000, s.now), UploadAdmission::Accept);
  s.sut.onUploadCommitted(s.now);
  EXPECT_EQ(s.sut.onUploadStarted(100001, s.now), UploadAdmission::ByteLimit);
  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  s.sut.onUploadCommitted(s.now);
  EXPECT_EQ(s.sut.onUploadStarted(1, s.now), UploadAdmission::FileLimit);
}

TEST(SyncCoordinator, FailedUploadReleasesItsReservation) {
  Session s;
  s.transferReady(1, 100000);
  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  s.sut.onUploadFailed(s.now);
  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  s.sut.onUploadCommitted(s.now);
  EXPECT_EQ(s.sut.onUploadStarted(1, s.now), UploadAdmission::FileLimit);
}

TEST(SyncCoordinator, TransferIdleTimeoutCleansUp) {
  Session s;
  s.transferReady();
  s.advance(kTransferIdleMs - 1);
  EXPECT_EQ(s.sut.state(), State::TransferReady);
  s.advance(1);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
}

TEST(SyncCoordinator, ProgressKeepsALongUploadAlive) {
  Session s;
  s.transferReady();
  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  for (int i = 0; i < 20; ++i) {  // 200 s of upload, a chunk every 10 s.
    s.advance(10000);
    s.sut.onUploadProgress(s.now);
  }
  EXPECT_EQ(s.sut.state(), State::Transferring);
  s.sut.onUploadCommitted(s.now);
  EXPECT_EQ(s.sut.state(), State::Transferring);
}

TEST(SyncCoordinator, StalledUploadTimesOutEvenWhileTheAppPolls) {
  Session s;
  s.transferReady();
  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  const uint32_t lastByteAt = s.now;
  for (int i = 0; i < 4; ++i) {  // BLE commands are not upload traffic.
    s.advance(20000);
    s.write(Opcode::GetStatus, {});
    EXPECT_EQ(s.ports.lastResult(), Result::Ok);
  }
  s.advance(lastByteAt + kTransferIdleMs - 1 - s.now);
  EXPECT_EQ(s.sut.state(), State::Transferring);
  s.advance(1);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_EQ(s.sut.lastReason(), Reason::Timeout);
}

// --- Leaving a session -------------------------------------------------------------------------

TEST(SyncCoordinator, EveryExitStopsHttpThenWifiThenDropsTheLink) {
  {
    Session s;
    s.transferReady();
    const size_t mark = s.ports.log.size();
    s.write(Opcode::CancelSync, {});
    EXPECT_EQ(s.ports.lastResult(), Result::Ok);
    EXPECT_EQ(s.ports.logSince(mark),
              (std::vector<std::string>{"http:stop", "wifi:stop", "ble:drop:1", "TransferReady>Cleanup:CancelSync"}));
  }
  {
    Session s;
    s.transferReady();
    ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
    s.sut.onUploadCommitted(s.now);
    const size_t mark = s.ports.log.size();
    s.sut.onSessionFinishRequested(s.now);
    EXPECT_EQ(s.ports.logSince(mark),
              (std::vector<std::string>{"Transferring>Finishing:SessionFinished", "http:stop", "wifi:stop",
                                        "ble:drop:1", "Finishing>Cleanup:SessionFinished"}));
  }
  {
    Session s;
    s.authenticate();
    s.startSync();
    const size_t mark = s.ports.log.size();
    s.sut.onWifiFailed(s.now);
    s.advance(kErrorHoldMs);
    EXPECT_EQ(s.ports.logSince(mark),
              (std::vector<std::string>{"http:stop", "wifi:stop", "ble:drop:1", "WifiConnecting>Error:WifiFailed",
                                        "Error>Cleanup:WifiFailed"}));
  }
}

TEST(SyncCoordinator, BleDropDuringTransferThenIdleClosesTheSession) {
  Session s;
  s.transferReady();
  ASSERT_EQ(s.sut.onUploadStarted(100000, s.now), UploadAdmission::Accept);
  s.sut.onUploadCommitted(s.now);
  s.sut.onCentralDisconnected(kApp, s.now);
  EXPECT_EQ(s.sut.state(), State::Transferring);
  EXPECT_FALSE(s.ports.logged("http:stop"));
  s.advance(kTransferIdleMs - 1);
  EXPECT_EQ(s.sut.state(), State::Transferring);
  s.advance(1);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_EQ(s.sut.lastReason(), Reason::Timeout);
  EXPECT_TRUE(s.ports.logged("http:stop"));
  EXPECT_TRUE(s.ports.logged("wifi:stop"));
  EXPECT_FALSE(s.ports.logged("ble:drop:1"));  // Already gone; nothing to drop.
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Advertising);
}

TEST(SyncCoordinator, BleDropWhileJoiningWifiAbortsTheSession) {
  Session s;
  s.authenticate();
  s.startSync();
  s.sut.onCentralDisconnected(kApp, s.now);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  EXPECT_TRUE(s.ports.logged("wifi:stop"));
}

TEST(SyncCoordinator, IdleRequestDuringSessionCleansUpThenGoesIdle) {
  Session s;
  s.transferReady();
  s.sut.requestIdle(s.now);
  EXPECT_EQ(s.sut.state(), State::Cleanup);
  s.sut.onCleanupDone(s.now);
  EXPECT_EQ(s.sut.state(), State::Idle);
  EXPECT_FALSE(s.ports.advertising);
}

TEST(SyncCoordinator, StatusShowsWifiOnlyWhileUp) {
  Session s;
  s.authenticate();
  EXPECT_EQ(s.sut.status().flags & kStatusWifiUp, 0);
  s.startSync();
  s.wifiUp();
  EXPECT_EQ(s.sut.status().flags & kStatusWifiUp, kStatusWifiUp);
  s.sut.onSessionFinishRequested(s.now);
  EXPECT_EQ(s.sut.status().flags & kStatusWifiUp, 0);
}

}  // namespace
