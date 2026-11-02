#include <gtest/gtest.h>

#include "RadioPolicy.h"
#include "SyncCoordinator.h"

namespace {

// The coordinator's heap gate copies the page turner's measured C3 numbers instead of including its
// headers in the core library; this holds the copy to the original.
TEST(SyncCoordinator, HeapGateMirrorsThePageTurnersC3Numbers) {
  EXPECT_EQ(size_t{crossx::sync::kBleHostBytes}, bleturner::kStackBytes);
  EXPECT_EQ(size_t{crossx::sync::kReaderLargestBlockBytes}, bleturner::kMinimumLargestBlockBytes);
}

}  // namespace
