// nhl-legacy-builder - abstract input source (ISO image or extracted folder).

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "errors.h"

namespace packager {

struct GameFile {
  std::string rel_path;  // forward-slash relative path within the game tree
  uint64_t size = 0;
};

// progress(bytes_done, bytes_total, current_rel_path)
using ProgressFn =
    std::function<void(uint64_t, uint64_t, std::string_view)>;

class GameSource {
 public:
  virtual ~GameSource() = default;

  // Validates and opens the input. All other methods require Open() success.
  virtual bool Open(const std::filesystem::path& input, Error& err) = 0;

  // SHA-256 (lowercase hex) and size of the source's root default.xex.
  virtual bool HashDefaultXex(std::string& out_sha256_hex, uint64_t& out_size,
                              Error& err) = 0;

  // Every file (recursive) with its size, for progress totals + disk checks.
  virtual bool List(std::vector<GameFile>& out, Error& err) = 0;

  // Streams every file into dest_dir, preserving the directory tree.
  virtual bool ExtractAll(const std::filesystem::path& dest_dir,
                          const ProgressFn& progress, Error& err) = 0;

  // One-line description of the opened source (for verify reports).
  virtual std::string Describe() const = 0;
};

}  // namespace packager
