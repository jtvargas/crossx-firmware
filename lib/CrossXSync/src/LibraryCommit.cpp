#include "LibraryCommit.h"

#include <cstdio>
#include <cstring>

namespace crossx::storage {
namespace {

bool isLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

bool hexOfLength(const char* s, size_t n) {
  if (s == nullptr) return false;
  for (size_t i = 0; i < n; ++i) {
    if (!isLowerHex(s[i])) return false;
  }
  return s[n] == '\0';
}

bool keepChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

bool joinPath(const char* dir, const char* name, const char* ext, char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return false;
  const int n = snprintf(out, capacity, "%s/%s%s", dir, name, ext);
  if (n < 0 || static_cast<size_t>(n) >= capacity || static_cast<size_t>(n) > kMaxPathChars) {
    out[0] = '\0';
    return false;
  }
  return true;
}

}  // namespace

bool validFileId(const char* fileId) { return hexOfLength(fileId, kFileIdHexChars); }

bool validDigestHex(const char* digest) { return hexOfLength(digest, kDigestHexChars); }

bool stagingPath(const char* fileId, char* out, size_t capacity) {
  if (!validFileId(fileId)) {
    if (out && capacity) out[0] = '\0';
    return false;
  }
  return joinPath(kIncomingDir, fileId, ".part", out, capacity);
}

bool pendingReceiptPath(const char* fileId, char* out, size_t capacity) {
  if (!validFileId(fileId)) {
    if (out && capacity) out[0] = '\0';
    return false;
  }
  return joinPath(kReceiptsDir, fileId, ".pending", out, capacity);
}

bool receiptPath(const char* fileId, char* out, size_t capacity) {
  if (!validFileId(fileId)) {
    if (out && capacity) out[0] = '\0';
    return false;
  }
  return joinPath(kReceiptsDir, fileId, ".meta", out, capacity);
}

size_t sanitizeTitle(const char* title, char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;
  size_t n = 0;
  bool lastWasDash = true;  // Also strips a leading separator.
  const size_t limit = capacity - 1 < kMaxTitleChars ? capacity - 1 : kMaxTitleChars;
  for (const char* p = title ? title : ""; *p && n < limit; ++p) {
    const char c = *p;
    if (keepChar(c)) {
      out[n++] = c;
      lastWasDash = false;
    } else if (!lastWasDash) {
      out[n++] = '-';
      lastWasDash = true;
    }
  }
  while (n > 0 && out[n - 1] == '-') --n;  // Trailing separator
  if (n == 0) {
    static constexpr char kUntitled[] = "Untitled";
    n = sizeof(kUntitled) - 1 < limit ? sizeof(kUntitled) - 1 : limit;
    memcpy(out, kUntitled, n);
  }
  out[n] = '\0';
  return n;
}

bool commitPath(const char* title, const char* fileId, char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return false;
  if (!validFileId(fileId)) {
    out[0] = '\0';
    return false;
  }
  char clean[kMaxTitleChars + 1];
  sanitizeTitle(title, clean, sizeof(clean));
  char name[kMaxTitleChars + 1 + kShortIdChars + 1];
  snprintf(name, sizeof(name), "%s-%.*s", clean, static_cast<int>(kShortIdChars), fileId);
  return joinPath(kBooksDir, name, ".epub", out, capacity);
}

CommitDecision decide(const char* fileId, const char* digestHex, uint32_t expectedLength, bool receiptExists,
                      const char* receiptDigestHex) {
  if (!validFileId(fileId) || !validDigestHex(digestHex) || !acceptableLength(expectedLength)) {
    return CommitDecision::RejectBadRequest;
  }
  if (!receiptExists) return CommitDecision::Accept;
  if (receiptDigestHex != nullptr && strcmp(receiptDigestHex, digestHex) == 0) return CommitDecision::AlreadyCommitted;
  return CommitDecision::RejectDigestChanged;
}

size_t buildReceiptMacMessage(const char* fileId, const char* sha256Hex, uint32_t size, const char* path, char* out,
                              size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;
  out[0] = '\0';
  if (!validFileId(fileId) || !validDigestHex(sha256Hex) || path == nullptr || path[0] == '\0') return 0;
  const int n = snprintf(out, capacity, "%s\n%s\n%lu\n%s", fileId, sha256Hex, static_cast<unsigned long>(size), path);
  if (n < 0 || static_cast<size_t>(n) >= capacity || static_cast<size_t>(n) > kMaxReceiptMacMessageChars) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

LeftoverAction leftoverAction(const char* name) {
  if (name == nullptr) return LeftoverAction::Keep;
  const size_t len = strlen(name);
  static constexpr char kExt[] = ".part";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  if (len != kFileIdHexChars + kExtLen) return LeftoverAction::Keep;
  if (strcmp(name + kFileIdHexChars, kExt) != 0) return LeftoverAction::Keep;
  for (size_t i = 0; i < kFileIdHexChars; ++i) {
    if (!isLowerHex(name[i])) return LeftoverAction::Keep;
  }
  return LeftoverAction::Delete;
}

}  // namespace crossx::storage
