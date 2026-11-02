#pragma once

#include <cstddef>
#include <cstdint>

#include "ProtocolV1.h"

// The one owner of a CrossX sync session (docs/crossx/architecture.md, "The state machine"). It
// decides what the radio, Wi-Fi, HTTP receiver, and storage may do, and when; they only report back.
// Pure: time comes in through tick(), effects go out through Ports, so the host tests drive it
// with fakes and the firmware wires it to NimBLE, WiFi, and SD.
namespace crossx::sync {

enum class State : uint8_t {
  Idle,            // Radio off. Reading owns the device.
  Advertising,     // The only state that advertises. Powered standby or the sync screen; Wi-Fi off.
  Connected,       // One central is connected; HELLO exchanged or pending. Nothing trusted yet.
  Authenticated,   // AUTH accepted for this connection. Control commands allowed.
  WifiConnecting,  // START_SYNC accepted; station mode joining the saved network.
  TransferReady,   // Wi-Fi up, HTTP receiver listening, waiting for the first upload.
  Transferring,    // At least one upload admitted in this session.
  Finishing,       // The app asked to finish (POST /finish). Transient: Cleanup follows in the same call.
  Cleanup,         // Tearing down HTTP, Wi-Fi, the link. Every session exit passes through here.
  Error,           // Terminal for this session; Cleanup follows after a short hold.
};

const char* stateName(State state);

// Why the coordinator moved, or why it stayed Idle. Reported to the UI and the log; the tests assert on it.
enum class Reason : uint8_t {
  None,
  UserRequest,
  BleConnected,
  BleDisconnected,
  HelloOk,
  AuthOk,
  AuthFailed,
  LockedOut,
  VersionMismatch,
  StartSync,
  CancelSync,
  WifiUp,
  WifiFailed,
  FirstUpload,
  SessionFinished,
  Timeout,
  ReaderBusy,
  StorageFailed,
  LowBattery,
  LowMemory,
  PowerOff,
  CleanupDone,
};

const char* reasonName(Reason reason);

// Bounds. Each one is a guarantee the protocol document makes; changing one is a protocol change.
inline constexpr uint8_t kMaxAuthFailures = 5;
inline constexpr uint32_t kLockoutMs = 60u * 1000u;
inline constexpr uint32_t kConnectedUnauthMs = 15u * 1000u;  // Connected but no AUTH: drop.
inline constexpr uint32_t kAuthIdleMs = 60u * 1000u;         // Authenticated but no command.
inline constexpr uint32_t kWifiConnectMs = 20u * 1000u;
inline constexpr uint32_t kTransferIdleMs = 90u * 1000u;  // No upload traffic: finish the session.
inline constexpr uint32_t kErrorHoldMs = 2000u;
inline constexpr uint32_t kMinCommandIntervalMs = 100u;  // Rate limit for control writes.
inline constexpr uint16_t kMaxFilesPerSession = 64;
inline constexpr uint32_t kMaxSessionBytes = 256u * 1024u * 1024u;
inline constexpr uint8_t kMinBatteryPercentForSync = 10;

// Heap the BLE host takes on the C3, and the one block the reader must still find afterwards. Mirrors
// the page turner's measured numbers (lib/BlePageTurner/src/RadioPolicy.h: kStackBytes,
// kMinimumLargestBlockBytes) without including its headers; a host test pins them equal.
inline constexpr uint32_t kBleHostBytes = 49152;
inline constexpr uint32_t kReaderLargestBlockBytes = 32768;

// What the coordinator asks of the platform. One implementation per board, one fake per test.
//
// Threading: the coordinator has no locking. Every input (the request*/on* calls and tick) must be
// delivered from one owning task; NimBLE, Wi-Fi, and HTTP callbacks queue events to that task instead
// of calling in. Port calls must not call back into the coordinator synchronously and must not block,
// except storeAuthCounter, the one call allowed to wait on NVS.
//
// Advertising: the radio advertises iff state() == Advertising. The coordinator calls
// startAdvertising() on entering that state and stopAdvertising() on leaving it, including when a
// central connects (the stack has usually stopped already, so stopAdvertising must be idempotent). The
// glue must never restart advertising on its own: NimBLE's advertise-on-disconnect stays off.
//
// Links: only connections where the X4 is the peripheral are reported; the page turner's own central
// links are not. The coordinator serves one of them, the handle it accepted in onCentralConnected.
class Ports {
 public:
  virtual ~Ports() = default;
  virtual void startAdvertising() = 0;
  virtual void stopAdvertising() = 0;
  // Drop one link. Its disconnect event may still arrive afterwards; the coordinator ignores it.
  virtual void disconnectCentral(uint16_t connHandle) = 0;
  // Notify a result frame on controlResult. The buffer is valid only for this call.
  virtual void notifyResult(const uint8_t* frame, size_t length) = 0;
  virtual void wifiStart() = 0;
  virtual void wifiStop() = 0;
  virtual void httpStart(const uint8_t (&sessionId)[protocol::kSessionIdBytes]) = 0;
  virtual void httpStop() = 0;
  // Fill `out` with random bytes for the HELLO nonce. False when no entropy is available.
  virtual bool randomNonce(uint8_t (&out)[protocol::kNonceBytes]) = 0;
  // HMAC-SHA256 under the pairing key over `message`. False when the device is not paired.
  virtual bool computeAuthMac(const uint8_t* message, size_t length, uint8_t (&out)[protocol::kMacBytes]) = 0;
  // Last accepted AUTH counter for the paired app; persisted so replay survives a reboot.
  virtual uint32_t loadAuthCounter() = 0;
  // Persist `counter` before AUTH answers Ok. May wait on NVS. False: not persisted, AUTH fails.
  virtual bool storeAuthCounter(uint32_t counter) = 0;
  virtual void onStateChanged(State from, State to, Reason reason) = 0;
};

// Facts the coordinator needs from the rest of the firmware when deciding. Refresh them before
// requestAdvertising() and onCleanupDone(), which decide whether the radio may advertise.
struct Environment {
  bool paired = false;
  bool sdMounted = false;
  bool readerOpen = false;  // A book is open: sync must not start on its own.
  bool wifiCredentialsSaved = false;
  bool bleHostRunning = false;  // The NimBLE host is already up, so starting to advertise costs no new heap.
  uint8_t batteryPercent = 100;
  uint32_t freeHeap = 0;
  uint32_t largestFreeBlock = 0;
  uint16_t pendingReceipts = 0;
};

// The coordinator's answer when the HTTP receiver is about to stream an upload. Anything but Accept
// means: write nothing (docs/crossx/protocol-v1.md maps each value to an HTTP status).
enum class UploadAdmission : uint8_t {
  Accept,          // Reserved against the session budget; onUploadCommitted or onUploadFailed must follow.
  NoSession,       // No session is accepting uploads.
  UploadInFlight,  // One at a time: the receiver has one 4 KB buffer and one SHA-256 context.
  FileLimit,       // Committed + in-flight files would exceed START_SYNC's fileCount.
  ByteLimit,       // Committed + in-flight bytes would exceed START_SYNC's totalBytes.
};

class SyncCoordinator {
 public:
  explicit SyncCoordinator(Ports& ports);

  State state() const { return state_; }
  Reason lastReason() const { return lastReason_; }
  bool authenticated() const { return authDone_; }
  const uint8_t* sessionId() const { return sessionId_; }

  void setEnvironment(const Environment& env) { env_ = env; }

  // --- Inputs from the user and the power manager. ---
  // Sync screen opened, or standby entered. Stays Idle without an SD card (Reason::StorageFailed) or
  // when the BLE host and the reader's block do not both fit in the heap (Reason::LowMemory).
  void requestAdvertising(uint32_t nowMs);
  void requestIdle(uint32_t nowMs);  // Sync screen closed, reading resumed, or power off.

  // --- Inputs from the radio. `connHandle` is the stack's handle for the link. ---
  void onCentralConnected(uint16_t connHandle, uint32_t nowMs);
  void onCentralDisconnected(uint16_t connHandle, uint32_t nowMs);
  // A write to the control characteristic. Replies through Ports::notifyResult.
  void onControlWrite(uint16_t connHandle, const uint8_t* bytes, size_t length, uint32_t nowMs);

  // --- Inputs from Wi-Fi and the HTTP receiver. ---
  void onWifiUp(const uint8_t (&ipv4)[protocol::kIpv4Bytes], uint16_t httpPort, uint32_t nowMs);
  void onWifiFailed(uint32_t nowMs);
  // Before streaming an upload whose Content-Length is `expectedBytes`.
  UploadAdmission onUploadStarted(uint32_t expectedBytes, uint32_t nowMs);
  // Every chunk received for the admitted upload: bytes flowing keep the transfer idle timer back.
  void onUploadProgress(uint32_t nowMs);
  // The admitted upload committed (its reservation is spent) or failed (its reservation is released).
  void onUploadCommitted(uint32_t nowMs);
  void onUploadFailed(uint32_t nowMs);
  void onSessionFinishRequested(uint32_t nowMs);
  void onCleanupDone(uint32_t nowMs);

  // Timers. Call at least every second.
  void tick(uint32_t nowMs);

  // Status for the deviceStatus characteristic and the UI.
  protocol::StatusPayload status() const;

 private:
  void transition(State to, Reason reason, uint32_t nowMs);
  Reason advertisingBlocker() const;
  void releaseLink();
  void linkGone(Reason reason, uint32_t nowMs);
  void teardown();
  void enterCleanup(Reason reason, uint32_t nowMs);
  void fail(Reason reason, uint32_t nowMs);
  void reply(protocol::Opcode request, uint16_t sequence, const uint8_t* payload, size_t length);
  void replyCode(protocol::Opcode request, uint16_t sequence, protocol::Result result);
  bool rateLimited(uint32_t nowMs);

  void handleHello(const protocol::Frame& frame, uint32_t nowMs);
  void handleAuth(const protocol::Frame& frame, uint32_t nowMs);
  void handleGetStatus(const protocol::Frame& frame);
  void handleStartSync(const protocol::Frame& frame, uint32_t nowMs);
  void handleCancelSync(const protocol::Frame& frame, uint32_t nowMs);
  void sendStartSyncResult(protocol::Result result, uint16_t sequence);

  Ports& ports_;
  Environment env_{};
  State state_ = State::Idle;
  Reason lastReason_ = Reason::None;
  uint32_t stateSinceMs_ = 0;
  uint32_t lastCommandMs_ = 0;
  uint32_t lastActivityMs_ = 0;  // Authenticated: last command. Transfer states: last upload event.

  bool linked_ = false;  // connHandle_ is the link this coordinator serves.
  uint16_t connHandle_ = 0;
  bool helloDone_ = false;
  bool authDone_ = false;
  uint8_t deviceNonce_[protocol::kNonceBytes]{};
  uint8_t appNonce_[protocol::kNonceBytes]{};
  uint8_t authFailures_ = 0;
  uint32_t lockedUntilMs_ = 0;

  uint8_t sessionId_[protocol::kSessionIdBytes]{};
  bool sessionActive_ = false;
  uint16_t startSyncSequence_ = 0;  // The newest START_SYNC for this session; the final result answers it.
  uint8_t ipv4_[protocol::kIpv4Bytes]{};
  uint16_t httpPort_ = 0;
  uint16_t declaredFiles_ = 0;
  uint32_t declaredBytes_ = 0;
  uint16_t committedFiles_ = 0;
  uint32_t committedBytes_ = 0;
  bool uploadInFlight_ = false;
  uint32_t inFlightBytes_ = 0;
  bool idleRequested_ = false;
};

}  // namespace crossx::sync
