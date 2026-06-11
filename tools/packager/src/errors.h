// nhl-legacy-builder - typed errors with end-user-facing descriptions.

#pragma once

#include <string>

namespace packager {

enum class ErrorCode {
  kNone = 0,
  kInputMissing,      // input path does not exist / not readable
  kNotXdvdfs,         // no XDVDFS magic at any known base offset
  kTruncated,         // image readable but damaged/short
  kNoDefaultXex,      // no default.xex in the source
  kHashMismatch,      // default.xex does not match the supported build
  kBadPayload,        // payload/manifest.toml missing or unreadable
  kDiskFull,          // not enough space at the destination
  kOutDirUnwritable,  // cannot create/write the destination
  kExtractFailed,     // IO error mid-copy; install left incomplete
};

struct Error {
  ErrorCode code = ErrorCode::kNone;
  std::string detail;  // extra context appended to the human message

  explicit operator bool() const { return code != ErrorCode::kNone; }
};

// One-paragraph human message for an error, including remediation hints.
std::string Describe(const Error& err);

}  // namespace packager
