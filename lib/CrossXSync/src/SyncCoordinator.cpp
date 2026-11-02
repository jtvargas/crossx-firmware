#include "SyncCoordinator.h"

#include <cstring>

namespace crossx::sync {

using namespace protocol;

namespace {

constexpr bool isSessionState(State s) {
  return s == State::WifiConnecting || s == State::TransferReady || s == State::Transferring ||
         s == State::Finishing;
}

constexpr bool radioOn(State s) { return s != State::Idle; }

constexpr DeviceState toDeviceState(State s) { return static_cast<DeviceState>(static_cast<uint8_t>(s)); }

}  // namespace

const char* stateName(State state) {
  switch (state) {
    case State::Idle:
      return "Idle";
    case State::Advertising:
      return "Advertising";
    case State::Connected:
      return "Connected";
    case State::Authenticated:
      return "Authenticated";
    case State::WifiConnecting:
      return "WifiConnecting";
    case State::TransferReady:
      return "TransferReady";
    case State::Transferring:
      return "Transferring";
    case State::Finishing:
      return "Finishing";
    case State::Cleanup:
      return "Cleanup";
    case State::Error:
      return "Error";
  }
  return "?";
}

const char* reasonName(Reason reason) {
  switch (reason) {
    case Reason::None:
      return "None";
    case Reason::UserRequest:
      return "UserRequest";
    case Reason::BleConnected:
      return "BleConnected";
    case Reason::BleDisconnected:
      return "BleDisconnected";
    case Reason::HelloOk:
      return "HelloOk";
    case Reason::AuthOk:
      return "AuthOk";
    case Reason::AuthFailed:
      return "AuthFailed";
    case Reason::LockedOut:
      return "LockedOut";
    case Reason::StartSync:
      return "StartSync";
    case Reason::CancelSync:
      return "CancelSync";
    case Reason::WifiUp:
      return "WifiUp";
    case Reason::WifiFailed:
      return "WifiFailed";
    case Reason::FirstUpload:
      return "FirstUpload";
    case Reason::SessionFinished:
      return "SessionFinished";
    case Reason::Timeout:
      return "Timeout";
    case Reason::ReaderBusy:
      return "ReaderBusy";
    case Reason::StorageFailed:
      return "StorageFailed";
    case Reason::LowBattery:
      return "LowBattery";
    case Reason::PowerOff:
      return "PowerOff";
    case Reason::CleanupDone:
      return "CleanupDone";
  }
  return "?";
}

SyncCoordinator::SyncCoordinator(Ports& ports) : ports_(ports) {}

bool SyncCoordinator::authenticated() const { return authDone_ && state_ != State::Idle && state_ != State::Cleanup; }

// --- Transitions -------------------------------------------------------------------------------

void SyncCoordinator::transition(State to, Reason reason, uint32_t nowMs) {
  if (to == state_) return;
  const State from = state_;
  state_ = to;
  lastReason_ = reason;
  stateSinceMs_ = nowMs;
  lastActivityMs_ = nowMs;
  if (radioOn(to) && !radioOn(from)) ports_.startAdvertising();
  if (to == State::Idle && radioOn(from)) ports_.stopAdvertising();
  ports_.onStateChanged(from, to, reason);
}

void SyncCoordinator::enterCleanup(Reason reason, uint32_t nowMs) {
  // Every path out of a session goes through here, so the radio, Wi-Fi, and receiver are torn
  // down in one order regardless of what failed.
  if (state_ == State::Cleanup) return;
  ports_.httpStop();
  ports_.wifiStop();
  ports_.disconnectCentral();
  sessionActive_ = false;
  helloDone_ = false;
  authDone_ = false;
  ipv4_ = 0;
  httpPort_ = 0;
  transition(State::Cleanup, reason, nowMs);
}

void SyncCoordinator::fail(Reason reason, uint32_t nowMs) {
  if (state_ == State::Error || state_ == State::Cleanup || state_ == State::Idle) return;
  ports_.httpStop();
  ports_.wifiStop();
  ports_.disconnectCentral();
  sessionActive_ = false;
  helloDone_ = false;
  authDone_ = false;
  transition(State::Error, reason, nowMs);
}

// --- User and power inputs ---------------------------------------------------------------------

void SyncCoordinator::requestAdvertising(uint32_t nowMs) {
  if (state_ != State::Idle) return;
  if (!env_.paired && !env_.sdMounted) {
    // Nothing useful can happen; stay dark rather than advertise a device that cannot store.
    return;
  }
  transition(State::Advertising, Reason::UserRequest, nowMs);
}

void SyncCoordinator::requestIdle(uint32_t nowMs) {
  switch (state_) {
    case State::Idle:
      return;
    case State::Advertising:
      transition(State::Idle, Reason::UserRequest, nowMs);
      return;
    case State::Connected:
    case State::Authenticated:
      ports_.disconnectCentral();
      helloDone_ = false;
      authDone_ = false;
      transition(State::Idle, Reason::UserRequest, nowMs);
      return;
    case State::Cleanup:
    case State::Error:
      // Cleanup finishes on its own; onCleanupDone() then honours the pending idle request.
      idleRequested_ = true;
      return;
    default:
      idleRequested_ = true;
      enterCleanup(Reason::UserRequest, nowMs);
      return;
  }
}

// --- Radio inputs ------------------------------------------------------------------------------

void SyncCoordinator::onCentralConnected(uint32_t nowMs) {
  if (state_ != State::Advertising) {
    // A second central during a session, or a connection while idle: not ours to serve.
    ports_.disconnectCentral();
    return;
  }
  helloDone_ = false;
  authDone_ = false;
  transition(State::Connected, Reason::BleConnected, nowMs);
}

void SyncCoordinator::onCentralDisconnected(uint32_t nowMs) {
  helloDone_ = false;
  authDone_ = false;
  switch (state_) {
    case State::Connected:
    case State::Authenticated:
      transition(State::Advertising, Reason::BleDisconnected, nowMs);
      return;
    case State::WifiConnecting:
      // The app only learns the address from START_SYNC's result; without the link it cannot upload.
      enterCleanup(Reason::BleDisconnected, nowMs);
      return;
    default:
      // During a transfer the BLE link is optional: the upload runs over Wi-Fi and the receipt is
      // durable on SD. Keep going; the idle timer closes the session if the app never finishes.
      return;
  }
}

bool SyncCoordinator::rateLimited(uint32_t nowMs) {
  if (lastCommandMs_ != 0 && static_cast<uint32_t>(nowMs - lastCommandMs_) < kMinCommandIntervalMs) return true;
  lastCommandMs_ = nowMs;
  return false;
}

void SyncCoordinator::onControlWrite(const uint8_t* bytes, size_t length, uint32_t nowMs) {
  if (state_ == State::Idle || state_ == State::Advertising || state_ == State::Cleanup || state_ == State::Error) {
    return;  // No central we trust enough to answer.
  }
  Frame frame{};
  const ParseStatus parsed = parseFrame(bytes, length, frame);
  if (parsed != ParseStatus::Ok) {
    // Malformed input is answered once with the sequence it carried, if there was one.
    const uint16_t seq = length >= 4 ? static_cast<uint16_t>(bytes[2] | (bytes[3] << 8)) : 0;
    const Opcode op = length >= 1 ? static_cast<Opcode>(bytes[0] & ~static_cast<uint8_t>(Opcode::ResultBit)) : Opcode::Hello;
    replyCode(op, seq, Result::BadFrame);
    return;
  }
  if (rateLimited(nowMs)) {
    replyCode(frame.header.opcode, frame.header.sequence, Result::RateLimited);
    return;
  }
  lastActivityMs_ = nowMs;
  switch (frame.header.opcode) {
    case Opcode::Hello:
      handleHello(frame);
      return;
    case Opcode::Auth:
      handleAuth(frame, nowMs);
      return;
    case Opcode::GetStatus:
      handleGetStatus(frame);
      return;
    case Opcode::StartSync:
      handleStartSync(frame, nowMs);
      return;
    case Opcode::CancelSync:
      handleCancelSync(frame, nowMs);
      return;
    default:
      replyCode(frame.header.opcode, frame.header.sequence, Result::BadFrame);
      return;
  }
}

void SyncCoordinator::reply(Opcode request, uint16_t sequence, const uint8_t* payload, size_t length) {
  uint8_t out[kMaxFrameBytes];
  const Opcode op = static_cast<Opcode>(static_cast<uint8_t>(request) | static_cast<uint8_t>(Opcode::ResultBit));
  const size_t n = encodeFrame(op, sequence, payload, length, out, sizeof(out));
  if (n) ports_.notifyResult(out, n);
}

void SyncCoordinator::replyCode(Opcode request, uint16_t sequence, Result result) {
  uint8_t code[1];
  encodeResult(result, code, sizeof(code));
  reply(request, sequence, code, sizeof(code));
}

void SyncCoordinator::handleHello(const Frame& frame) {
  HelloRequest req{};
  if (!decodeHelloRequest(frame.payload, frame.header.payloadLength, req)) {
    replyCode(Opcode::Hello, frame.header.sequence, Result::BadFrame);
    return;
  }
  HelloResult res{};
  res.major = kProtocolMajor;
  res.minor = kProtocolMinor;
  res.features = kFeatureWifiStation | kFeatureReceipts | kFeatureCancel;
  res.state = toDeviceState(state_);
  if (!versionCompatible(req.major)) {
    res.result = Result::UnsupportedVersion;
  } else if (!env_.paired) {
    res.result = Result::NotPaired;
  } else if (!ports_.randomNonce(deviceNonce_)) {
    res.result = Result::Internal;
  } else {
    memcpy(appNonce_, req.appNonce, kNonceBytes);
    memcpy(res.deviceNonce, deviceNonce_, kNonceBytes);
    helloDone_ = true;
    authDone_ = false;  // A fresh HELLO always needs a fresh AUTH.
    lastReason_ = Reason::HelloOk;
    res.result = Result::Ok;
  }
  uint8_t payload[kHelloResultBytes];
  reply(Opcode::Hello, frame.header.sequence, payload, encodeHelloResult(res, payload, sizeof(payload)));
  if (res.result == Result::UnsupportedVersion) ports_.disconnectCentral();
}

void SyncCoordinator::handleAuth(const Frame& frame, uint32_t nowMs) {
  if (lockedUntilMs_ != 0 && static_cast<int32_t>(lockedUntilMs_ - nowMs) > 0) {
    replyCode(Opcode::Auth, frame.header.sequence, Result::LockedOut);
    return;
  }
  lockedUntilMs_ = 0;
  if (!helloDone_) {
    replyCode(Opcode::Auth, frame.header.sequence, Result::OutOfOrder);
    return;
  }
  AuthRequest req{};
  if (!decodeAuthRequest(frame.payload, frame.header.payloadLength, req)) {
    replyCode(Opcode::Auth, frame.header.sequence, Result::BadFrame);
    return;
  }
  const uint32_t lastCounter = ports_.loadAuthCounter();
  if (req.counter <= lastCounter) {
    replyCode(Opcode::Auth, frame.header.sequence, Result::Replayed);
    return;
  }
  uint8_t message[kAuthMessageBytes];
  buildAuthMessage(deviceNonce_, appNonce_, req.counter, message);
  uint8_t expected[kMacBytes];
  if (!ports_.computeAuthMac(message, sizeof(message), expected)) {
    replyCode(Opcode::Auth, frame.header.sequence, Result::NotPaired);
    return;
  }
  if (!constantTimeEqual(expected, req.mac, kMacBytes)) {
    helloDone_ = false;  // The nonce is burnt either way; the app must HELLO again.
    if (++authFailures_ >= kMaxAuthFailures) {
      authFailures_ = 0;
      lockedUntilMs_ = nowMs + kLockoutMs;
      if (lockedUntilMs_ == 0) lockedUntilMs_ = 1;
      replyCode(Opcode::Auth, frame.header.sequence, Result::LockedOut);
      lastReason_ = Reason::LockedOut;
      ports_.disconnectCentral();
      return;
    }
    replyCode(Opcode::Auth, frame.header.sequence, Result::AuthFailed);
    lastReason_ = Reason::AuthFailed;
    return;
  }
  authFailures_ = 0;
  ports_.storeAuthCounter(req.counter);
  authDone_ = true;
  replyCode(Opcode::Auth, frame.header.sequence, Result::Ok);
  if (state_ == State::Connected) transition(State::Authenticated, Reason::AuthOk, nowMs);
}

void SyncCoordinator::handleGetStatus(const Frame& frame) {
  if (!authDone_) {
    replyCode(Opcode::GetStatus, frame.header.sequence, Result::NotAuthenticated);
    return;
  }
  uint8_t payload[1 + kStatusPayloadBytes];
  payload[0] = static_cast<uint8_t>(Result::Ok);
  const size_t n = encodeStatus(status(), payload + 1, kStatusPayloadBytes);
  reply(Opcode::GetStatus, frame.header.sequence, payload, 1 + n);
}

void SyncCoordinator::sendStartSyncResult(Result result, uint16_t sequence) {
  StartSyncResult res{};
  res.result = result;
  memcpy(res.sessionId, sessionId_, kSessionIdBytes);
  res.ipv4 = ipv4_;
  res.httpPort = httpPort_;
  res.ttlSeconds = static_cast<uint16_t>(kTransferIdleMs / 1000u);
  uint8_t payload[kStartSyncResultBytes];
  reply(Opcode::StartSync, sequence, payload, encodeStartSyncResult(res, payload, sizeof(payload)));
}

void SyncCoordinator::handleStartSync(const Frame& frame, uint32_t nowMs) {
  if (!authDone_) {
    replyCode(Opcode::StartSync, frame.header.sequence, Result::NotAuthenticated);
    return;
  }
  StartSyncRequest req{};
  if (!decodeStartSyncRequest(frame.payload, frame.header.payloadLength, req)) {
    replyCode(Opcode::StartSync, frame.header.sequence, Result::BadFrame);
    return;
  }
  if (sessionActive_) {
    // Idempotent for the same session: the app lost the first result and asks again.
    if (memcmp(req.sessionId, sessionId_, kSessionIdBytes) == 0) {
      sendStartSyncResult(Result::Ok, frame.header.sequence);
    } else {
      replyCode(Opcode::StartSync, frame.header.sequence, Result::Busy);
    }
    return;
  }
  if (req.fileCount == 0 || req.fileCount > kMaxFilesPerSession || req.totalBytes > kMaxSessionBytes) {
    replyCode(Opcode::StartSync, frame.header.sequence, Result::BadFrame);
    return;
  }
  if (env_.readerOpen) {
    replyCode(Opcode::StartSync, frame.header.sequence, Result::ReaderActive);
    return;
  }
  if (!env_.sdMounted) {
    replyCode(Opcode::StartSync, frame.header.sequence, Result::StorageUnavailable);
    return;
  }
  if (env_.batteryPercent < kMinBatteryPercentForSync) {
    replyCode(Opcode::StartSync, frame.header.sequence, Result::Busy);
    return;
  }
  if (!env_.wifiCredentialsSaved) {
    replyCode(Opcode::StartSync, frame.header.sequence, Result::WifiUnavailable);
    return;
  }
  memcpy(sessionId_, req.sessionId, kSessionIdBytes);
  sessionActive_ = true;
  startSyncSequence_ = frame.header.sequence;
  committedFiles_ = 0;
  failedFiles_ = 0;
  ipv4_ = 0;
  httpPort_ = 0;
  ports_.wifiStart();
  transition(State::WifiConnecting, Reason::StartSync, nowMs);
  // The result with the address follows from onWifiUp(); the app waits for that notification.
}

void SyncCoordinator::handleCancelSync(const Frame& frame, uint32_t nowMs) {
  if (!authDone_) {
    replyCode(Opcode::CancelSync, frame.header.sequence, Result::NotAuthenticated);
    return;
  }
  replyCode(Opcode::CancelSync, frame.header.sequence, Result::Ok);
  if (isSessionState(state_)) enterCleanup(Reason::CancelSync, nowMs);
}

// --- Wi-Fi and receiver inputs -----------------------------------------------------------------

void SyncCoordinator::onWifiUp(uint32_t ipv4, uint16_t httpPort, uint32_t nowMs) {
  if (state_ != State::WifiConnecting) return;
  ipv4_ = ipv4;
  httpPort_ = httpPort;
  ports_.httpStart(sessionId_);
  transition(State::TransferReady, Reason::WifiUp, nowMs);
  sendStartSyncResult(Result::Ok, startSyncSequence_);
}

void SyncCoordinator::onWifiFailed(uint32_t nowMs) {
  if (state_ != State::WifiConnecting) return;
  sendStartSyncResult(Result::WifiUnavailable, startSyncSequence_);
  fail(Reason::WifiFailed, nowMs);
}

void SyncCoordinator::onUploadStarted(uint32_t nowMs) {
  if (state_ == State::TransferReady) transition(State::Transferring, Reason::FirstUpload, nowMs);
  if (state_ == State::Transferring) lastActivityMs_ = nowMs;
}

void SyncCoordinator::onUploadCommitted(uint32_t nowMs) {
  if (state_ != State::Transferring && state_ != State::TransferReady) return;
  if (state_ == State::TransferReady) transition(State::Transferring, Reason::FirstUpload, nowMs);
  ++committedFiles_;
  lastActivityMs_ = nowMs;
}

void SyncCoordinator::onUploadFailed(uint32_t nowMs) {
  if (state_ != State::Transferring && state_ != State::TransferReady) return;
  ++failedFiles_;
  lastActivityMs_ = nowMs;
}

void SyncCoordinator::onSessionFinishRequested(uint32_t nowMs) {
  if (state_ != State::Transferring && state_ != State::TransferReady) return;
  transition(State::Finishing, Reason::SessionFinished, nowMs);
  enterCleanup(Reason::SessionFinished, nowMs);
}

void SyncCoordinator::onCleanupDone(uint32_t nowMs) {
  if (state_ != State::Cleanup) return;
  const bool toIdle = idleRequested_;
  idleRequested_ = false;
  if (toIdle) {
    transition(State::Idle, Reason::CleanupDone, nowMs);
  } else {
    transition(State::Advertising, Reason::CleanupDone, nowMs);
  }
}

// --- Timers ------------------------------------------------------------------------------------

void SyncCoordinator::tick(uint32_t nowMs) {
  const uint32_t inState = nowMs - stateSinceMs_;
  const uint32_t sinceActivity = nowMs - lastActivityMs_;
  switch (state_) {
    case State::Connected:
      if (inState >= kConnectedUnauthMs) {
        ports_.disconnectCentral();
        helloDone_ = false;
        transition(State::Advertising, Reason::Timeout, nowMs);
      }
      return;
    case State::Authenticated:
      if (sinceActivity >= kAuthIdleMs) {
        ports_.disconnectCentral();
        helloDone_ = false;
        authDone_ = false;
        transition(State::Advertising, Reason::Timeout, nowMs);
      }
      return;
    case State::WifiConnecting:
      if (inState >= kWifiConnectMs) {
        sendStartSyncResult(Result::WifiUnavailable, startSyncSequence_);
        fail(Reason::Timeout, nowMs);
      }
      return;
    case State::TransferReady:
    case State::Transferring:
      if (sinceActivity >= kTransferIdleMs) enterCleanup(Reason::Timeout, nowMs);
      return;
    case State::Error:
      if (inState >= kErrorHoldMs) enterCleanup(lastReason_, nowMs);
      return;
    default:
      return;
  }
}

StatusPayload SyncCoordinator::status() const {
  StatusPayload s{};
  s.state = toDeviceState(state_);
  s.batteryPercent = env_.batteryPercent;
  s.flags = static_cast<uint8_t>((env_.sdMounted ? kStatusSdMounted : 0) | (ipv4_ ? kStatusWifiUp : 0) |
                                 (env_.readerOpen ? kStatusReaderOpen : 0) | (env_.paired ? kStatusPaired : 0));
  s.freeHeap = env_.freeHeap;
  s.largestFreeBlock = env_.largestFreeBlock;
  s.pendingReceipts = env_.pendingReceipts;
  return s;
}

}  // namespace crossx::sync
