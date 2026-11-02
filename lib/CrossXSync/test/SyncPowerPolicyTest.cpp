#include <gtest/gtest.h>

#include "SyncPowerPolicy.h"

using namespace crossx::power;

namespace {

TEST(SyncPowerPolicy, PowerOffHasNoRadio) {
  constexpr RadioBudget b = budget(Mode::PowerOff, true);
  static_assert(!b.bleAllowed && !b.wifiAllowed && b.advIntervalMs == 0);
  EXPECT_FALSE(b.bleAllowed);
}

TEST(SyncPowerPolicy, ReadingNeverAllowsWifiAndBleOnlyByChoice) {
  constexpr RadioBudget off = budget(Mode::Reading, false);
  constexpr RadioBudget on = budget(Mode::Reading, true);
  static_assert(!off.bleAllowed && !off.wifiAllowed);
  static_assert(on.bleAllowed && !on.wifiAllowed && on.advIntervalMs == kAdvIntervalSlowMs);
  EXPECT_EQ(off.advIntervalMs, 0);
}

TEST(SyncPowerPolicy, StandbyAdvertisesSlowlyAndIsBounded) {
  constexpr RadioBudget b = budget(Mode::AutoSyncStandby, false);
  static_assert(b.bleAllowed && b.wifiAllowed && b.advIntervalMs == kAdvIntervalSlowMs);
  EXPECT_EQ(b.standbyMaxMs, kStandbyMaxMs);
  EXPECT_TRUE(standbyAllowed(Mode::AutoSyncStandby, 50, 10, 0));
  EXPECT_FALSE(standbyAllowed(Mode::AutoSyncStandby, 9, 10, 0));
  EXPECT_FALSE(standbyAllowed(Mode::AutoSyncStandby, 50, 10, kStandbyMaxMs));
  EXPECT_FALSE(standbyAllowed(Mode::Reading, 100, 10, 0));
  EXPECT_FALSE(standbyAllowed(Mode::PowerOff, 100, 10, 0));
}

}  // namespace
