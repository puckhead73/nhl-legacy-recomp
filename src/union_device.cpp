// nhllegacy - loose-file overlay device. See union_device.h.

#include "union_device.h"

#include <cstdlib>
#include <string>

#include <rex/logging.h>

namespace nhllegacy {

namespace fs = rex::filesystem;

UnionDevice::UnionDevice(std::string_view mount_path,
                         const std::filesystem::path& writable_host_path,
                         const std::filesystem::path& readonly_host_path,
                         const std::filesystem::path& override_root)
    : fs::Device(mount_path),
      upper_(mount_path, writable_host_path, /*read_only=*/false),
      lower_(mount_path, readonly_host_path, override_root),
      probe_(std::getenv("NHL_LOOSE_PROBE") != nullptr) {}

UnionDevice::~UnionDevice() = default;

bool UnionDevice::Initialize() {
  // The writable scratch layer is required (the guest probes/writes cache: on
  // boot). The loose-asset layer is best-effort: if it fails to initialize we
  // still want a functional writable cache rather than a failed mount.
  if (!upper_.Initialize()) {
    return false;
  }
  lower_active_ = lower_.Initialize();
  return true;
}

void UnionDevice::Dump(rex::string::StringBuffer* string_buffer) {
  upper_.Dump(string_buffer);
}

fs::Entry* UnionDevice::ResolvePath(std::string_view path) {
  // Upper (scratch) wins so the game sees its own writes; otherwise fall
  // through to the read-only loose tree. Resolving a parent directory for a
  // create returns the writable upper entry, so new files land in scratch.
  //
  // Note: the guest opens most asset files RELATIVE to a directory handle, so
  // those go through Entry::GetChild on the resolved directory entry rather
  // than here — which is why per-file .rx2 synth lives in LooseTreeDevice's
  // entry tree, not in this method.
  fs::Entry* upper = upper_.ResolvePath(path);
  fs::Entry* result = upper ? upper : lower_.ResolvePath(path);

  if (probe_) {
    const char* layer = upper ? "scratch" : (result ? "loose  " : "miss   ");
    std::string key(path);
    std::lock_guard<std::mutex> guard(probe_mu_);
    if (probe_seen_.insert(key).second) {
      REXLOG_INFO("[loose-probe] {} {}", layer, key);
    }
  }
  return result;
}

}  // namespace nhllegacy
