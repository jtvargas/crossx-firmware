#pragma once

#include <cstdint>

// What the radios may do in each power state (docs/architecture.md, "Power states"). Pure and
// constexpr so the tests can check every cell; the firmware's power manager reads the same table.
//
// Reading comes first: nothing here may cost a page turn. Standby is best effort and its
// battery cost is a measured number (docs/hardware-x4.md), not a promise.
namespace crossx::power {

enum class Mode : uint8_t {
  Reading,          // A book is open or the UI is in use. Wi-Fi off; BLE only if the user allowed it.
  AutoSyncStandby,  // Screen idle, device awake at low clock, advertising. Wi-Fi off until a session.
  PowerOff,         // Deep sleep or hard off. No radio can listen; nothing arrives until wake.
};

struct RadioBudget {
  bool bleAllowed;
  bool wifiAllowed;          // Only inside an authenticated session.
  uint16_t advIntervalMs;    // 0 when BLE is not allowed.
  uint32_t standbyMaxMs;     // How long standby may hold the device awake before PowerOff. 0: n/a.
};

// Advertising intervals. The slow one is for standby, where the app can tolerate a second of
// discovery latency in exchange for radio duty cycle; the fast one is for the sync screen.
inline constexpr uint16_t kAdvIntervalFastMs = 100;
inline constexpr uint16_t kAdvIntervalSlowMs = 1000;
// Standby holds the device awake at most this long without a session, then it powers off.
// Measured standby current decides whether this stays (docs/hardware-x4.md, decision gate A).
inline constexpr uint32_t kStandbyMaxMs = 30u * 60u * 1000u;

constexpr RadioBudget budget(Mode mode, bool userEnabledBleWhileReading) {
  switch (mode) {
    case Mode::Reading:
      return {userEnabledBleWhileReading, false, userEnabledBleWhileReading ? kAdvIntervalSlowMs : uint16_t{0}, 0};
    case Mode::AutoSyncStandby:
      return {true, true, kAdvIntervalSlowMs, kStandbyMaxMs};
    case Mode::PowerOff:
      return {false, false, 0, 0};
  }
  return {false, false, 0, 0};
}

// Whether the device may stay in standby instead of sleeping. Low battery always wins.
constexpr bool standbyAllowed(Mode mode, uint8_t batteryPercent, uint8_t minBatteryPercent, uint32_t standbyForMs) {
  if (mode != Mode::AutoSyncStandby) return false;
  if (batteryPercent < minBatteryPercent) return false;
  return standbyForMs < kStandbyMaxMs;
}

}  // namespace crossx::power
