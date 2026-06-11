// nhl-legacy-builder - ISO (XDVDFS) input via the SDK's DiscImageDevice.
//
// The device probes all known XGD partition base offsets (0x0, XGD2
// 0xFD90000, XGD3 0x2080000, XGD1 0x18300000), verifies the
// MICROSOFT*XBOX*MEDIA magic and builds the directory tree over a memory
// mapping of the image - it streams, never loading the image into RAM.

#pragma once

#include <memory>

#include "game_source.h"

namespace rex::filesystem {
class DiscImageDevice;
class Entry;
}  // namespace rex::filesystem

namespace packager {

class IsoSource : public GameSource {
 public:
  IsoSource();
  ~IsoSource() override;

  bool Open(const std::filesystem::path& input, Error& err) override;
  bool HashDefaultXex(std::string& out_sha256_hex, uint64_t& out_size,
                      Error& err) override;
  bool List(std::vector<GameFile>& out, Error& err) override;
  bool ExtractAll(const std::filesystem::path& dest_dir,
                  const ProgressFn& progress, Error& err) override;
  std::string Describe() const override;

 private:
  struct Item {
    rex::filesystem::Entry* entry;
    std::string rel_path;
  };
  void CollectFiles(std::vector<Item>& out) const;

  std::filesystem::path iso_path_;
  std::unique_ptr<rex::filesystem::DiscImageDevice> device_;
};

}  // namespace packager
