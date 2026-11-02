#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "LibraryCommit.h"

using namespace crossx::storage;

namespace {

constexpr const char* kId = "0123456789abcdef0123456789abcdef";
constexpr const char* kDigest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

TEST(LibraryCommit, FileIdMustBeExactLowercaseHex) {
  EXPECT_TRUE(validFileId(kId));
  EXPECT_FALSE(validFileId("0123456789ABCDEF0123456789abcdef"));
  EXPECT_FALSE(validFileId("0123456789abcdef0123456789abcde"));
  EXPECT_FALSE(validFileId("0123456789abcdef0123456789abcdef0"));
  EXPECT_FALSE(validFileId("../../etc/passwd.part.............."));
  EXPECT_FALSE(validFileId(nullptr));
  EXPECT_FALSE(validFileId(""));
}

TEST(LibraryCommit, PathsAreUnderCrossXDirectories) {
  char out[kMaxPathChars + 1];
  ASSERT_TRUE(stagingPath(kId, out, sizeof(out)));
  EXPECT_STREQ(out, "/crossx/incoming/0123456789abcdef0123456789abcdef.part");
  ASSERT_TRUE(receiptPath(kId, out, sizeof(out)));
  EXPECT_STREQ(out, "/crossx/receipts/0123456789abcdef0123456789abcdef.meta");
  ASSERT_TRUE(commitPath("Deep Work", kId, out, sizeof(out)));
  EXPECT_STREQ(out, "/Books/CrossX/Deep-Work-01234567.epub");
}

TEST(LibraryCommit, InvalidIdNeverYieldsAPath) {
  char out[kMaxPathChars + 1] = "junk";
  EXPECT_FALSE(stagingPath("../x", out, sizeof(out)));
  EXPECT_STREQ(out, "");
  strcpy(out, "junk");
  EXPECT_FALSE(commitPath("Title", "nope", out, sizeof(out)));
  EXPECT_STREQ(out, "");
}

TEST(LibraryCommit, SmallBufferFails) {
  char out[10];
  EXPECT_FALSE(stagingPath(kId, out, sizeof(out)));
  EXPECT_STREQ(out, "");
}

TEST(LibraryCommit, TitleSanitising) {
  char out[kMaxTitleChars + 1];
  sanitizeTitle("  The/Quick\\Brown:Fox*?  ", out, sizeof(out));
  EXPECT_STREQ(out, "The-Quick-Brown-Fox");
  sanitizeTitle("Ünïcödé — title", out, sizeof(out));
  EXPECT_STREQ(out, "n-c-d-title");
  sanitizeTitle("", out, sizeof(out));
  EXPECT_STREQ(out, "Untitled");
  sanitizeTitle(nullptr, out, sizeof(out));
  EXPECT_STREQ(out, "Untitled");
  sanitizeTitle("...", out, sizeof(out));
  EXPECT_STREQ(out, "Untitled");
  const std::string longTitle(500, 'a');
  EXPECT_EQ(sanitizeTitle(longTitle.c_str(), out, sizeof(out)), kMaxTitleChars);
  EXPECT_EQ(strlen(out), kMaxTitleChars);
}

TEST(LibraryCommit, LongTitleStillFitsPath) {
  char out[kMaxPathChars + 1];
  const std::string longTitle(500, 'Z');
  ASSERT_TRUE(commitPath(longTitle.c_str(), kId, out, sizeof(out)));
  EXPECT_LE(strlen(out), kMaxPathChars);
  EXPECT_EQ(std::string(out).find("/Books/CrossX/"), 0u);
  EXPECT_NE(std::string(out).find("-01234567.epub"), std::string::npos);
}

TEST(LibraryCommit, DecisionTable) {
  EXPECT_EQ(decide(kId, kDigest, 10000, false, nullptr), CommitDecision::Accept);
  EXPECT_EQ(decide(kId, kDigest, 10000, true, kDigest), CommitDecision::AlreadyCommitted);
  const std::string other = std::string("f") + (kDigest + 1);
  EXPECT_EQ(decide(kId, kDigest, 10000, true, other.c_str()), CommitDecision::RejectDigestChanged);
  EXPECT_EQ(decide(kId, kDigest, 10000, true, nullptr), CommitDecision::RejectDigestChanged);
  EXPECT_EQ(decide("bad", kDigest, 10000, false, nullptr), CommitDecision::RejectBadRequest);
  EXPECT_EQ(decide(kId, "bad", 10000, false, nullptr), CommitDecision::RejectBadRequest);
  EXPECT_EQ(decide(kId, kDigest, kMinEpubBytes - 1, false, nullptr), CommitDecision::RejectBadRequest);
  EXPECT_EQ(decide(kId, kDigest, kMaxEpubBytes + 1, false, nullptr), CommitDecision::RejectBadRequest);
  EXPECT_EQ(decide(kId, kDigest, kMaxEpubBytes, false, nullptr), CommitDecision::Accept);
}

TEST(LibraryCommit, LeftoverReconciliationOnlyTouchesOurParts) {
  EXPECT_EQ(leftoverAction("0123456789abcdef0123456789abcdef.part"), LeftoverAction::Delete);
  EXPECT_EQ(leftoverAction("0123456789abcdef0123456789abcdef.epub"), LeftoverAction::Keep);
  EXPECT_EQ(leftoverAction("notes.txt"), LeftoverAction::Keep);
  EXPECT_EQ(leftoverAction("0123456789ABCDEF0123456789abcdef.part"), LeftoverAction::Keep);
  EXPECT_EQ(leftoverAction(nullptr), LeftoverAction::Keep);
}

}  // namespace
