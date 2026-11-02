#pragma once

#include <cstddef>
#include <cstdint>

// Where an incoming EPUB lives at each step of the SD transaction, and whether a request may
// commit (docs/architecture.md, "Storage transaction"). Pure string and decision rules; the
// firmware's receiver does the I/O.
//
//   /crossx/incoming/<file-id>.part     bytes streamed here while hashing
//   /crossx/receipts/<file-id>.meta     durable receipt written after the commit
//   /Books/CrossX/<title>-<short-id>.epub  the committed book the library sees
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
bool receiptPath(const char* fileId, char* out, size_t capacity);
// Title keeps ASCII letters, digits, space, '-', '_', '.'; other bytes become '-'; runs collapse;
// empty becomes "Untitled". The short id is the first kShortIdChars of the file id.
bool commitPath(const char* title, const char* fileId, char* out, size_t capacity);
// The sanitised title alone, for tests and the UI.
size_t sanitizeTitle(const char* title, char* out, size_t capacity);

// What the receiver should do with an upload for `fileId`, given what the receipt store knows.
enum class CommitDecision : uint8_t {
  Accept,              // No receipt: stream, verify, commit.
  AlreadyCommitted,    // Same id, same digest: answer with the existing receipt, write nothing.
  RejectDigestChanged, // Same id, different digest: refuse; the app must use a new id.
  RejectBadRequest,    // Invalid id, digest, or length.
};

CommitDecision decide(const char* fileId, const char* digestHex, uint32_t expectedLength, bool receiptExists,
                      const char* receiptDigestHex);

// Boot reconciliation: what to do with a leftover in /crossx/incoming.
enum class LeftoverAction : uint8_t {
  Delete,  // A .part with no receipt: the transfer never finished.
  Keep,    // Not ours (unknown name); leave it alone.
};
LeftoverAction leftoverAction(const char* name);

}  // namespace crossx::storage
