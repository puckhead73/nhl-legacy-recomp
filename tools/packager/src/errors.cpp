#include "errors.h"

namespace packager {

std::string Describe(const Error& err) {
  std::string msg;
  switch (err.code) {
    case ErrorCode::kNone:
      return "";
    case ErrorCode::kInputMissing:
      msg = "The input path does not exist or cannot be read.";
      break;
    case ErrorCode::kNotXdvdfs:
      msg =
          "This file is not a recognized Xbox 360 disc image (no XDVDFS "
          "volume found at any known partition offset). Make sure you "
          "selected a raw .iso dump of the NHL Legacy disc.";
      break;
    case ErrorCode::kTruncated:
      msg =
          "The disc image appears incomplete or damaged. Re-dump the disc "
          "and try again.";
      break;
    case ErrorCode::kNoDefaultXex:
      msg =
          "No default.xex was found at the root of this image - it does not "
          "look like an Xbox 360 game disc for NHL Legacy.";
      break;
    case ErrorCode::kHashMismatch:
      msg =
          "The game executable (default.xex) does not match the supported "
          "vanilla NHL Legacy build. Common causes: a different "
          "region/version of the game, a modified dump, or a disc with a "
          "title update applied. This port only supports the vanilla "
          "image it was recompiled from.";
      break;
    case ErrorCode::kBadPayload:
      msg =
          "The builder's payload directory is missing or damaged "
          "(payload/manifest.toml not readable). Re-extract the release "
          "zip and run the builder from inside that folder.";
      break;
    case ErrorCode::kDiskFull:
      msg = "Not enough free disk space at the install destination.";
      break;
    case ErrorCode::kOutDirUnwritable:
      msg =
          "Cannot write to the install destination (permission denied or "
          "the folder is in use).";
      break;
    case ErrorCode::kExtractFailed:
      msg =
          "Copying game files failed partway through. The install is "
          "incomplete - re-run the builder to retry.";
      break;
  }
  if (!err.detail.empty()) {
    msg += "\n  (";
    msg += err.detail;
    msg += ")";
  }
  return msg;
}

}  // namespace packager
