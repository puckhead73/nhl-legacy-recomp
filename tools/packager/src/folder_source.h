// nhl-legacy-builder - pre-extracted game folder input (e.g. a Xenia-style
// directory already containing default.xex and the game data tree).

#pragma once

#include "game_source.h"

namespace packager {

class FolderSource : public GameSource {
 public:
  bool Open(const std::filesystem::path& input, Error& err) override;
  bool HashDefaultXex(std::string& out_sha256_hex, uint64_t& out_size,
                      Error& err) override;
  bool List(std::vector<GameFile>& out, Error& err) override;
  bool ExtractAll(const std::filesystem::path& dest_dir,
                  const ProgressFn& progress, Error& err) override;
  std::string Describe() const override;

 private:
  std::filesystem::path root_;
  std::filesystem::path xex_path_;  // resolved default.xex (case-insensitive)
};

}  // namespace packager
