// nhllegacy - loose-file overlay device.
//
// A union mount: a read/write "upper" layer (the game's writable scratch
// cache) stacked over a read-only "lower" layer (the extracted loose-asset
// tree, `_compiled`, via LooseTreeDevice). Path resolution prefers the upper
// layer and falls back to the lower; creates/writes always land in the upper.
//
// Purpose: overlay the loose extracted assets onto the guest's cache: drive so
// NHL Legacy's native loose-file-first loader (it probes cache:\<logical path>
// before falling back to the .big archive under game:\) serves loose files for
// easy replacement — while the game's own cache writes (e.g. 809284.ver,
// admanager_cache_images/) stay isolated in the scratch dir. The lower layer
// (LooseTreeDevice) additionally synthesizes texture .rx2 from loose .dds
// overrides at load time. This sidesteps repacking the un-reversed,
// LZX-compressed EA "EB\0\3" .big format entirely.

#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/host_path_device.h>

#include "loose_tree_device.h"

namespace nhllegacy {

class UnionDevice : public rex::filesystem::Device {
 public:
  // `override_root` (optional): root of loose .dds texture overrides; forwarded
  // to the lower LooseTreeDevice (see loose_tree_device.h).
  UnionDevice(std::string_view mount_path,
              const std::filesystem::path& writable_host_path,
              const std::filesystem::path& readonly_host_path,
              const std::filesystem::path& override_root = {});
  ~UnionDevice() override;

  bool Initialize() override;
  void Dump(rex::string::StringBuffer* string_buffer) override;
  rex::filesystem::Entry* ResolvePath(std::string_view path) override;

  // True only if the read-only loose-asset (lower) layer initialized. The mount
  // succeeds on the writable upper layer alone, so callers that report "loose files
  // overlaid" must check this rather than assume Initialize() implies the lower layer.
  bool lower_active() const { return lower_active_; }

  // Writes are allowed; they are routed to the upper (scratch) layer.
  bool is_read_only() const override { return false; }

  // Volume/identity metadata mirrors the writable upper layer.
  const std::string& name() const override { return upper_.name(); }
  uint32_t attributes() const override { return upper_.attributes(); }
  uint32_t component_name_max_length() const override {
    return upper_.component_name_max_length();
  }
  uint32_t total_allocation_units() const override {
    return upper_.total_allocation_units();
  }
  uint32_t available_allocation_units() const override {
    return upper_.available_allocation_units();
  }
  uint32_t sectors_per_allocation_unit() const override {
    return upper_.sectors_per_allocation_unit();
  }
  uint32_t bytes_per_sector() const override { return upper_.bytes_per_sector(); }

 private:
  rex::filesystem::HostPathDevice upper_;  // writable scratch (game cache)
  LooseTreeDevice lower_;                   // read-only _compiled + .dds synth
  bool lower_active_ = false;               // did the lower layer initialize?

  // Diagnostic: when the NHL_LOOSE_PROBE env var is set, log each distinct
  // cache:\ path the guest resolves once, tagged by the layer that served it.
  bool probe_ = false;
  std::mutex probe_mu_;
  std::unordered_set<std::string> probe_seen_;
};

}  // namespace nhllegacy
