// nhllegacy - read-only device over the extracted `_compiled` asset tree, with
// per-file runtime .dds -> .rx2 texture synthesis.
//
// Why this exists (not just a HostPathDevice): the guest opens texture .rx2
// files RELATIVE to a directory handle (it opens cache:\rendering\<cat> once,
// then opens files inside it). Those file opens resolve through Entry::GetChild
// on the directory entry — NOT through Device::ResolvePath. So a synth injected
// at ResolvePath is bypassed. Here the synth lives in the ENTRY TREE: a
// directory entry's child for an overridden <name>.rx2 IS a synth entry, so
// GetChild (and full-path ResolvePath) both reach it.
//
// For a texture <name>.rx2 with loose .dds under
// `override_root/<rel>/<slot>.dds`, Open() converts it to a game-valid .rx2 via
// tdb-rx2-ffi (original .rx2 as template). Every other file streams from disk.

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>

namespace nhllegacy {

class LooseTreeDevice : public rex::filesystem::Device {
 public:
  LooseTreeDevice(std::string_view mount_path,
                  const std::filesystem::path& compiled_root,
                  const std::filesystem::path& override_root);
  ~LooseTreeDevice() override;

  bool Initialize() override;
  void Dump(rex::string::StringBuffer* string_buffer) override;
  rex::filesystem::Entry* ResolvePath(std::string_view path) override;

  bool is_read_only() const override { return true; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  // Build a synthesized .rx2 for the entry at relative path `rel` (original
  // template at `original`) from loose .dds overrides, or nullptr if there is
  // no override (caller serves the original file). Not internally locked —
  // callers serialize via synth_mutex().
  std::shared_ptr<const std::vector<uint8_t>> BuildSynth(
      const std::string& rel, const std::filesystem::path& original);

  const std::filesystem::path& override_root() const { return override_root_; }
  std::mutex& synth_mutex() { return synth_mu_; }

 private:
  std::string name_;
  std::filesystem::path compiled_root_;
  std::filesystem::path override_root_;
  std::unique_ptr<rex::filesystem::Entry> root_;
  std::mutex synth_mu_;
};

}  // namespace nhllegacy
