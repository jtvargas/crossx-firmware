#pragma once

#include <cstddef>
#include <cstdint>

// Where an incoming EPUB lives at each step of the SD transaction, and whether a request may
// commit (docs/crossx/architecture.md, "Storage transaction"). Pure string and decision rules; the
// firmware's receiver does the I/O, in this order:
//
//   1. /crossx/incoming/<file-id>.part        bytes streamed here while hashing, then verified
//   2. /crossx/receipts/<file-id>.pending     the receipt, written and flushed before the commit
//   3. /Books/CrossX/<title>-<short-id>.epub  the .part renamed: the committed book the library sees
//   4. /crossx/receipts/<file-id>.meta        the .pending renamed: the durable receipt
namespace crossx::storage {

inline constexpr size_t kFileIdHexChars = 32;  // 16 bytes, lowercase hex
inline constexpr size_t kDigestHexChars = 64;  // SHA-256
inline constexpr size_t kShortIdChars = 8;     // Leading hex of the file id, enough to tell twins apart
inline constexpr size_t kMaxTitleChars = 64;   // After sanitising; keeps the path under FAT limits
inline constexpr size_t kMaxPathChars = 160;
inline constexpr uint32_t kMaxEpubBytes = 64u * 1024u * 1024u;
inline constexpr uint32_t kMinEpubBytes = 1024;  // A ZIP with a mimetype entry is already bigger.

inline constexpr const char* kIncomingDir = "/crossx/incoming";
inline constexpr const char* kReceiptsDir = "/crossx/receipts";
inline constexpr const char* kBooksDir = "/Books/CrossX";

// True for exactly kFileIdHexChars lowercase hex characters (the app must not send anything else).
bool validFileId(const char* fileId);
bool validDigestHex(const char* digest);
// A length the receiver is willing to stream to SD.
constexpr bool acceptableLength(uint32_t bytes) { return bytes >= kMinEpubBytes && bytes <= kMaxEpubBytes; }

// Writes the path into `out`; returns false (and empties `out`) when inputs are invalid or it
// would not fit. `capacity` includes the terminating NUL.
bool stagingPath(const char* fileId, char* out, size_t capacity);
bool pendingReceiptPath(const char* fileId, char* out, size_t capacity);
bool receiptPath(const char* fileId, char* out, size_t capacity);
// The short id is the first kShortIdChars of the file id.
bool commitPath(const char* title, const char* fileId, char* out, size_t capacity);
// The title as it appears in the commit path: ASCII letters, digits, and '_' are kept; every other
// byte (space, '.', '-', punctuation, any non-ASCII byte) is a separator. A run of separators becomes
// one '-', with none at either end. At most kMaxTitleChars; empty becomes "Untitled".
size_t sanitizeTitle(const char* title, char* out, size_t capacity);

// What the receiver should do with an upload for `fileId`, given what the receipt store knows.
enum class CommitDecision : uint8_t {
  Accept,               // No receipt: stream, verify, commit.
  AlreadyCommitted,     // Same id, same digest: answer with the existing receipt, write nothing.
  RejectDigestChanged,  // Same id, different digest: refuse; the app must use a new id.
  RejectBadRequest,     // Invalid id, digest, or length.
};

CommitDecision decide(const char* fileId, const char* digestHex, uint32_t expectedLength, bool receiptExists,
                      const char* receiptDigestHex);

// The bytes the receipt's "mac" covers: fileId "\n" sha256 "\n" size (decimal) "\n" path. The MAC is
// HMAC-SHA256 under the session key, computed by the platform layer. Returns the length written, or 0
// (and empties `out`) when an input is invalid or it does not fit.
inline constexpr size_t kMaxReceiptMacMessageChars = kFileIdHexChars + 1 + kDigestHexChars + 1 + 10 + 1 + kMaxPathChars;
size_t buildReceiptMacMessage(const char* fileId, const char* sha256Hex, uint32_t size, const char* path, char* out,
                              size_t capacity);

// Boot reconciliation, /crossx/incoming: what to do with a leftover there.
enum class LeftoverAction : uint8_t {
  Delete,  // A well-formed <file-id>.part: an upload that never committed (a commit renames it away).
  Keep,    // Not ours (unknown name); leave it alone.
};
LeftoverAction leftoverAction(const char* name);

// Boot reconciliation, /crossx/receipts/<file-id>.pending: the power failed between steps 2 and 4.
enum class PendingAction : uint8_t {
  Promote,  // The book at the receipt's path exists: the rename landed; rename .pending to .meta.
  Discard,  // No book: the commit never happened; delete .pending and let the app upload again.
};
constexpr PendingAction pendingReceiptAction(bool bookExists) {
  return bookExists ? PendingAction::Promote : PendingAction::Discard;
}

}  // namespace crossx::storage
